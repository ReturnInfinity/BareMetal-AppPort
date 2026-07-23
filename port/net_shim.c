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
// Scope: IPv4 TCP and UDP only (no raw sockets, no setsockopt options
// actually honored, no non-blocking mode). Good enough for a TCP
// client or a single-threaded accept-serve-close TCP server, or a
// UDP client/server exchanging whole datagrams.
// =============================================================================

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/sys.h"

#include "net_glue.h"
#include "net_shim.h"

#define SOCK_FD_BASE          100
#define SOCK_MAX              16
#define ACCEPTQ_MAX           8
#define UDP_RXQ_MAX            8
#define NET_BLOCK_TIMEOUT_MS  30000

enum { SK_FREE = 0, SK_CLOSED, SK_CONNECTING, SK_CONNECTED, SK_LISTENING, SK_ERROR };

// One queued, not-yet-consumed UDP datagram. Unlike TCP's rx_head
// (a byte stream, safe to pbuf_cat() together), each of these is a
// separate pbuf: UDP datagram boundaries have to be preserved so
// recv()/recvfrom() hands back one whole datagram per call, along
// with the sender's address.
struct udp_dgram {
	struct pbuf *p;
	ip4_addr_t addr;
	u16_t port;
};

struct bsock {
	int state;
	int type; // SOCK_STREAM or SOCK_DGRAM
	struct tcp_pcb *pcb;
	struct udp_pcb *upcb;

	// Bytes received but not yet consumed by recv()/read(). tcp_recved()
	// is only called as bytes are actually handed to the caller, so a
	// slow reader naturally throttles the sender via the TCP window
	// instead of us buffering without bound.
	struct pbuf *rx_head;
	u16_t rx_off;
	int eof;

	// UDP only: datagrams lwIP has already received but that our own
	// recv()/recvfrom() hasn't handed to the app yet. Unbounded like
	// TCP's rx_head is not an option here -- there's no flow control
	// to throttle a sender with -- so this is a fixed-depth ring and
	// on_udp_recv() drops datagrams once it's full.
	struct udp_dgram udpq[UDP_RXQ_MAX];
	int udpq_head, udpq_tail, udpq_count;

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
	ns->type = SOCK_STREAM;
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

static void on_udp_recv(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
	struct bsock *s = arg;
	(void)upcb;

	if (s->udpq_count >= UDP_RXQ_MAX) {
		pbuf_free(p); // queue full -- drop the datagram
		return;
	}

	struct udp_dgram *d = &s->udpq[s->udpq_tail];
	d->p = p;
	d->addr = *addr;
	d->port = port;
	s->udpq_tail = (s->udpq_tail + 1) % UDP_RXQ_MAX;
	s->udpq_count++;
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

	int base_type = type & 0xFF;
	if (domain != AF_INET || (base_type != SOCK_STREAM && base_type != SOCK_DGRAM))
		return -EAFNOSUPPORT; // only IPv4 TCP/UDP

	int slot = alloc_slot();
	if (slot < 0)
		return -EMFILE;

	struct bsock *s = &socks[slot];
	reset_sock(s);
	s->type = base_type;

	if (base_type == SOCK_DGRAM) {
		s->upcb = udp_new();
		if (!s->upcb)
			return -ENOMEM;
		udp_recv(s->upcb, on_udp_recv, s);
	} else {
		s->pcb = tcp_new();
		if (!s->pcb)
			return -ENOMEM;
		tcp_arg(s->pcb, s);
	}
	s->state = SK_CLOSED;

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

	if (s->type == SOCK_DGRAM)
		return udp_bind(s->upcb, &ip, lwip_ntohs(sin->sin_port)) == ERR_OK ? 0 : -EADDRINUSE;

	return tcp_bind(s->pcb, &ip, lwip_ntohs(sin->sin_port)) == ERR_OK ? 0 : -EADDRINUSE;
}

long net_shim_listen(long fd, long backlog)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->type != SOCK_STREAM)
		return -EOPNOTSUPP;

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

	if (s->type != SOCK_STREAM)
		return -EOPNOTSUPP;

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

	if (s->type == SOCK_DGRAM) {
		// UDP "connect" is local-only bookkeeping -- no handshake,
		// just records a default destination for send()/recv() to
		// use so callers don't have to pass an address every time.
		if (udp_connect(s->upcb, &ip, lwip_ntohs(sin->sin_port)) != ERR_OK)
			return -ECONNREFUSED;
		s->state = SK_CONNECTED;
		return 0;
	}

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

// Pops the oldest queued datagram, copying up to len bytes into buf
// (extra bytes in an oversized datagram are discarded, per recv(2)
// UDP semantics) and reporting the sender's address if requested.
static long udp_do_recv(struct bsock *s, void *buf, size_t len, ip4_addr_t *addr_out, u16_t *port_out)
{
	struct udp_dgram *d = &s->udpq[s->udpq_head];
	struct pbuf *p = d->p;
	size_t n = len < p->tot_len ? len : p->tot_len;

	pbuf_copy_partial(p, buf, n, 0);

	if (addr_out)
		*addr_out = d->addr;
	if (port_out)
		*port_out = d->port;

	pbuf_free(p);
	s->udpq_head = (s->udpq_head + 1) % UDP_RXQ_MAX;
	s->udpq_count--;

	return (long)n;
}

static long udp_wait_rx(struct bsock *s)
{
	u32_t start = sys_now();
	while (s->udpq_count == 0) {
		net_poll();
		if ((u32_t)(sys_now() - start) >= NET_BLOCK_TIMEOUT_MS)
			return -ETIMEDOUT;
	}
	return 0;
}

long net_shim_recv(long fd, void *buf, size_t len, long flags)
{
	(void)flags;
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->type == SOCK_DGRAM) {
		long r = udp_wait_rx(s);
		if (r < 0)
			return r;
		return udp_do_recv(s, buf, len, NULL, NULL);
	}

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

long net_shim_recvfrom(long fd, void *buf, size_t len, long flags, void *addr, socklen_t *addrlenp)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->type != SOCK_DGRAM)
		return net_shim_recv(fd, buf, len, flags);

	long r = udp_wait_rx(s);
	if (r < 0)
		return r;

	ip4_addr_t src_ip;
	u16_t src_port;
	long n = udp_do_recv(s, buf, len, &src_ip, &src_port);

	if (addr && addrlenp && *addrlenp >= (socklen_t)sizeof(struct sockaddr_in)) {
		struct sockaddr_in sin;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = lwip_htons(src_port);
		sin.sin_addr.s_addr = src_ip.addr;
		memcpy(addr, &sin, sizeof(sin));
		*addrlenp = sizeof(sin);
	}

	return n;
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

// dst/dst_port are only used when explicit -- a NULL dst sends to
// the pcb's connect()-ed default remote, matching send()'s "no
// address given" semantics.
static long udp_do_send(struct bsock *s, const void *buf, size_t len, const ip4_addr_t *dst, u16_t dst_port)
{
	if (len > 0xFFFF)
		return -EMSGSIZE;

	struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
	if (!p)
		return -ENOMEM;

	memcpy(p->payload, buf, len);

	err_t e = dst ? udp_sendto(s->upcb, p, dst, dst_port) : udp_send(s->upcb, p);
	pbuf_free(p);

	if (e != ERR_OK)
		return e == ERR_MEM ? -ENOBUFS : (e == ERR_RTE ? -ENETUNREACH : -EIO);

	return (long)len;
}

long net_shim_send(long fd, const void *buf, size_t len, long flags)
{
	(void)flags;
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->type == SOCK_DGRAM) {
		if (s->state != SK_CONNECTED)
			return -EDESTADDRREQ; // send() with no prior connect()
		return udp_do_send(s, buf, len, NULL, 0);
	}

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

long net_shim_sendto(long fd, const void *buf, size_t len, long flags, const void *addr, long addrlen)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->type != SOCK_DGRAM)
		return net_shim_send(fd, buf, len, flags); // TCP: destination is implicit

	if (!addr)
		return net_shim_send(fd, buf, len, flags);

	if ((size_t)addrlen < sizeof(struct sockaddr_in))
		return -EINVAL;

	const struct sockaddr_in *sin = addr;
	ip4_addr_t ip;
	ip.addr = sin->sin_addr.s_addr;

	return udp_do_send(s, buf, len, &ip, lwip_ntohs(sin->sin_port));
}

long net_shim_close(long fd)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->type == SOCK_DGRAM) {
		if (s->upcb) {
			udp_recv(s->upcb, NULL, NULL);
			udp_remove(s->upcb);
		}
		while (s->udpq_count > 0) {
			pbuf_free(s->udpq[s->udpq_head].p);
			s->udpq_head = (s->udpq_head + 1) % UDP_RXQ_MAX;
			s->udpq_count--;
		}
		s->state = SK_FREE;
		return 0;
	}

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
