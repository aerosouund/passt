// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright Red Hat
 * Author: Ammar Yasser <aerosound161@gmail.com>
 *
 * vhost.c - vhost-net (vhost-kernel) acceleration for pasta mode
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>

#include "util.h"
#include "passt.h"
#include "vhost.h"
#include "epoll_ctl.h"
#include "udp.h"
#include "tcp_buf.h" 

struct vq_state vhost_vq_state[2];

struct vring_desc_pasta vring_desc[2][VHOST_NDESCS]
				__attribute__((aligned(PAGE_SIZE)));

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
union vring_avail_u vring_avail_all[2] __attribute__((aligned(PAGE_SIZE)));
union vring_used_u vring_used_all[2] __attribute__((aligned(PAGE_SIZE)));
#pragma GCC diagnostic pop

union vhost_memory_u vhost_memory = {
	.mem = {
		.nregions = N_VHOST_REGIONS,
	},
};

/**
 * vhost_setup_net() - Open and negotiate features on /dev/vhost-net
 * @c:		Execution context; c->vhost.fd and c->vhost.features are set
 *		on success, and left untouched on failure
 *
 * Failure here means vhost-net isn't usable, not that pasta can't run: the
 * caller falls back to plain tap operation unless acceleration was required.
 * That's why nothing in here is fatal.
 *
 * Return: 0 on success, -1 if vhost-net is unavailable or unusable
 */
int vhost_setup_net(struct ctx *c)
{
	const uint64_t req_features = (1ULL << VIRTIO_F_VERSION_1) |
				      (1ULL << VHOST_NET_F_VIRTIO_NET_HDR);
	uint64_t features;
	int vhost_fd;

	vhost_fd = open("/dev/vhost-net", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (vhost_fd < 0) {
		debug_perror("Couldn't open /dev/vhost-net");
		return -1;
	}

	if (ioctl(vhost_fd, VHOST_SET_OWNER, NULL) < 0) {
		debug_perror("VHOST_SET_OWNER ioctl failed");
		goto close_fd;
	}

	if (ioctl(vhost_fd, VHOST_GET_FEATURES, &features) < 0) {
		debug_perror("VHOST_GET_FEATURES ioctl failed");
		goto close_fd;
	}

	if ((features & req_features) != req_features) {
		debug("vhost-net is missing features: 0x%016" PRIx64,
		      req_features & ~features);
		goto close_fd;
	}

	features = req_features;
	if (ioctl(vhost_fd, VHOST_SET_FEATURES, &features) < 0) {
		debug_perror("VHOST_SET_FEATURES ioctl failed");
		goto close_fd;
	}

	c->vhost.features = features;
	c->vhost.fd = vhost_fd;

	return 0;

close_fd:
	close(vhost_fd);

	return -1;
}

/**
 * vhost_setup_eventfds() - Set up one queue's eventfds and ring size
 * @c:		Execution context; c->vhost.fd must already be set
 * @queue_idx:	Index of the queue (vring) to configure
 *
 */
void vhost_setup_eventfds(struct ctx *c, int queue_idx)
{
	struct vhost_vring_file vring_file = { .index = queue_idx };
	struct vhost_vring_state state = { .index = queue_idx };
	union epoll_ref ref = { 0 };
	int vhost_fd = c->vhost.fd;
	int rc;

	state.num = VHOST_NDESCS;
	ref.queue = queue_idx;

	vring_file.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (vring_file.fd < 0)
		die_perror("Failed to create vhost call eventfd, queue %d",
			   queue_idx);
	ref.fd = vring_file.fd;

	rc = ioctl(vhost_fd, VHOST_SET_VRING_CALL, &vring_file);
	if (rc < 0)
		die_perror("VHOST_SET_VRING_CALL ioctl failed, queue %d",
			   queue_idx);

	ref.type = EPOLL_TYPE_VHOST_CALL;
	rc = epoll_add(c->epollfd, EPOLLIN, ref);
	if (rc < 0)
		die_perror("Failed to watch vhost call eventfd, queue %d",
			   queue_idx);
	c->vhost.vq[queue_idx].call_fd = vring_file.fd;

	vring_file.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (vring_file.fd < 0)
		die_perror("Failed to create vhost error eventfd, queue %d",
			   queue_idx);

	rc = ioctl(vhost_fd, VHOST_SET_VRING_ERR, &vring_file);
	if (rc < 0)
		die_perror("VHOST_SET_VRING_ERR ioctl failed, queue %d",
			   queue_idx);

	ref.type = EPOLL_TYPE_VHOST_ERROR;
	ref.fd = vring_file.fd;
	rc = epoll_add(c->epollfd, EPOLLIN, ref);
	if (rc < 0)
		die_perror("Failed to watch vhost error eventfd, queue %d",
			   queue_idx);
	c->vhost.vq[queue_idx].err_fd = vring_file.fd;

	rc = ioctl(vhost_fd, VHOST_SET_VRING_NUM, &state);
	if (rc < 0)
		die_perror("VHOST_SET_VRING_NUM ioctl failed, queue %d",
			   queue_idx);

	vring_file.fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (vring_file.fd < 0)
		die_perror("Failed to create vhost kick eventfd, queue %d",
			   queue_idx);

	rc = ioctl(vhost_fd, VHOST_SET_VRING_KICK, &vring_file);
	if (rc < 0)
		die_perror("VHOST_SET_VRING_KICK ioctl failed, queue %d",
			   queue_idx);

	c->vhost.vq[queue_idx].kick_fd = vring_file.fd;

	vhost_vq_state[queue_idx].num_free = VHOST_NDESCS;
}

/**
 * vhost_setup_memory_table() - Register the GPA/HVA translation table
 * @c:		Execution context; c->vhost.fd must already be set
 *
 * vhost-net reads the addresses we put in descriptors as guest physical
 * addresses, and translates them through this table before touching the
 * memory they refer to. pasta has no guest and no second address space:
 * the addresses we put there are our own virtual addresses. Every region
 * below therefore sets guest_phys_addr equal to userspace_addr, so that
 * GPA == HVA and the translation is an identity mapping.
 *
 * Return: 0 on success, -1 on error with errno set
 */
int vhost_setup_memory_table(struct ctx *c) {
	size_t region_idx = 0;

	/* general purpose buffers */ 
	general_register_memory_regions(&vhost_memory, &region_idx);

	/* tcp specific buffers */
	tcp_register_memory_regions(&vhost_memory, &region_idx);

	/* udp specific buffers */
	udp_register_memory_regions(&vhost_memory, &region_idx);

	vhost_memory.mem.nregions = region_idx;

	return ioctl(c->vhost.fd, VHOST_SET_MEM_TABLE, &vhost_memory.mem);
}

/**
 * vhost_set_vring() - Register a vring's addresses and bind its backend
 * @c:		Execution context; c->vhost.fd must already be set
 * @queue_idx:	Index of the queue (vring) to configure
 * @tap_fd:	Tap fd to bind as this queue's backend
 */
void vhost_set_vring(struct ctx *c, int queue_idx, int tap_fd)
{
	int vhost_fd = c->vhost.fd;
	struct vhost_vring_addr addr = {
		.index = queue_idx,
		.desc_user_addr = (unsigned long)vring_desc[queue_idx],
		.avail_user_addr = (unsigned long)&vring_avail_all[queue_idx],
		.used_user_addr = (unsigned long)&vring_used_all[queue_idx],
		.log_guest_addr = (unsigned long)&vring_used_all[queue_idx],
	};
	struct vhost_vring_file file = {
		.index = queue_idx,
		.fd = tap_fd,
	};
	unsigned int i;
	int rc;

	rc = ioctl(vhost_fd, VHOST_SET_VRING_ADDR, &addr);
	if (rc < 0)
		die_perror("VHOST_SET_VRING_ADDR ioctl failed, queue %d",
			   queue_idx);

	if (queue_idx == VHOST_AVAIL_Q_IDX) {
		for (i = 0; i < VHOST_NDESCS; ++i) {
			vring_desc[VHOST_AVAIL_Q_IDX][i].addr =
				(uintptr_t)pkt_buf + i * VHOST_DESC_BYTES;
			vring_desc[VHOST_AVAIL_Q_IDX][i].len = VHOST_DESC_BYTES;
			vring_desc[VHOST_AVAIL_Q_IDX][i].flags =
				VRING_DESC_F_WRITE;
		}

		for (i = 0; i < VHOST_NDESCS; ++i)
			vring_avail_all[VHOST_AVAIL_Q_IDX].avail.ring[i] =
				htole16(i);

		vhost_rx_descriptor_handoff(c);
	}

	if (queue_idx == VHOST_USED_Q_IDX) {
		for (i = 0; i < (VHOST_NDESCS - 1); ++i)
			vring_desc[VHOST_USED_Q_IDX][i].next = i + 1;
	}

	rc = ioctl(vhost_fd, VHOST_NET_SET_BACKEND, &file);
	if (rc < 0)
		die_perror("VHOST_NET_SET_BACKEND ioctl failed, queue %d",
			   queue_idx);
}

/**
 * vhost_rx_descriptor_handoff() - Announce freed from-guest descriptors
 * @c:		Execution context
 *
 * Bumps avail.idx by the number of descriptors accumulated in
 * vhost_vq_state[VHOST_AVAIL_Q_IDX].num_free (from prior
 * consume_one_rx_descriptor() calls),
 * then resets the counter to zero. The kernel will see the new
 * avail.idx and consume the freshly-available descriptors.
 */
void vhost_rx_descriptor_handoff(struct ctx *c)
{
	smp_wmb();

	if (!vhost_vq_state[VHOST_AVAIL_Q_IDX].num_free)
		return;

	vring_avail_all[VHOST_AVAIL_Q_IDX].avail.idx +=
		vhost_vq_state[VHOST_AVAIL_Q_IDX].num_free;
	vhost_vq_state[VHOST_AVAIL_Q_IDX].num_free = 0;
	vhost_kick(&vring_used_all[VHOST_AVAIL_Q_IDX].used,
		   c->vhost.vq[VHOST_AVAIL_Q_IDX].kick_fd);
}

/**
 * vhost_kick() - Notify the kernel that new descriptors are available
 * @used:	Used ring of the queue we're announcing on, checked to see
 *		whether the kernel's virtio thread is already reading
 *		descriptors and doesn't want to be notified
 * @kick_fd:	Kick eventfd of that same queue
 */
void vhost_kick(struct vring_used_pasta *used, int kick_fd)
{
	/* Ensure that the read of used->flags doesn't get reordered to be
	 * above the avail.idx update
	 */
	smp_mb();

	if (!(used->flags & VRING_USED_F_NO_NOTIFY))
		eventfd_write(kick_fd, 1);
}
