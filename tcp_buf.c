// SPDX-License-Identifier: GPL-2.0-or-later

/* PASST - Plug A Simple Socket Transport
 *  for qemu/UNIX domain socket mode
 *
 * PASTA - Pack A Subtle Tap Abstraction
 *  for network namespace/tap device mode
 *
 * tcp_buf.c - TCP L2 buffer management functions
 *
 * Copyright Red Hat
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <errno.h>

#include <netinet/ip.h>

#include <netinet/tcp.h>
#include <linux/virtio_net.h>

#include "util.h"
#include "ip.h"
#include "iov.h"
#include "passt.h"
#include "tap.h"
#include "siphash.h"
#include "inany.h"
#include "tcp_conn.h"
#include "tcp_internal.h"
#include "vhost.h"

#define TCP_FRAMES_MEM			128
#define TCP_FRAMES							   \
	(c->mode == MODE_PASTA ? 1 : TCP_FRAMES_MEM)

#define TCP_NREGIONS 5

/* Static buffers */

/* Ethernet header for IPv4 and IPv6 frames */
static struct ethhdr		tcp_eth_hdr[TCP_FRAMES_MEM];

static struct virtio_net_hdr_mrg_rxbuf tcp_payload_tap_hdr[TCP_FRAMES_MEM];

/* IP headers for IPv4 and IPv6 */
static struct iphdr		tcp4_payload_ip[TCP_FRAMES_MEM];
static struct ipv6hdr		tcp6_payload_ip[TCP_FRAMES_MEM];

/* TCP segments with payload for IPv4 and IPv6 frames */
static struct tcp_payload_t	tcp_payload[TCP_FRAMES_MEM];

static_assert(MSS4 <= sizeof(tcp_payload[0].data), "MSS4 is greater than 65516");
static_assert(MSS6 <= sizeof(tcp_payload[0].data), "MSS6 is greater than 65516");

/* References tracking the owner connection of frames in the tap outqueue */
static struct tcp_tap_conn *tcp_frame_conns[TCP_FRAMES_MEM];

/**
 * struct tcp_payload_idx - Grow-only cursors into the TCP frame buffers
 * @tcp_buf_idx:	The last index we wrote data to in the tcp buffers
 * @processed:		Last index we pushed to the underlying connection
 */
static struct tcp_payload_idx {
	unsigned int tcp_buf_idx;
	unsigned int processed;
} tcp_idx;

/* latest index we wrote to into the tcp buffers */
#define TCP_CURR_IDX	(tcp_idx.tcp_buf_idx % TCP_FRAMES_MEM)

/* current amount of frames queued in tcp buffers but not sent */
#define TCP_FRAME_COUNT	(tcp_idx.tcp_buf_idx - tcp_idx.processed)

/* recvmsg()/sendmsg() data for tap */
static struct iovec	iov_sock		[TCP_FRAMES_MEM + DISCARD_IOV_NUM];

static struct iovec	tcp_l2_iov[TCP_FRAMES_MEM][TCP_NUM_IOVS];

/**
 * tcp_update_l2_buf() - Update Ethernet header buffers with addresses
 * @eth_d:	Ethernet destination address, NULL if unchanged
 */
void tcp_update_l2_buf(const unsigned char *eth_d)
{
	int i;

	for (i = 0; i < TCP_FRAMES_MEM; i++)
		eth_update_mac(&tcp_eth_hdr[i], eth_d, NULL);
}

/**
 * tcp_register_memory_region() - Register the TCP specific buffers into a memory
 *                                struct so it can be shared with the kernel.
 * @vhost_mem: The memory struct to register regions into
 * @last_idx:  The last index at which memory region has been placed
 */
void tcp_register_memory_regions(union vhost_memory_u *vhost_mem, size_t *last_idx) {
	if ((*last_idx + TCP_NREGIONS) > N_VHOST_REGIONS)
		die("tcp: memory region overflow. maximum is %d, got %zu",
		    N_VHOST_REGIONS, *last_idx + TCP_NREGIONS);
	
   	vhost_mem->mem.regions[(*last_idx)++] = VHOST_MEMORY_REGION(tcp_payload_tap_hdr);
	vhost_mem->mem.regions[(*last_idx)++] = VHOST_MEMORY_REGION(tcp4_payload_ip);
	vhost_mem->mem.regions[(*last_idx)++] = VHOST_MEMORY_REGION(tcp6_payload_ip);
	vhost_mem->mem.regions[(*last_idx)++] = VHOST_MEMORY_REGION(tcp_payload);
	vhost_mem->mem.regions[(*last_idx)++] = VHOST_MEMORY_REGION(tcp_eth_hdr);
}

/**
 * tcp_sock_iov_init() - Initialise scatter-gather L2 buffers for IPv4 sockets
 * @c:		Execution context
 */
void tcp_sock_iov_init(const struct ctx *c)
{
	struct ipv6hdr ip6 = L2_BUF_IP6_INIT(IPPROTO_TCP);
	struct iphdr iph = L2_BUF_IP4_INIT(IPPROTO_TCP);
	int i;

	for (i = 0; i < ARRAY_SIZE(tcp_payload); i++) {
		tcp6_payload_ip[i] = ip6;
		tcp4_payload_ip[i] = iph;
	}

	for (i = 0; i < TCP_FRAMES_MEM; i++) {
		struct iovec *iov = tcp_l2_iov[i];

		iov[TCP_IOV_TAP] = tap_hdr_iov(c, (struct tap_hdr *)&tcp_payload_tap_hdr[i]);
		iov[TCP_IOV_ETH].iov_len = sizeof(struct ethhdr);
		iov[TCP_IOV_PAYLOAD].iov_base = &tcp_payload[i];
		iov[TCP_IOV_ETH_PAD].iov_base = eth_pad;
	}
}

/**
 * tcp_revert_seq() - Revert affected conn->seq_to_tap after failed transmission
 * @c:		Execution context
 * @conns:	Array of connection pointers corresponding to queued frames
 * @frames:	Two-dimensional array containing queued frames with sub-iovs
 * @num_frames:	Number of entries in the two arrays to be compared
 * @now:	Current timestamp
 */
static void tcp_revert_seq(const struct ctx *c, struct tcp_tap_conn **conns,
			   struct iovec (*frames)[TCP_NUM_IOVS], int num_frames,
			   const struct timespec *now)
{
	int i;

	for (i = 0; i < num_frames; i++) {
		const struct tcphdr *th = frames[i][TCP_IOV_PAYLOAD].iov_base;
		struct tcp_tap_conn *conn = conns[i];
		uint32_t seq = ntohl(th->seq);
		uint32_t peek_offset;

		if (SEQ_LE(conn->seq_to_tap, seq))
			continue;

		conn->seq_to_tap = seq;
		peek_offset = conn->seq_to_tap - conn->seq_ack_from_tap;
		if (tcp_set_peek_offset(conn, peek_offset, now))
			tcp_rst(c, conn, now);
	}
}

/**
 * tcp_payload_flush() - Send out buffers for segments with data or flags
 * @c:		Execution context
 * @now:	Current timestamp
 */
void tcp_payload_flush(const struct ctx *c, const struct timespec *now)
{
	unsigned int total = TCP_FRAME_COUNT;
	unsigned int start = tcp_idx.processed % TCP_FRAMES_MEM;
	unsigned int first_batch_size, sent;

	if (!total)
		return;

	/* What is smaller ? all we what we want to send ? or 128 - the index
	 * indicating the start of where we wrote this batch ?
	 */
	first_batch_size = MIN(total, TCP_FRAMES_MEM - start);

	sent = tap_send_frames(c, &tcp_l2_iov[start][0], TCP_NUM_IOVS,
			       first_batch_size);
	if (sent < first_batch_size) {
		tcp_revert_seq(c, &tcp_frame_conns[start + sent],
			       &tcp_l2_iov[start + sent],
			       first_batch_size - sent, now);
		goto out;
	}

	/* There was more data to send than from tcp_idx.processed up to 128.
	 * the rest of the batch is going to be at index 0 up total -
	 * first_batch_size.
	 */
	if (total > first_batch_size) {
		unsigned int second_batch_size = total - first_batch_size;
		size_t m2;

		m2 = tap_send_frames(c, &tcp_l2_iov[0][0], TCP_NUM_IOVS,
				     second_batch_size);
		sent += m2;

		if (m2 < second_batch_size)
			tcp_revert_seq(c, &tcp_frame_conns[m2], &tcp_l2_iov[m2],
				       second_batch_size - m2, now);
	}

out:
	tcp_idx.processed += sent;
}

/**
 * tcp_l2_buf_pad() - Calculate padding to send out of padding (zero) buffer
 * @iov:	Pointer to iovec of frame parts we're about to send
 */
static void tcp_l2_buf_pad(struct iovec *iov)
{
	size_t l2len = iov[TCP_IOV_ETH].iov_len +
		       iov[TCP_IOV_IP].iov_len +
		       iov[TCP_IOV_PAYLOAD].iov_len;

	if (l2len < ETH_ZLEN)
		iov[TCP_IOV_ETH_PAD].iov_len = ETH_ZLEN - l2len;
	else
		iov[TCP_IOV_ETH_PAD].iov_len = 0;
}

/**
 * tcp_l2_buf_fill_headers() - Fill 802.3, IP, TCP headers in pre-cooked buffers
 * @c:		Execution context
 * @conn:	Connection pointer
 * @iov:	Pointer to an array of iovec of TCP pre-cooked buffers
 * @csum_flags:	TCP_CSUM if TCP checksum must be computed,
 * 		IP4_CSUM if IPv4 checksum must be computed,
 * 		otherwise IPv4 checksum is provided in IP4_CMASK
 * @seq:	Sequence number for this segment
 */
static void tcp_l2_buf_fill_headers(const struct ctx *c,
				    struct tcp_tap_conn *conn,
				    struct iovec *iov, uint32_t csum_flags,
				    uint32_t seq)
{
	struct iov_tail tail = IOV_TAIL(&iov[TCP_IOV_PAYLOAD], 1, 0);
	struct tcphdr th_storage, *th = IOV_REMOVE_HEADER(&tail, th_storage);
	struct tap_hdr *taph = iov[TCP_IOV_TAP].iov_base;
	const struct flowside *tapside = TAPFLOW(conn);
	const struct in_addr *a4 = inany_v4(&tapside->oaddr);
	struct ethhdr *eh = iov[TCP_IOV_ETH].iov_base;
	struct ipv6hdr *ip6h = NULL;
	struct iphdr *ip4h = NULL;
	size_t l2len;

	if (a4)
		ip4h = iov[TCP_IOV_IP].iov_base;
	else
		ip6h = iov[TCP_IOV_IP].iov_base;

	l2len = tcp_fill_headers(c, conn, eh, ip4h, ip6h, th, &tail,
				 iov_tail_size(&tail), csum_flags, seq);

	/* With vhost-net this buffer holds a virtio-net header, which a tap
	 * one must not be written over
	 */
	if (c->vhost.fd == -1)
		tap_hdr_update(taph, l2len);
}

/**
 * tcp_buf_send_flag() - Send segment with flags to tap (no payload)
 * @c:		Execution context
 * @conn:	Connection pointer
 * @flags:	TCP flags: if not set, send segment only if ACK is due
 * @now:	Current timestamp
 *
 * Return: negative error code on connection reset, 0 otherwise
 */
int tcp_buf_send_flag(const struct ctx *c, struct tcp_tap_conn *conn, int flags,
		      const struct timespec *now)
{
	struct tcp_payload_t *payload;
	struct iovec *iov;
	size_t optlen;
	size_t l4len;
	uint32_t seq;
	int ret;

	iov = tcp_l2_iov[TCP_CURR_IDX];
	if (CONN_V4(conn))
		iov[TCP_IOV_IP] = IOV_OF_LVALUE(tcp4_payload_ip[TCP_CURR_IDX]);
	else
		iov[TCP_IOV_IP] = IOV_OF_LVALUE(tcp6_payload_ip[TCP_CURR_IDX]);

	iov[TCP_IOV_ETH] = IOV_OF_LVALUE(tcp_eth_hdr[TCP_CURR_IDX]);
	payload = iov[TCP_IOV_PAYLOAD].iov_base;
	seq = conn->seq_to_tap;
	ret = tcp_prepare_flags(c, conn, flags, &payload->th,
				(struct tcp_syn_opts *)&payload->data,
				&optlen, now);
	if (ret <= 0)
		return ret;

	tcp_idx.tcp_buf_idx++;
	tcp_frame_conns[TCP_CURR_IDX] = conn;
	l4len = optlen + sizeof(struct tcphdr);
	iov[TCP_IOV_PAYLOAD].iov_len = l4len;

	if (flags & KEEPALIVE)
		seq--;

	tcp_l2_buf_fill_headers(c, conn, iov, IP4_CSUM | TCP_CSUM, seq);

	tcp_l2_buf_pad(iov);

	if (flags & DUP_ACK) {
		struct iovec *dup_iov = tcp_l2_iov[TCP_CURR_IDX];
		tcp_frame_conns[TCP_CURR_IDX] = conn;
		tcp_idx.tcp_buf_idx++;

		memcpy(dup_iov[TCP_IOV_TAP].iov_base, iov[TCP_IOV_TAP].iov_base,
		       iov[TCP_IOV_TAP].iov_len);
		dup_iov[TCP_IOV_ETH].iov_base = iov[TCP_IOV_ETH].iov_base;
		dup_iov[TCP_IOV_IP] = iov[TCP_IOV_IP];
		memcpy(dup_iov[TCP_IOV_PAYLOAD].iov_base,
		       iov[TCP_IOV_PAYLOAD].iov_base, l4len);
		dup_iov[TCP_IOV_PAYLOAD].iov_len = l4len;
		dup_iov[TCP_IOV_ETH_PAD].iov_len = iov[TCP_IOV_ETH_PAD].iov_len;
	}

	if (TCP_FRAME_COUNT > TCP_FRAMES_MEM - 2)
		tcp_payload_flush(c, now);

	return 0;
}

/**
 * tcp_data_to_tap() - Finalise (queue) highest-numbered scatter-gather buffer
 * @c:		Execution context
 * @conn:	Connection pointer
 * @dlen:	TCP payload length
 * @no_csum:	Don't compute IPv4 checksum, use the one from previous buffer
 * @seq:	Sequence number to be sent
 * @push:	Set PSH flag, last segment in a batch
 * @now:	Current timestamp
 */
static void tcp_data_to_tap(const struct ctx *c, struct tcp_tap_conn *conn,
			    ssize_t dlen, int no_csum, uint32_t seq, bool push,
			    const struct timespec *now)
{
	struct tcp_payload_t *payload;
	uint32_t check = IP4_CSUM;
	struct iovec *iov;

	conn->seq_to_tap = seq + dlen;
	tcp_frame_conns[TCP_CURR_IDX] = conn;
	iov = tcp_l2_iov[TCP_CURR_IDX];
	if (CONN_V4(conn)) {
		if (no_csum) {
			/* TCP_CURR_IDX may be zero if the underlying
			 * tcp_idx.tcp_buf_idx is a multiple of 128, minus one
			 * will yield an invalid index. The previous index to 0
			 * is 127.
			 */
			unsigned int prev_idx = (TCP_CURR_IDX +
						 TCP_FRAMES_MEM - 1) %
						TCP_FRAMES_MEM;
			struct iovec *iov_prev = tcp_l2_iov[prev_idx];
			const struct iphdr *iph = iov_prev[TCP_IOV_IP].iov_base;

			/* overwrite IP4_CSUM flag as we set the checksum */
			check = iph->check;
		}
		iov[TCP_IOV_IP] = IOV_OF_LVALUE(tcp4_payload_ip[TCP_CURR_IDX]);
	} else if (CONN_V6(conn)) {
		iov[TCP_IOV_IP] = IOV_OF_LVALUE(tcp6_payload_ip[TCP_CURR_IDX]);
	}
	iov[TCP_IOV_ETH].iov_base = &tcp_eth_hdr[TCP_CURR_IDX];
	payload = iov[TCP_IOV_PAYLOAD].iov_base;
	payload->th.th_off = sizeof(struct tcphdr) / 4;
	payload->th.th_x2 = 0;
	payload->th.th_flags = 0;
	payload->th.ack = 1;
	payload->th.psh = push;
	iov[TCP_IOV_PAYLOAD].iov_len = dlen + sizeof(struct tcphdr);
	tcp_l2_buf_fill_headers(c, conn, iov, TCP_CSUM | check, seq);

	tcp_l2_buf_pad(iov);

	tcp_idx.tcp_buf_idx++;
	if (TCP_FRAME_COUNT > TCP_FRAMES_MEM - 1)
		tcp_payload_flush(c, now);
}

/**
 * tcp_buf_data_from_sock() - Handle new data from socket, queue to tap, in window
 * @c:		Execution context
 * @conn:	Connection pointer
 * @already_sent:	Number of bytes already sent to tap, but not acked
 * @now:	Current timestamp
 *
 * Return: negative on connection reset, 0 otherwise
 *
 * #syscalls recvmsg
 */
int tcp_buf_data_from_sock(const struct ctx *c, struct tcp_tap_conn *conn,
			   uint32_t already_sent, const struct timespec *now)
{
	uint32_t wnd_scaled = conn->wnd_from_tap << conn->ws_from_tap;
	int fill_bufs, send_bufs = 0, last_len, iov_rem = 0;
	int len, dlen, i, s = conn->sock;
	struct msghdr mh_sock = { 0 };
	uint16_t mss = MSS_GET(conn);
	struct iovec *iov;
	uint32_t seq;

	/* Set up buffer descriptors we'll fill completely and partially. */
	fill_bufs = DIV_ROUND_UP(wnd_scaled - already_sent, mss);
	if (fill_bufs > TCP_FRAMES) {
		fill_bufs = TCP_FRAMES;
		iov_rem = 0;
	} else {
		iov_rem = (wnd_scaled - already_sent) % mss;
	}

	if (tcp_prepare_iov(&mh_sock, iov_sock, already_sent, fill_bufs)) {
		tcp_rst(c, conn, now);
		return -1;
	}

	if (TCP_FRAME_COUNT + (unsigned int)fill_bufs > TCP_FRAMES_MEM)
		tcp_payload_flush(c, now);

	for (i = 0, iov = iov_sock + DISCARD_IOV_NUM; i < fill_bufs; i++, iov++) {
		unsigned int idx = (TCP_CURR_IDX + i) % TCP_FRAMES_MEM;

		iov->iov_base = &tcp_payload[idx].data;
		iov->iov_len = mss;
	}
	if (iov_rem)
		iov_sock[fill_bufs + DISCARD_IOV_NUM - 1].iov_len = iov_rem;

	/* Receive into buffers, don't dequeue until acknowledged by guest. */
	do
		len = recvmsg(s, &mh_sock, MSG_PEEK);
	while (len < 0 && errno == EINTR);

	if (len < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			tcp_rst(c, conn, now);
			return -errno;
		}

		if (already_sent) /* No new data and EAGAIN: set EPOLLET */
			conn_flag(c, conn, STALLED, now);

		return 0;
	}

	if (!len) {
		if (already_sent) {
			conn_flag(c, conn, STALLED, now);
		} else if ((conn->events & (SOCK_FIN_RCVD | TAP_FIN_SENT)) ==
			   SOCK_FIN_RCVD) {
			int ret;

			/* On TAP_FIN_SENT, we won't get further data events
			 * from the socket, and this might be the last ACK
			 * segment we send to the tap, so update its sequence to
			 * include everything we received until now.
			 *
			 * See also the special handling on CONN_IS_CLOSING() in
			 * tcp_update_seqack_wnd().
			 */
			conn->seq_ack_to_tap = conn->seq_from_tap;

			ret = tcp_buf_send_flag(c, conn, FIN | ACK, now);
			if (ret) {
				tcp_rst(c, conn, now);
				return ret;
			}

			conn_event(c, conn, TAP_FIN_SENT, now);
			conn_flag(c, conn, ACK_FROM_TAP_DUE, now);
		}

		return 0;
	}

	if (!peek_offset_cap)
		len -= already_sent;

	if (len <= 0) {
		conn_flag(c, conn, STALLED, now);
		return 0;
	}

	conn_flag(c, conn, ~ACK_FROM_TAP_BLOCKS, now);
	conn_flag(c, conn, ~STALLED, now);

	send_bufs = DIV_ROUND_UP(len, mss);
	last_len = len - (send_bufs - 1) * mss;

	/* Likely, some new data was acked too. */
	tcp_update_seqack_wnd(c, conn, false, NULL, now);

	/* Finally, queue to tap */
	dlen = mss;
	seq = conn->seq_to_tap;
	for (i = 0; i < send_bufs; i++) {
		int no_csum = i && i != send_bufs - 1 && TCP_CURR_IDX;
		bool push = false;

		if (i == send_bufs - 1) {
			dlen = last_len;
			push = true;
		}

		tcp_data_to_tap(c, conn, dlen, no_csum, seq, push, now);
		seq += dlen;
	}

	conn_flag(c, conn, ACK_FROM_TAP_DUE, now);

	return 0;
}
