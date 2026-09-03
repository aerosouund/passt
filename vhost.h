/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright Red Hat
 * Author: Ammar Yasser <aerosound161@gmail.com>
 *
 * vhost.h - vhost-net (vhost-kernel) acceleration for pasta mode
 */

#ifndef VHOST_H
#define VHOST_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/vhost.h>
#include <linux/vhost_types.h>
#include <linux/virtio_config.h>
#include <linux/virtio_net.h>
#include <linux/virtio_ring.h>

#include "passt.h"

#define VHOST_AVAIL_Q_IDX 0
#define VHOST_USED_Q_IDX 1

/**
 * struct vq_state - Per-virtqueue local descriptor tracking
 * @num_free:		Number of descriptors ready to be announced to the
 *			kernel via vhost_rx_descriptor_handoff()
 * @last_used_idx:	Number of used-ring entries consumed so far;
 *			lagging read cursor vs. vring_used->idx (the
 *			kernel's write cursor)
 */
extern struct vq_state {
	uint16_t num_free;
	uint16_t last_used_idx;
	uint16_t next_free;
} vhost_vq_state[2];

struct vring_desc_pasta {
	__virtio64 addr;
	__virtio32 len;
	__virtio16 flags;
	__virtio16 next;
};

extern struct vring_desc_pasta vring_desc[2][VHOST_NDESCS];

struct vring_avail_pasta {
	__virtio16 flags;
	__virtio16 idx;
	__virtio16 ring[];
};

union vring_avail_u {
	struct vring_avail_pasta avail;
	char buf[offsetof(struct vring_avail, ring[VHOST_NDESCS])];
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
extern union vring_avail_u vring_avail_all[2];
#pragma GCC diagnostic pop

struct vring_used_pasta {
	__virtio16 flags;
	__virtio16 idx;
	vring_used_elem_t ring[];
};

union vring_used_u {
	struct vring_used_pasta used;
	char buf[offsetof(struct vring_used, ring[VHOST_NDESCS])];
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
extern union vring_used_u vring_used_all[2];
#pragma GCC diagnostic pop

#define N_VHOST_REGIONS 12
union vhost_memory_u {
	struct vhost_memory mem;
	char buf[offsetof(struct vhost_memory, regions[N_VHOST_REGIONS])];
};
extern union vhost_memory_u vhost_memory;

#define VHOST_MEMORY_REGION_PTR(addr, size) \
    (struct vhost_memory_region) { \
        .guest_phys_addr = (uintptr_t)addr, \
        .memory_size = size, \
        .userspace_addr  = (uintptr_t)addr, \
    }
#define VHOST_MEMORY_REGION(buf) VHOST_MEMORY_REGION_PTR(&buf, sizeof(buf))

void vhost_set_vring(struct ctx *c, int queue_idx, int tap_fd);
int vhost_setup_memory_table(struct ctx *c);
int vhost_setup_net(struct ctx *c);
void vhost_setup_eventfds(struct ctx *c, int queue_idx);
void vhost_rx_descriptor_handoff(struct ctx *c);
void vhost_kick(struct vring_used_pasta *used, int kick_fd);

#endif /* VHOST_H */
