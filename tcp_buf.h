/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2021 Red Hat GmbH
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

#ifndef TCP_BUF_H
#define TCP_BUF_H

#include <linux/virtio_net.h>

#include "tcp_conn.h"
#include "tcp_internal.h"

void tcp_sock_iov_init(const struct ctx *c);
void tcp_payload_flush(const struct ctx *c, const struct timespec *now);
int tcp_buf_data_from_sock(const struct ctx *c, struct tcp_tap_conn *conn,
			   uint32_t already_sent, const struct timespec *now);
int tcp_buf_send_flag(const struct ctx *c, struct tcp_tap_conn *conn, int flags,
		      const struct timespec *now);

#define TCP_FRAMES_MEM			128
#define TCP_FRAMES							   \
(c->mode == MODE_PASTA ? 1 : TCP_FRAMES_MEM)

extern struct virtio_net_hdr_mrg_rxbuf tcp_payload_tap_hdr[TCP_FRAMES_MEM];
extern struct tcp_payload_t	tcp_payload[TCP_FRAMES_MEM];

extern struct ethhdr		tcp4_eth_src;
extern struct ethhdr		tcp6_eth_src;

extern struct iphdr		tcp4_payload_ip[TCP_FRAMES_MEM];
extern struct ipv6hdr		tcp6_payload_ip[TCP_FRAMES_MEM];

#endif  /*TCP_BUF_H */
