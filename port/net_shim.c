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
// either completes or that socket's timeout elapses.
//
// Each socket carries its own SO_RCVTIMEO (accept/recv/recvfrom) and
// SO_SNDTIMEO (connect/send) timeout in milliseconds, settable via
// setsockopt() and defaulting to NET_BLOCK_TIMEOUT_MS. A value of 0
// (the real POSIX meaning of an unset/zeroed SO_*TIMEO) blocks
// indefinitely instead of timing out -- safe to offer now that
// threads exist (thread_shim.c): a call parked here no longer risks
// wedging the whole VM, since the round-robin timer tick keeps other
// threads running regardless, and the EINTR check below already lets
// another thread break it out early via a signal
// (pthread_kill()/pthread_cancel()). The bounded 30s default remains
// for callers that never touch setsockopt(), matching this port's
// original stance of not blocking forever by surprise.
//
// connect() doesn't have its own dedicated timeout socket option in
// real POSIX, but ties its bound to SO_SNDTIMEO here too -- the
// closest fit among the two this port exposes, and consistent with
// send() sharing the same option.
//
// Non-blocking mode (O_NONBLOCK via SOCK_NONBLOCK/accept4()/
// fcntl(F_SETFL)/ioctl(FIONBIO), or MSG_DONTWAIT per call) runs the
// same loops for exactly one net_poll() pass and then fails with
// -EAGAIN instead of waiting; a non-blocking connect() returns
// -EINPROGRESS and reports its outcome through POLLOUT/POLLERR and
// getsockopt(SO_ERROR). net_shim_poll() reports real per-socket
// readiness from the same state these loops wait on, which is what
// posix_shim.c's poll()/select() are built on -- together, enough for
// a single-threaded event-loop server juggling many connections.
//
// Scope: IPv4 TCP and UDP only (no raw sockets; only SO_RCVTIMEO/
// SO_SNDTIMEO are honored by setsockopt(), and only those plus
// SO_ERROR/SO_TYPE by getsockopt() -- every other option is still an
// accept-and-ignore stub).
// =============================================================================

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/sys.h"

#include "libBareMetal.h"
#include "net_glue.h"
#include "net_shim.h"
#include "thread_shim.h"

#define SOCK_FD_BASE          100
#define SOCK_MAX              16
// Can never usefully exceed the socket table, since every queued
// connection already owns a slot -- see on_accept().
#define ACCEPTQ_MAX           SOCK_MAX
#define UDP_RXQ_MAX            8
#define NET_BLOCK_TIMEOUT_MS  30000

// Each blocking loop below calls net_poll() to drain the NIC/service
// lwIP's timers, then -- if still not ready -- waits one interval via
// thread_shim_sleep_until() rather than immediately looping back to
// net_poll() again. Without this a blocking accept()/connect()/recv()/
// send() with nothing to do (e.g. an idle listening server) spins at
// 100% CPU for up to NET_BLOCK_TIMEOUT_MS on every call. Matches the
// same scheduler-aware wait posix_shim.c's nanosleep() already uses --
// a raw b_system(SLEEP, ...) HLT here would, same as it did there,
// starve every other thread for the whole interval instead of just
// this one socket's caller (see thread_shim_sleep_until()'s comment).
#define NET_POLL_INTERVAL_NS  10000000ULL // 10ms

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

	// SO_RCVTIMEO/SO_SNDTIMEO, in milliseconds; 0 means block
	// indefinitely (real POSIX semantics). Defaulted to
	// NET_BLOCK_TIMEOUT_MS wherever a slot starts life -- see
	// net_shim_socket() and on_accept().
	long rcv_timeout_ms;
	long snd_timeout_ms;

	// O_NONBLOCK -- see the file header.
	int nonblock;

	// Pending SO_ERROR (a positive errno), set when lwIP reports a
	// connection failure and cleared by getsockopt(SO_ERROR) or by the
	// connect() call that reports it.
	int so_error;
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
	s->rcv_timeout_ms = NET_BLOCK_TIMEOUT_MS;
	s->snd_timeout_ms = NET_BLOCK_TIMEOUT_MS;
}

// timeout_ms == 0 means block indefinitely (SO_RCVTIMEO/SO_SNDTIMEO's
// real POSIX meaning for an unset/zeroed value) -- never times out.
static int block_timed_out(u32_t start, long timeout_ms)
{
	return timeout_ms != 0 && (u32_t)(sys_now() - start) >= (u32_t)timeout_ms;
}

// O_NONBLOCK set on the socket, or MSG_DONTWAIT passed for this call.
static int dont_wait(const struct bsock *s, long flags)
{
	return s->nonblock || (flags & MSG_DONTWAIT);
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

	if (s->state == SK_CONNECTING)
		s->so_error = (err == ERR_RST) ? ECONNREFUSED : ETIMEDOUT;
	else
		s->so_error = (err == ERR_ABRT) ? ECONNABORTED : ECONNRESET;

	// lwIP has already freed the pcb by the time this fires.
	s->pcb = NULL;
	s->state = SK_ERROR;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
	struct bsock *s = arg;
	(void)pcb;
	if (err == ERR_OK) {
		s->state = SK_CONNECTED;
	} else {
		s->state = SK_ERROR;
		s->so_error = ECONNREFUSED;
	}
	return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
	struct bsock *listener = arg;

	if (err != ERR_OK || !newpcb)
		return ERR_VAL;
	// Refusing here makes lwIP reset the (already-handshaken)
	// connection. The listen backlog normally stops it getting this
	// far -- see below -- so this is only reached with the socket
	// table itself full.
	if (listener->acceptq_count >= ACCEPTQ_MAX)
		return ERR_MEM;

	int slot = alloc_slot();
	if (slot < 0)
		return ERR_MEM;

	// Keep counting this connection against the listener's backlog
	// until the app actually accept()s it (net_shim_accept()'s
	// tcp_backlog_accepted()), not just until the handshake finishes.
	// Once handshakes-in-progress plus this queue reach the backlog,
	// lwIP drops further SYNs and the client retransmits, as on Linux,
	// instead of a burst of connections overflowing acceptq here and
	// being reset.
	tcp_backlog_delayed(newpcb);

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
	s->nonblock = (type & SOCK_NONBLOCK) != 0;

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

// getsockname()/getpeername() -- found missing (OPENISSUES.md's
// Networking section doesn't call these out by name, PYTHON.md's Phase
// 2 does) while boot-testing socketserver.TCPServer.server_bind(),
// which calls getsockname() right after bind() to learn the port a
// bind to port 0 actually picked. No new state needed: tcp_pcb/udp_pcb
// (both built on lwIP's IP_PCB base) already track local_ip/local_port
// once bind() succeeds and remote_ip/remote_port once a connection is
// established -- this just reads them back out, the same
// pcb-field-to-sockaddr_in translation net_shim_accept() already does
// for the accepted connection's remote address.
long net_shim_getsockname(long fd, void *addr, socklen_t *addrlenp)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];
	if (!addr || !addrlenp || *addrlenp < (socklen_t)sizeof(struct sockaddr_in))
		return -EINVAL;

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	if (s->type == SOCK_DGRAM) {
		sin.sin_port = lwip_htons(s->upcb->local_port);
		sin.sin_addr.s_addr = s->upcb->local_ip.addr;
	} else if (s->pcb) { // NULL once reset (on_err())
		sin.sin_port = lwip_htons(s->pcb->local_port);
		sin.sin_addr.s_addr = s->pcb->local_ip.addr;
	}
	memcpy(addr, &sin, sizeof(sin));
	*addrlenp = sizeof(sin);

	return 0;
}

long net_shim_getpeername(long fd, void *addr, socklen_t *addrlenp)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];
	if (!addr || !addrlenp || *addrlenp < (socklen_t)sizeof(struct sockaddr_in))
		return -EINVAL;
	if (s->type != SOCK_STREAM || s->state != SK_CONNECTED)
		return -ENOTCONN;

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = lwip_htons(s->pcb->remote_port);
	sin.sin_addr.s_addr = s->pcb->remote_ip.addr;
	memcpy(addr, &sin, sizeof(sin));
	*addrlenp = sizeof(sin);

	return 0;
}

long net_shim_listen(long fd, long backlog)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->type != SOCK_STREAM)
		return -EOPNOTSUPP;

	// The requested backlog is deliberately not passed on. lwIP counts
	// handshakes still in progress against it, where Linux keeps those
	// in a separate SYN queue and applies the backlog only to finished,
	// not-yet-accept()ed connections -- so a burst of connections
	// bigger than a small listen() backlog would get SYNs dropped here
	// that Linux accepts. What lwIP gets instead is the most acceptq
	// can ever hold, so it never overflows (see on_accept()).
	(void)backlog;

	struct tcp_pcb *lpcb = tcp_listen_with_backlog(s->pcb, ACCEPTQ_MAX);
	if (!lpcb)
		return -EADDRINUSE;

	s->pcb = lpcb;
	s->state = SK_LISTENING;
	tcp_arg(lpcb, s);
	tcp_accept(lpcb, on_accept);

	return 0;
}

long net_shim_accept(long fd, void *addr, socklen_t *addrlenp, long flags)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->type != SOCK_STREAM)
		return -EOPNOTSUPP;

	u32_t start = sys_now();
	while (s->acceptq_count == 0) {
		if (s->state != SK_LISTENING)
			return -EINVAL;
		net_poll();
		if (s->acceptq_count != 0)
			break;
		if (s->nonblock)
			return -EAGAIN;
		if (block_timed_out(start, s->rcv_timeout_ms))
			return -EAGAIN;
		if (thread_shim_sleep_until(b_system(TIMECOUNTER, 0, 0) + NET_POLL_INTERVAL_NS) == -EINTR)
			return -EINTR;
	}

	int slot = s->acceptq[s->acceptq_head];
	s->acceptq_head = (s->acceptq_head + 1) % ACCEPTQ_MAX;
	s->acceptq_count--;

	struct bsock *ns = &socks[slot];
	ns->nonblock = (flags & SOCK_NONBLOCK) != 0; // not inherited from the listener, same as Linux

	// ns->pcb is NULL if the connection was reset while still queued
	// (on_err()) -- it's still handed over, as on Linux, and its first
	// recv()/send() reports the reset.
	if (ns->pcb)
		tcp_backlog_accepted(ns->pcb);

	if (addr && addrlenp && *addrlenp >= (socklen_t)sizeof(struct sockaddr_in)) {
		struct sockaddr_in sin;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		if (ns->pcb) {
			sin.sin_port = lwip_htons(ns->pcb->remote_port);
			sin.sin_addr.s_addr = ns->pcb->remote_ip.addr;
		}
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

	if (s->state == SK_CONNECTING)
		return -EALREADY;
	if (s->state == SK_CONNECTED)
		return -EISCONN;
	if (s->state != SK_CLOSED || !s->pcb)
		return -EINVAL;

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
		if (s->state != SK_CONNECTING)
			break;
		if (s->nonblock)
			return -EINPROGRESS; // outcome via POLLOUT/POLLERR + SO_ERROR
		if (block_timed_out(start, s->snd_timeout_ms))
			return -ETIMEDOUT;
		if (thread_shim_sleep_until(b_system(TIMECOUNTER, 0, 0) + NET_POLL_INTERVAL_NS) == -EINTR)
			return -EINTR;
	}

	if (s->state == SK_CONNECTED)
		return 0;
	long err = s->so_error ? s->so_error : ECONNREFUSED;
	s->so_error = 0;
	return -err;
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

// Pops the oldest queued datagram, scattering up to the iovecs' total
// capacity across them (extra bytes in an oversized datagram are
// discarded, per recv(2) UDP semantics -- *truncated is set if so) and
// reporting the sender's address if requested.
static long udp_do_recvv(struct bsock *s, const struct iovec *iov, size_t iovcnt, int *truncated, ip4_addr_t *addr_out, u16_t *port_out)
{
	struct udp_dgram *d = &s->udpq[s->udpq_head];
	struct pbuf *p = d->p;
	size_t n = 0;

	for (size_t i = 0; i < iovcnt && n < p->tot_len; i++) {
		size_t left = p->tot_len - n;
		size_t chunk = iov[i].iov_len < left ? iov[i].iov_len : left;
		pbuf_copy_partial(p, iov[i].iov_base, (u16_t)chunk, (u16_t)n);
		n += chunk;
	}

	if (truncated)
		*truncated = n < p->tot_len;
	if (addr_out)
		*addr_out = d->addr;
	if (port_out)
		*port_out = d->port;

	pbuf_free(p);
	s->udpq_head = (s->udpq_head + 1) % UDP_RXQ_MAX;
	s->udpq_count--;

	return (long)n;
}

static long udp_do_recv(struct bsock *s, void *buf, size_t len, ip4_addr_t *addr_out, u16_t *port_out)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	return udp_do_recvv(s, &iov, 1, NULL, addr_out, port_out);
}

static long udp_wait_rx(struct bsock *s, int nonblock)
{
	u32_t start = sys_now();
	while (s->udpq_count == 0) {
		net_poll();
		if (s->udpq_count != 0)
			break;
		if (nonblock)
			return -EAGAIN;
		if (block_timed_out(start, s->rcv_timeout_ms))
			return -ETIMEDOUT;
		if (thread_shim_sleep_until(b_system(TIMECOUNTER, 0, 0) + NET_POLL_INTERVAL_NS) == -EINTR)
			return -EINTR;
	}
	return 0;
}

// Blocks until a TCP socket has bytes queued (returns 1), has hit EOF
// (0), or has failed/timed out/been interrupted (negative errno) --
// or, if nonblock, returns -EAGAIN after one net_poll() pass instead
// of waiting.
static long tcp_wait_rx(struct bsock *s, int nonblock)
{
	u32_t start = sys_now();
	for (;;) {
		if (s->rx_head)
			return 1;
		if (s->eof)
			return 0;
		if (s->state == SK_ERROR)
			return -ECONNRESET;
		if (s->state == SK_CLOSED || s->state == SK_LISTENING)
			return -ENOTCONN;
		net_poll(); // SK_CONNECTING waits for the connection like Linux
		if (s->rx_head || s->eof || s->state == SK_ERROR)
			continue;
		if (nonblock)
			return -EAGAIN;
		if (block_timed_out(start, s->rcv_timeout_ms))
			return -ETIMEDOUT;
		if (thread_shim_sleep_until(b_system(TIMECOUNTER, 0, 0) + NET_POLL_INTERVAL_NS) == -EINTR)
			return -EINTR;
	}
}

long net_shim_recv(long fd, void *buf, size_t len, long flags)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->type == SOCK_DGRAM) {
		long r = udp_wait_rx(s, dont_wait(s, flags));
		if (r < 0)
			return r;
		return udp_do_recv(s, buf, len, NULL, NULL);
	}

	if (len == 0)
		return 0;

	long r = tcp_wait_rx(s, dont_wait(s, flags));
	if (r <= 0)
		return r;

	// Like recvmsg() below: once something is queued, hand back as much
	// of it as fits (bsock_read() only ever takes from one pbuf), never
	// blocking again.
	long total = 0;
	while ((size_t)total < len && s->rx_head)
		total += bsock_read(s, (char *)buf + total, len - total);
	return total;
}

long net_shim_recvfrom(long fd, void *buf, size_t len, long flags, void *addr, socklen_t *addrlenp)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->type != SOCK_DGRAM)
		return net_shim_recv(fd, buf, len, flags);

	long r = udp_wait_rx(s, dont_wait(s, flags));
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
// address given" semantics. The iovecs are gathered into a single
// datagram, per sendmsg(2).
static long udp_do_sendv(struct bsock *s, const struct iovec *iov, size_t iovcnt, const ip4_addr_t *dst, u16_t dst_port)
{
	size_t len = 0;
	for (size_t i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len > 0xFFFF - len)
			return -EMSGSIZE;
		len += iov[i].iov_len;
	}

	struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
	if (!p)
		return -ENOMEM;

	size_t off = 0;
	for (size_t i = 0; i < iovcnt; i++) {
		memcpy((char *)p->payload + off, iov[i].iov_base, iov[i].iov_len);
		off += iov[i].iov_len;
	}

	err_t e = dst ? udp_sendto(s->upcb, p, dst, dst_port) : udp_send(s->upcb, p);
	pbuf_free(p);

	if (e != ERR_OK)
		return e == ERR_MEM ? -ENOBUFS : (e == ERR_RTE ? -ENETUNREACH : -EIO);

	return (long)len;
}

static long udp_do_send(struct bsock *s, const void *buf, size_t len, const ip4_addr_t *dst, u16_t dst_port)
{
	struct iovec iov = { .iov_base = (void *)buf, .iov_len = len };
	return udp_do_sendv(s, &iov, 1, dst, dst_port);
}

long net_shim_send(long fd, const void *buf, size_t len, long flags)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (s->type == SOCK_DGRAM) {
		if (s->state != SK_CONNECTED)
			return -EDESTADDRREQ; // send() with no prior connect()
		return udp_do_send(s, buf, len, NULL, 0);
	}

	if (len == 0)
		return 0;
	if (s->state == SK_CLOSED || s->state == SK_LISTENING)
		return -ENOTCONN;

	// A send() while a connect() is still in progress waits for it
	// (or, non-blocking, fails with -EAGAIN), like Linux.
	int nonblock = dont_wait(s, flags);
	u32_t start = sys_now();
	for (int polled = 0;; polled = 1) {
		long n = (s->state == SK_CONNECTING) ? 0 : bsock_write(s, buf, len);
		if (n != 0)
			return n;
		if (s->state == SK_ERROR)
			return -ECONNRESET;
		if (nonblock && polled)
			return -EAGAIN; // still no room after one net_poll() pass
		net_poll();
		if (nonblock)
			continue;
		if (block_timed_out(start, s->snd_timeout_ms))
			return -ETIMEDOUT;
		if (thread_shim_sleep_until(b_system(TIMECOUNTER, 0, 0) + NET_POLL_INTERVAL_NS) == -EINTR)
			return -EINTR;
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

// sendmsg()/recvmsg(): the iovec forms of send()/recv() that Zig's
// std.Io.net (0.15+ writes, 0.17+ reads) and other runtimes use instead
// of write()/read(). No ancillary data is supported -- sending any is
// -EOPNOTSUPP, and receiving always reports msg_controllen = 0 -- and,
// as with send()/recv(), flags (MSG_NOSIGNAL, MSG_CMSG_CLOEXEC, ...)
// are accepted and ignored.
long net_shim_sendmsg(long fd, const struct msghdr *msg, long flags)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (msg->msg_controllen != 0)
		return -EOPNOTSUPP;

	if (s->type == SOCK_DGRAM) {
		if (!msg->msg_name) {
			if (s->state != SK_CONNECTED)
				return -EDESTADDRREQ;
			return udp_do_sendv(s, msg->msg_iov, msg->msg_iovlen, NULL, 0);
		}
		if (msg->msg_namelen < sizeof(struct sockaddr_in))
			return -EINVAL;
		const struct sockaddr_in *sin = msg->msg_name;
		ip4_addr_t ip;
		ip.addr = sin->sin_addr.s_addr;
		return udp_do_sendv(s, msg->msg_iov, msg->msg_iovlen, &ip, lwip_ntohs(sin->sin_port));
	}

	// TCP: same shape as sys_writev() -- each iovec through the
	// blocking send path, stopping early on a short write, and only
	// reporting an error if nothing was sent at all.
	long total = 0;
	for (size_t i = 0; i < (size_t)msg->msg_iovlen; i++) {
		const struct iovec *v = &msg->msg_iov[i];
		if (v->iov_len == 0)
			continue;
		long n = net_shim_send(fd, v->iov_base, v->iov_len, flags);
		if (n < 0)
			return total ? total : n;
		total += n;
		if ((size_t)n < v->iov_len)
			break;
	}
	return total;
}

long net_shim_recvmsg(long fd, struct msghdr *msg, long flags)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	msg->msg_controllen = 0;
	msg->msg_flags = 0;

	if (s->type == SOCK_DGRAM) {
		long r = udp_wait_rx(s, dont_wait(s, flags));
		if (r < 0)
			return r;

		ip4_addr_t src_ip;
		u16_t src_port;
		int truncated;
		long n = udp_do_recvv(s, msg->msg_iov, msg->msg_iovlen, &truncated, &src_ip, &src_port);
		if (truncated)
			msg->msg_flags |= MSG_TRUNC;

		if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_in)) {
			struct sockaddr_in sin;
			memset(&sin, 0, sizeof(sin));
			sin.sin_family = AF_INET;
			sin.sin_port = lwip_htons(src_port);
			sin.sin_addr.s_addr = src_ip.addr;
			memcpy(msg->msg_name, &sin, sizeof(sin));
			msg->msg_namelen = sizeof(sin);
		}
		return n;
	}

	// TCP: a connected stream has no per-message source address.
	msg->msg_namelen = 0;

	size_t cap = 0;
	for (size_t i = 0; i < (size_t)msg->msg_iovlen; i++)
		cap += msg->msg_iov[i].iov_len;
	if (cap == 0)
		return 0;

	// Block only until the first bytes arrive, then fill as many
	// iovecs as the already-queued data covers. Unlike sys_readv()'s
	// per-iovec read() loop, this never blocks again once it has
	// something to return -- a request that exactly fills the first
	// iovec mustn't stall waiting for bytes the peer will only send
	// after it gets a response.
	long r = tcp_wait_rx(s, dont_wait(s, flags));
	if (r <= 0)
		return r;

	long total = 0;
	for (size_t i = 0; i < (size_t)msg->msg_iovlen && s->rx_head; i++) {
		char *base = msg->msg_iov[i].iov_base;
		size_t len = msg->msg_iov[i].iov_len;
		size_t off = 0;
		while (off < len && s->rx_head)
			off += (size_t)bsock_read(s, base + off, len - off);
		total += (long)off;
	}
	return total;
}

// Only SO_RCVTIMEO/SO_SNDTIMEO actually do anything -- every other
// level/optname (SO_REUSEADDR, TCP_NODELAY, ...) is still an
// accept-and-ignore stub, same as before this existed (see
// posix_shim.c's sys_setsockopt()).
long net_shim_setsockopt(long fd, long level, long optname, const void *optval, long optlen)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	if (level != SOL_SOCKET || (optname != SO_RCVTIMEO && optname != SO_SNDTIMEO))
		return 0;
	if (!optval || (size_t)optlen < sizeof(struct timeval))
		return -EINVAL;

	const struct timeval *tv = optval;
	if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
		return -EINVAL;

	long ms = tv->tv_sec * 1000L + tv->tv_usec / 1000L;

	if (optname == SO_RCVTIMEO)
		s->rcv_timeout_ms = ms;
	else
		s->snd_timeout_ms = ms;

	return 0;
}

long net_shim_getsockopt(long fd, long level, long optname, void *optval, socklen_t *optlenp)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	// SO_ERROR is how a non-blocking connect()'s outcome is read back
	// once poll() reports the socket writable (or POLLERR); reading it
	// clears it, as on Linux.
	if (level == SOL_SOCKET && (optname == SO_ERROR || optname == SO_TYPE)) {
		if (!optval || !optlenp || (size_t)*optlenp < sizeof(int))
			return -EINVAL;
		int v;
		if (optname == SO_ERROR) {
			v = s->so_error;
			s->so_error = 0;
		} else {
			v = s->type;
		}
		memcpy(optval, &v, sizeof(v));
		*optlenp = sizeof(v);
		return 0;
	}

	if (level != SOL_SOCKET || (optname != SO_RCVTIMEO && optname != SO_SNDTIMEO))
		return 0;
	if (!optval || !optlenp || (size_t)*optlenp < sizeof(struct timeval))
		return -EINVAL;

	long ms = (optname == SO_RCVTIMEO) ? s->rcv_timeout_ms : s->snd_timeout_ms;

	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	memcpy(optval, &tv, sizeof(tv));
	*optlenp = sizeof(tv);

	return 0;
}

// fcntl() on a socket: only O_NONBLOCK is a real flag here.
// F_GETFL reports O_RDWR plus O_NONBLOCK if set; F_SETFL takes
// O_NONBLOCK and ignores everything else (O_APPEND/O_ASYNC/... have
// no meaning on this port's sockets). F_DUPFD* fail for the same
// reason posix_shim.c's non-socket fcntl() does -- there's no dup().
// Everything else (F_GETFD/F_SETFD's FD_CLOEXEC, ...) is accept-and-
// ignore.
long net_shim_fcntl(long fd, long cmd, long arg)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];

	switch (cmd) {
	case F_GETFL:
		return O_RDWR | (s->nonblock ? O_NONBLOCK : 0);
	case F_SETFL:
		s->nonblock = (arg & O_NONBLOCK) != 0;
		return 0;
	case F_DUPFD:
	case F_DUPFD_CLOEXEC:
		return -EINVAL;
	default:
		return 0;
	}
}

// ioctl() on a socket: FIONBIO (the other way to set O_NONBLOCK) and
// FIONREAD (bytes readable right now without blocking -- for UDP, the
// size of the next datagram, as on Linux).
long net_shim_ioctl(long fd, long req, long arg)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];
	int *p = (int *)arg;

	switch (req) {
	case FIONBIO:
		if (!p)
			return -EFAULT;
		s->nonblock = *p != 0;
		return 0;
	case FIONREAD:
		if (!p)
			return -EFAULT;
		if (s->state == SK_LISTENING)
			return -EINVAL;
		net_poll();
		if (s->type == SOCK_DGRAM)
			*p = s->udpq_count ? s->udpq[s->udpq_head].p->tot_len : 0;
		else
			*p = s->rx_head ? s->rx_head->tot_len - s->rx_off : 0;
		return 0;
	default:
		return -ENOTTY;
	}
}

// Drains the NIC and services lwIP's timers once, if any socket is
// open at all -- what posix_shim.c's poll()/select() call each time
// around their wait loop before checking net_shim_poll(). With no
// sockets there's nothing poll() could be waiting on here, and the
// netif may never have been brought up (net_ensure_ready() runs on the
// first socket() call).
void net_shim_pump(void)
{
	for (int i = 0; i < SOCK_MAX; i++) {
		if (socks[i].state != SK_FREE) {
			net_poll();
			return;
		}
	}
}

// Current readiness of a socket, as poll() revents for the requested
// events. Mirrors exactly what the calls above would do right now:
// POLLIN means recv()/accept() won't wait (data or EOF queued, a
// connection waiting, or a failure to report), POLLOUT means send()
// would accept at least one byte (or a pending connect() finished).
// POLLERR/POLLHUP are always reported regardless of events, as poll(2)
// specifies. Doesn't call net_poll() itself -- see net_shim_pump().
int net_shim_poll(long fd, int events)
{
	struct bsock *s = &socks[fd - SOCK_FD_BASE];
	int r = 0;

	if (s->type == SOCK_DGRAM) {
		if (s->udpq_count)
			r |= POLLIN | POLLRDNORM;
		r |= POLLOUT | POLLWRNORM;
	} else {
		switch (s->state) {
		case SK_LISTENING:
			if (s->acceptq_count)
				r |= POLLIN | POLLRDNORM;
			break;
		case SK_CONNECTED:
			if (s->rx_head || s->eof)
				r |= POLLIN | POLLRDNORM;
			if (s->eof)
				r |= POLLRDHUP;
			if (s->pcb && tcp_sndbuf(s->pcb) > 0 && tcp_sndqueuelen(s->pcb) < TCP_SND_QUEUELEN)
				r |= POLLOUT | POLLWRNORM;
			break;
		case SK_ERROR:
			// Readable (so a recv() surfaces the error, or any
			// data that arrived before it) and writable (so a
			// pending connect()'s caller wakes to read SO_ERROR).
			r |= POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM | POLLHUP;
			if (s->so_error)
				r |= POLLERR;
			break;
		case SK_CLOSED:
			r |= POLLOUT | POLLWRNORM | POLLHUP; // never connected, as Linux reports it
			break;
		default: // SK_CONNECTING
			break;
		}
	}

	return r & (events | POLLERR | POLLHUP);
}

// Tears down one already-accepted (tcp_accepted() called), not-yet-
// accept()ed connection sitting in a listener's acceptq -- same
// tcp_close()-falling-back-to-tcp_abort()-plus-rx_head-drain sequence
// net_shim_close() below runs for a normal connected socket, just
// applied directly to the queued bsock slot rather than through an fd.
// ns->pcb can already be NULL here if on_err() fired while this
// connection was still sitting in the queue (lwIP frees the pcb itself
// before that callback runs) -- nothing left to close in that case.
static void close_queued_conn(struct bsock *ns)
{
	if (ns->pcb) {
		tcp_arg(ns->pcb, NULL);
		tcp_recv(ns->pcb, NULL);
		tcp_err(ns->pcb, NULL);
		if (tcp_close(ns->pcb) != ERR_OK)
			tcp_abort(ns->pcb);
	}

	while (ns->rx_head) {
		struct pbuf *rest = ns->rx_head->next;
		ns->rx_head->next = NULL;
		pbuf_free(ns->rx_head);
		ns->rx_head = rest;
	}

	ns->state = SK_FREE;
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
		if (s->state == SK_LISTENING) {
			tcp_accept(s->pcb, NULL);
			// Connections lwIP already handed us (on_accept()
			// called tcp_accepted()) but the app never got
			// around to accept()ing -- see close_queued_conn()
			// above. Without this, both the tcp_pcb and the
			// bsock slot on_accept() allocated for it would be
			// leaked for good: nothing else ever revisits an
			// acceptq entry once its listener is gone.
			while (s->acceptq_count > 0) {
				int slot = s->acceptq[s->acceptq_head];
				s->acceptq_head = (s->acceptq_head + 1) % ACCEPTQ_MAX;
				s->acceptq_count--;
				close_queued_conn(&socks[slot]);
			}
		} else {
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

	s->state = SK_FREE;
	return 0;
}

// =============================================================================
// EOF
