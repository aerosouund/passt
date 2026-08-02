/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2021 Red Hat GmbH
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <netinet/in.h>
#include <netinet/udp.h>

#include "tap_hdr.h"
#include "fwd.h"

/**
 * struct udp_payload_t - UDP header and data for inbound messages
 * @uh:		UDP header
 * @data:	UDP data
 */
struct udp_payload_t {
	struct udphdr uh;
	char data[USHRT_MAX - sizeof(struct udphdr)];
#ifdef __AVX2__
} __attribute__ ((packed, aligned(32)));
#else
} __attribute__ ((packed, aligned(__alignof__(unsigned int))));
#endif

#define UDP_MAX_FRAMES		32  /* max # of frames to receive at once */

/* UDP header and data for inbound messages */
extern struct udp_payload_t udp_payload[UDP_MAX_FRAMES];

/* Ethernet headers for IPv4 and IPv6 frames */
extern struct ethhdr udp_eth_hdr[UDP_MAX_FRAMES];

/* IOVs and msghdr arrays for receiving datagrams from sockets */
extern struct iovec	udp_iov_recv		[UDP_MAX_FRAMES];
extern struct mmsghdr	udp_mh_recv		[UDP_MAX_FRAMES];


/**
 * struct udp_meta_t - Pre-cooked headers for UDP packets
 * @ip6h:	Pre-filled IPv6 header (except for payload_len and addresses)
 * @ip4h:	Pre-filled IPv4 header (except for tot_len and saddr)
 * @taph:	Tap backend specific header
 */
struct udp_meta_t {
	struct ipv6hdr ip6h;
	struct iphdr ip4h;
	struct tap_hdr taph;
#ifdef __AVX2__
} __attribute__((aligned(32)));
#else
};
#endif

/* Pre-cooked headers for UDP packets */
extern struct udp_meta_t udp_meta[UDP_MAX_FRAMES];


void udp_listen_sock_handler(const struct ctx *c, union epoll_ref ref,
			     uint32_t events, const struct timespec *now);
void udp_sock_handler(const struct ctx *c, union epoll_ref ref,
		      uint32_t events, const struct timespec *now);
int udp_tap_handler(const struct ctx *c, uint8_t pif,
		    sa_family_t af, const void *saddr, const void *daddr,
		    uint8_t ttl, const struct pool *p, int idx,
		    const struct timespec *now);
int udp_init(struct ctx *c);
void udp_update_l2_buf(const unsigned char *eth_d);

/**
 * struct udp_ctx - Execution context for UDP
 * @scan_in:		Port scanning state for inbound packets
 * @scan_out:		Port scanning state for outbound packets
 * @timeout:		Timeout for unidirectional flows (in s)
 * @stream_timeout:	Timeout for stream-like flows (in s)
 */
struct udp_ctx {
	struct fwd_scan scan_in;
	struct fwd_scan scan_out;
	int timeout;
	int stream_timeout;
};

#endif /* UDP_H */
