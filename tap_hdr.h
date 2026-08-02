/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2021 Red Hat GmbH
 * Author: David Gibson <david@gibson.dropbear.id.au>
 */

#ifndef TAP_HDR_H
#define TAP_HDR_H

#include <stdint.h>
#include <linux/virtio_net.h>

/**
 * struct tap_hdr - tap backend specific headers
 * @vnet_len:	Frame length (for qemu socket transport)
 */
struct tap_hdr {
	union {
		uint32_t vnet_len;
		struct virtio_net_hdr_mrg_rxbuf hdr;
	};
};

#endif /* TAP_HDR_H */
