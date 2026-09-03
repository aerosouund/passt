/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2021 Red Hat GmbH
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

#ifndef TCP_BUF_H
#define TCP_BUF_H

union vhost_memory_u;
struct tcp_tap_conn;

void tcp_sock_iov_init(const struct ctx *c);
void tcp_payload_flush(const struct ctx *c, const struct timespec *now);
int tcp_buf_data_from_sock(const struct ctx *c, struct tcp_tap_conn *conn,
			   uint32_t already_sent, const struct timespec *now);
int tcp_buf_send_flag(const struct ctx *c, struct tcp_tap_conn *conn, int flags,
		      const struct timespec *now);
void tcp_register_memory_regions(union vhost_memory_u *vhost_mem, size_t *last_idx);

#endif  /*TCP_BUF_H */
