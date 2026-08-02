/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2021 Red Hat GmbH
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

#ifndef TAP_HDR_H
#define TAP_HDR_H

#include <stdint.h>

/**
 * struct tap_hdr - tap backend specific headers
 * @vnet_len:	Frame length (for qemu socket transport)
 */
struct tap_hdr {
	uint32_t vnet_len;
} __attribute__((packed));

#endif /* TAP_HDR_H */
