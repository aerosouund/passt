// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * virtio API, vring and virtqueue functions definition
 *
 * Copyright Red Hat
 * Author: Laurent Vivier <lvivier@redhat.com>
 */

#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdbool.h>
#include <stddef.h>
#include <linux/vhost_types.h>

struct ctx;

/* Maximum size of a virtqueue */
#define VIRTQUEUE_MAX_SIZE 1024

#define VNET_HLEN	(sizeof(struct virtio_net_hdr_mrg_rxbuf))

/* Keep in sync with PKT_BUF_BYTES in passt.h */
#define PKT_BUF_BYTES		(8UL << 20)

#define VHOST_NDESCS (PKT_BUF_BYTES / 65520)
// static_assert(!(VHOST_NDESCS & (VHOST_NDESCS - 1)),
// 			 "Number of vhost descs must be a power of two by standard");


#define VIRTQUEUE_MAX_SIZE 1024

/* (ammar): where is this used ? */
#define FRAGMENT_MSG_RATE	10  /* # seconds between fragment warnings */

#define VHOST_VIRTIO         0xAF
#define VHOST_GET_FEATURES   _IOR(VHOST_VIRTIO, 0x00, __u64)
#define VHOST_SET_FEATURES   _IOW(VHOST_VIRTIO, 0x00, __u64)
#define VHOST_SET_OWNER	     _IO(VHOST_VIRTIO, 0x01)
#define VHOST_SET_MEM_TABLE  _IOW(VHOST_VIRTIO, 0x03, struct vhost_memory)
#define VHOST_SET_VRING_NUM  _IOW(VHOST_VIRTIO, 0x10, struct vhost_vring_state)
#define VHOST_SET_VRING_ADDR _IOW(VHOST_VIRTIO, 0x11, struct vhost_vring_addr)
#define VHOST_SET_VRING_KICK _IOW(VHOST_VIRTIO, 0x20, struct vhost_vring_file)
#define VHOST_SET_VRING_CALL _IOW(VHOST_VIRTIO, 0x21, struct vhost_vring_file)
#define VHOST_SET_VRING_ERR  _IOW(VHOST_VIRTIO, 0x22, struct vhost_vring_file)
#define VHOST_SET_BACKEND_FEATURES _IOW(VHOST_VIRTIO, 0x25, __u64)
#define VHOST_NET_SET_BACKEND _IOW(VHOST_VIRTIO, 0x30, struct vhost_vring_file)

// (ammar) why do we need here a struct to keep track of the free descriptors
// then later add them to the available descriptors on the variable vring_avail_0
extern struct vq_state {
	/* Number of free descriptors */
	uint16_t num_free;

	/* Last used idx processed */
	uint16_t last_used_idx;
} vqs[2];


// don't use zero and one. use tx and rx from passt perspective
extern struct vring_desc vring_desc[2][VHOST_NDESCS];
union vring_avail_u {
	struct vring_avail avail;
	char buf[offsetof(struct vring_avail, ring[VHOST_NDESCS])];
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
extern union vring_avail_u vring_avail_all[2];
#pragma GCC diagnostic pop

union vring_used_u {
	struct vring_used used;
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

/**
 * struct vu_ring - Virtqueue rings
 * @num:		Size of the queue
 * @desc:		Descriptor ring
 * @avail:		Available ring
 * @used:		Used ring
 * @log_guest_addr:	Guest address for logging
 * @flags:		Vring flags
 * 			VHOST_VRING_F_LOG is set if log address is valid
 */
struct vu_ring {
	unsigned int num;
	struct vring_desc *desc;
	struct vring_avail *avail;
	struct vring_used *used;
	uint64_t log_guest_addr;
	uint32_t flags;
};

/**
 * struct vu_virtq - Virtqueue definition
 * @vring:			Virtqueue rings
 * @last_avail_idx:		Next head to pop
 * @shadow_avail_idx:		Last avail_idx read from VQ.
 * @used_idx:			Descriptor ring current index
 * @signalled_used:		Last used index value we have signalled on
 * @signalled_used_valid:	True if signalled_used if valid
 * @notification:		True if the queues notify (via event
 * 				index or interrupt)
 * @inuse:			Number of entries in use
 * @call_fd:			The event file descriptor to signal when
 * 				buffers are used.
 * @kick_fd:			The event file descriptor for adding
 * 				buffers to the vring
 * @err_fd:			The event file descriptor to signal when
 * 				error occurs
 * @enable:			True if the virtqueue is enabled
 * @started:			True if the virtqueue is started
 * @vra:			QEMU address of our rings
 */
struct vu_virtq {
	struct vu_ring vring;
	uint16_t last_avail_idx;
	uint16_t shadow_avail_idx;
	uint16_t used_idx;
	uint16_t signalled_used;
	bool signalled_used_valid;
	bool notification;
	unsigned int inuse;
	int call_fd;
	int kick_fd;
	int err_fd;
	unsigned int enable;
	bool started;
	struct vhost_vring_addr vra;
};

/**
 * struct vu_dev_region - guest shared memory region
 * @gpa:		Guest physical address of the region
 * @size:		Memory size in bytes
 * @qva:		QEMU virtual address
 * @mmap_offset:	Offset where the region starts in the mapped memory
 * @mmap_addr:		Address of the mapped memory
 */
struct vu_dev_region {
	uint64_t gpa;
	uint64_t size;
	uint64_t qva;
	uint64_t mmap_offset;
	uint64_t mmap_addr;
};

#define VHOST_USER_MAX_VQS 2U

/*
 * Set a reasonable maximum number of ram slots, which will be supported by
 * any architecture.
 */
#define VHOST_USER_MAX_RAM_SLOTS 32

/**
 * struct vdev_memory - Describes the shared memory regions for a vhost-user
 * 			device
 * @nregions:		Number of shared memory regions
 * @regions:		Guest shared memory regions
 */
struct vdev_memory {
	uint32_t nregions;
	struct vu_dev_region regions[VHOST_USER_MAX_RAM_SLOTS];
};

/**
 * struct vu_dev - vhost-user device information
 * @context:			Execution context
 * @memory:			Shared memory regions
 * @vq:				Virtqueues of the device
 * @features:			Vhost-user features
 * @protocol_features:		Vhost-user protocol features
 * @log_call_fd:		Eventfd to report logging update
 * @log_size:			Size of the logging memory region
 * @log_table:			Base of the logging memory region
 */
struct vu_dev {
	struct ctx *context;
	struct vdev_memory memory;
	struct vu_virtq vq[VHOST_USER_MAX_VQS];
	uint64_t features;
	uint64_t protocol_features;
	int log_call_fd;
	uint64_t log_size;
	uint8_t *log_table;
};

/**
 * struct vu_virtq_element - virtqueue element
 * @index:	Descriptor ring index
 * @out_num:	Number of outgoing iovec buffers
 * @in_num:	Number of incoming iovec buffers
 * @in_sg:	Incoming iovec buffers
 * @out_sg:	Outgoing iovec buffers
 */
struct vu_virtq_element {
	unsigned int index;
	unsigned int out_num;
	unsigned int in_num;
	struct iovec *in_sg;
	struct iovec *out_sg;
};

enum set_vring_err {
	VRING_SETUP_OK = 0,
	VRING_SETUP_ERR_SET_ADDR,
	VRING_SETUP_ERR_SET_BACKEND,
};

enum vhost_setup_err {
	VHOST_SETUP_OK = 0,
	VHOST_SETUP_ERR_OPEN,
	VHOST_SETUP_ERR_SET_OWNER,
	VHOST_SETUP_ERR_GET_FEATURES,
	VHOST_SETUP_ERR_MISSING_FEATURES,
	VHOST_SETUP_ERR_SET_FEATURES,
};

enum eventfd_setup_err {
	EVENTFD_SETUP_OK = 0,
	EVENTFD_SETUP_ERR_CALL_FD,
	EVENTFD_SETUP_ERR_SET_VRING_CALL,
	EVENTFD_SETUP_ERR_EPOLL_ADD,
	EVENTFD_SETUP_ERR_SET_VRING_NUM,
	EVENTFD_SETUP_ERR_KICK_FD,
	EVENTFD_SETUP_ERR_SET_VRING_KICK,
	EVENTFD_SETUP_ERR_ERR_FD,
	EVENTFD_SETUP_ERR_SET_ERR_FD,
	EVENTFD_SETUP_ERR_ERR_EPOLL_ADD,
};

/**
 * has_feature() - Check a feature bit in a features set
 * @features:	Features set
 * @fb:		Feature bit to check
 *
 * Return: true if the feature bit is set
 */
static inline bool has_feature(uint64_t features, unsigned int fbit)
{
	return !!(features & (1ULL << fbit));
}

/**
 * vu_has_feature() - Check if a virtio-net feature is available
 * @vdev:	Vhost-user device
 * @fbit:	Feature to check
 *
 * Return: true if the feature is available
 */
static inline bool vu_has_feature(const struct vu_dev *vdev,
				  unsigned int fbit)
{
	return has_feature(vdev->features, fbit);
}

/**
 * vu_has_protocol_feature() - Check if a vhost-user feature is available
 * @vdev:	Vhost-user device
 * @fbit:	Feature to check
 *
 * Return: true if the feature is available
 */
/* cppcheck-suppress unusedFunction */
static inline bool vu_has_protocol_feature(const struct vu_dev *vdev,
					   unsigned int fbit)
{
	return has_feature(vdev->protocol_features, fbit);
}

void vu_queue_notify(const struct vu_dev *dev, struct vu_virtq *vq);
int vu_queue_pop(const struct vu_dev *dev, struct vu_virtq *vq,
		 struct vu_virtq_element *elem,
		 struct iovec *in_sg, size_t max_in_sg,
		 struct iovec *out_sg, size_t max_out_sg);
void vu_queue_detach_element(struct vu_virtq *vq);
void vu_queue_unpop(struct vu_virtq *vq);
bool vu_queue_rewind(struct vu_virtq *vq, unsigned int num);
void vu_queue_fill(const struct vu_dev *vdev, struct vu_virtq *vq,
		   const struct vu_virtq_element *elem, unsigned int len,
		   unsigned int idx);
void vu_queue_flush(const struct vu_dev *vdev, struct vu_virtq *vq,
		    unsigned int count);
enum set_vring_err set_vring_for_queue(struct ctx *c, int queue_idx, int tap_fd);
int setup_memory_table(struct ctx *c);
enum vhost_setup_err setup_vhost_net(struct ctx *c);
enum eventfd_setup_err setup_eventfds(struct ctx *c, int queue_idx);
void rx_pkt_refill(struct ctx *c);
void vhost_kick(struct vring_used *used, int kick_fd);

#endif /* VIRTIO_H */
