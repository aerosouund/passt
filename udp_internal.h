/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2021 Red Hat GmbH
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

#ifndef UDP_INTERNAL_H
#define UDP_INTERNAL_H

#include <netinet/in.h>
#include <netinet/udp.h>

#include "tap.h" /* needed by udp_meta_t */


size_t udp_update_hdr4(struct iphdr *ip4h, struct udphdr *uh,
		       struct iov_tail *payload,
		       const struct flowside *toside, size_t dlen,
		       bool no_udp_csum);
size_t udp_update_hdr6(struct ipv6hdr *ip6h, struct udphdr *uh,
		       struct iov_tail *payload,
		       const struct flowside *toside, size_t dlen,
		       bool no_udp_csum);
void udp_sock_fwd(const struct ctx *c, int s, int rule_hint,
		  uint8_t frompif, in_port_t port, const struct timespec *now);

#endif /* UDP_INTERNAL_H */
