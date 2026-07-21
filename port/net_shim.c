// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// net_shim.c -- a thin blocking BSD-socket-shaped wrapper over
// lwIP's raw (callback-based) TCP API, standing in for lwIP's own
// sockets.c. That code needs a real OS thread (tcpip.c) pumping a
// mailbox; we have none, so instead: every blocking call here
// (connect/accept/send/recv) runs its own synchronous loop that
// calls net_poll() (drains the NIC, services lwIP's timers, which
// fires our registered callbacks synchronously) until the operation
// either completes or NET_BLOCK_TIMEOUT_MS elapses.
//
// That timeout is a deliberate departure from POSIX (which blocks
// indefinitely): this is a single-threaded, unikernel-style VM with
// no way to interrupt or recover a truly stuck blocking call, so a
// bounded wait is safer than risking a permanently hung VM.
//
// Scope: IPv4 TCP only (no UDP/raw sockets, no setsockopt options
// actually honored, no non-blocking mode). Good enough for a TCP
// client or a single-threaded accept-serve-close TCP server.
// =============================================================================

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "lwip/tcp.h"
#include "lwip/sys.h"

#include "net_glue.h"
#include "net_shim.h"

#define SOCK_FD_BASE          100
#define SOCK_MAX              16
#define ACCEPTQ_MAX           8
#define NET_BLOCK_TIMEOUT_MS  30000

enum { SK_FREE = 0, SK_CLOSED, SK_CONNECTING, SK_CONNECTED, SK_LISTENING, SK_ERROR };

struct bsock {
	int state;
	struct tcp_pcb *pcb;

	// Bytes received but not yet consumed by recv()/read(). tcp_recved()
	// is only called as bytes are actually handed to the caller, so a
	// slow reader naturally throttles the sender via the TCP window
	// instead of us buffering without bound.
	struct pbuf *rx_head;
	u16_t rx_off;
	int eof;

	// Listening sockets only: connections lwIP has already accepted
	// (tcp_accepted() called) but that our own accept() hasn't
	// handed to the app yet, referenced by socket-table slot index.
	int acceptq[ACCEPTQ_MAX];
	int acceptq_head, acceptq_tail, acceptq_count;
};

static struct bsock socks[SOCK_MAX];

static int alloc_slot(void)
{
	for (int i = 0; i < SOCK_MAX; i++)
		if (socks[i].state == SK_FREE)
			return i;
	return -1;
}

static void reset_sock(struct bsock *s)
{
	memset(s, 0, sizeof(*s));
}

int net_shim_is_fd(long fd)
{
	long i = fd - SOCK_FD_BASE;
	return i >= 0 && i < SOCK_MAX && socks[i].state != SK_FREE;
}

// ---- lwIP raw-API callbacks ----

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	struct bsock *s = arg;
	(void)pcb;

	if (!p) {
		s->eof = 1;
		return ERR_OK;
	}
	if (err != ERR_OK) {
		pbuf_free(p);
		return ERR_OK;
	}

	if (s->rx_head)
		pbuf_cat(s->rx_head, p);
	else
		s->rx_head = p;

	return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
	struct bsock *s = arg;
	(void)err;
	// lwIP has already freed the pcb by the time this fires.
	s->pcb = NULL;
	s->state = SK_ERROR;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct bsock *s = arg;
	(void)pcb;
	s->state = (err == ERR_OK) ? SK_CONNECTED : SK_ERROR;
	return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	struct bsock *listener = arg;

	if (err != ERR_OK || !newpcb)
		return ERR_VAL;
	if (listener->acceptq_count >= ACCEPTQ_MAX)
		return ERR_MEM;

	int slot = alloc_slot();
	if (slot < 0)
		return ERR_MEM;

	struct bsock *ns = &socks[slot];
	reset_sock(ns);
	ns->state = SK_CONNECTED;
	ns->pcb = newpcb;

	tcp_arg(newpcb, ns);
	tcp_recv(newpcb, on_recv);
	tcp_err(newpcb, on_err);

	listener->acceptq[listener->acceptq_tail] = slot;
	listener->acceptq_tail = (listener->acceptq_tail + 1) % ACCEPTQ_MAX;
	listener->acceptq_count++;

	tcp_accepted(listener->pcb);

	return ERR_OK;
}

// ---- API ----

// Lazily brings the interface up and waits for DHCP on the first
// socket() call, rather than unconditionally at process startup --
// most programs never touch the network, and DHCP takes real time.
static int net_ready;

static void net_ensure_ready(void)
{
	if (net_ready)
		return;
	net_init();
	net_wait_ready(NET_BLOCK_TIMEOUT_MS);
	net_ready = 1;
}

long net_shim_socket(long domain, long type, long protocol)
{
	(void)protocol;

	net_ensure_ready();

	if (domain != AF_INET || (type & 0xFF) != SOCK_STREAM)
		return -EAFNOSUPPORT; // only IPv4 TCP in this first pass

	int slot = alloc_slot();
	if (slot < 0)
		return -EMFILE;

	struct bsock *s = &socks[slot];
	reset_sock(s);
	s->pcb = tcp_new();
	if (!s->pcb)
		return -ENOMEM;
	s->state = SK_CLOSED;
	tcp_arg(s->pcb, s);

	return SOCK_FD_BASE + slot;
}

long net_shim_bind(long fd, const void *addr, long addrlen)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];
	if ((size_t)addrlen < sizeof(struct sockaddr_in))
		return -EINVAL;

	const struct sockaddr_in *sin = addr;
	ip4_addr_t ip;
	ip.addr = sin->sin_addr.s_addr;

	return tcp_bind(s->pcb, &ip, lwip_ntohs(sin->sin_port)) == ERR_OK ? 0 : -EADDRINUSE;
}

long net_shim_listen(long fd, long backlog)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	struct tcp_pcb *lpcb = tcp_listen_with_backlog(s->pcb, (u8_t)backlog);
	if (!lpcb)
		return -EADDRINUSE;

	s->pcb = lpcb;
	s->state = SK_LISTENING;
	tcp_arg(lpcb, s);
	tcp_accept(lpcb, on_accept);

	return 0;
}

long net_shim_accept(long fd, void *addr, socklen_t *addrlenp)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	u32_t start = sys_now();
	while (s->acceptq_count == 0) {
		if (s->state != SK_LISTENING)
			return -EINVAL;
		net_poll();
		if ((u32_t)(sys_now() - start) >= NET_BLOCK_TIMEOUT_MS)
			return -EAGAIN;
	}

	int slot = s->acceptq[s->acceptq_head];
	s->acceptq_head = (s->acceptq_head + 1) % ACCEPTQ_MAX;
	s->acceptq_count--;

	struct bsock *ns = &socks[slot];

	if (addr && addrlenp && *addrlenp >= (socklen_t)sizeof(struct sockaddr_in)) {
		struct sockaddr_in sin;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = lwip_htons(ns->pcb->remote_port);
		sin.sin_addr.s_addr = ns->pcb->remote_ip.addr;
		memcpy(addr, &sin, sizeof(sin));
		*addrlenp = sizeof(sin);
	}

	return SOCK_FD_BASE + slot;
}

long net_shim_connect(long fd, const void *addr, long addrlen)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];
	if ((size_t)addrlen < sizeof(struct sockaddr_in))
		return -EINVAL;

	const struct sockaddr_in *sin = addr;
	ip4_addr_t ip;
	ip.addr = sin->sin_addr.s_addr;

	tcp_arg(s->pcb, s);
	tcp_recv(s->pcb, on_recv);
	tcp_err(s->pcb, on_err);

	s->state = SK_CONNECTING;
	if (tcp_connect(s->pcb, &ip, lwip_ntohs(sin->sin_port), on_connected) != ERR_OK) {
		s->state = SK_ERROR;
		return -ECONNREFUSED;
	}

	u32_t start = sys_now();
	while (s->state == SK_CONNECTING) {
		net_poll();
		if ((u32_t)(sys_now() - start) >= NET_BLOCK_TIMEOUT_MS)
			return -ETIMEDOUT;
	}

	return s->state == SK_CONNECTED ? 0 : -ECONNREFUSED;
}

static long bsock_read(struct bsock *s, void *buf, size_t len)
{
	struct pbuf *p = s->rx_head;
	size_t avail = p->len - s->rx_off;
	size_t n = len < avail ? len : avail;

	memcpy(buf, (const char *)p->payload + s->rx_off, n);
	s->rx_off += n;

	if (s->rx_off == p->len) {
		struct pbuf *rest = p->next;
		p->next = NULL; // detach so pbuf_free() doesn't free the rest of the chain too
		pbuf_free(p);
		s->rx_head = rest;
		s->rx_off = 0;
	}

	if (s->pcb)
		tcp_recved(s->pcb, (u16_t)n);

	return (long)n;
}

long net_shim_recv(long fd, void *buf, size_t len, long flags)
{
	(void)flags;
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (len == 0)
		return 0;

	u32_t start = sys_now();
	for (;;) {
		if (s->rx_head)
			return bsock_read(s, buf, len);
		if (s->eof)
			return 0;
		if (s->state == SK_ERROR)
			return -ECONNRESET;
		net_poll();
		if ((u32_t)(sys_now() - start) >= NET_BLOCK_TIMEOUT_MS)
			return -ETIMEDOUT;
	}
}

static long bsock_write(struct bsock *s, const void *buf, size_t len)
{
	if (s->state == SK_ERROR || !s->pcb)
		return -ECONNRESET;

	u16_t sndbuf = tcp_sndbuf(s->pcb);
	if (sndbuf == 0)
		return 0; // caller retries after polling

	size_t n = len < sndbuf ? len : sndbuf;
	if (n > 0xFFFF)
		n = 0xFFFF;

	err_t e = tcp_write(s->pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY);
	if (e != ERR_OK)
		return e == ERR_MEM ? 0 : -ECONNRESET;

	tcp_output(s->pcb);
	return (long)n;
}

long net_shim_send(long fd, const void *buf, size_t len, long flags)
{
	(void)flags;
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (len == 0)
		return 0;

	u32_t start = sys_now();
	for (;;) {
		long n = bsock_write(s, buf, len);
		if (n != 0)
			return n;
		if (s->state == SK_ERROR)
			return -ECONNRESET;
		net_poll();
		if ((u32_t)(sys_now() - start) >= NET_BLOCK_TIMEOUT_MS)
			return -ETIMEDOUT;
	}
}

long net_shim_close(long fd)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->pcb) {
		tcp_arg(s->pcb, NULL);
		if (s->state == SK_LISTENING)
			tcp_accept(s->pcb, NULL);
		else {
			tcp_recv(s->pcb, NULL);
			tcp_err(s->pcb, NULL);
		}
		if (tcp_close(s->pcb) != ERR_OK)
			tcp_abort(s->pcb);
	}

	while (s->rx_head) {
		struct pbuf *rest = s->rx_head->next;
		s->rx_head->next = NULL;
		pbuf_free(s->rx_head);
		s->rx_head = rest;
	}
	// Any not-yet-accept()ed connections left in acceptq are leaked
	// (never explicitly closed) -- acceptable gap for this first pass.

	s->state = SK_FREE;
	return 0;
}

// =============================================================================
// EOF
