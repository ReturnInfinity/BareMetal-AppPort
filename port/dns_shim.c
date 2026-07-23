// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// dns_shim.c -- provides the standard gethostbyname() (declared in
// <netdb.h>) backed by lwIP's raw dns_gethostbyname(), the same
// callback-plus-net_poll()-loop pattern net_shim.c uses for TCP/UDP.
//
// This deliberately shadows musl's own gethostbyname() rather than
// leaving it in place: musl's version reads /etc/resolv.conf for
// server addresses (nothing writes that file on this port's BMFS
// image, so it'd fall back to querying 127.0.0.1, which nothing
// listens on) instead of the DNS servers net_glue.c already
// configures from the Firecracker "ip=" param, DHCP, or the
// 8.8.8.8/1.1.1.1 fallback. Since this object is linked ahead of
// libc.a (see build-app.sh), the linker resolves gethostbyname()
// against this definition and never pulls musl's in.
//
// Scope: gethostbyname() only -- no gethostbyname_r(), no
// getaddrinfo(), no IPv6/AF_UNSPEC (LWIP_IPV6=0 anyway).
// =============================================================================

#include <netdb.h>
#include <string.h>
#include <sys/socket.h>

#include "lwip/dns.h"
#include "lwip/sys.h"

#include "net_glue.h"

#define DNS_BLOCK_TIMEOUT_MS 30000

struct dns_wait {
	int done;
	int ok;
	ip4_addr_t addr;
};

static void on_found(const char *name, const ip_addr_t *ipaddr, void *arg)
{
	(void)name;
	struct dns_wait *w = arg;

	if (ipaddr) {
		w->addr = *ipaddr;
		w->ok = 1;
	}
	w->done = 1;
}

// Blocks (like every other net_*/dns_* call in this port) until the
// name resolves, fails, or DNS_BLOCK_TIMEOUT_MS elapses.
static int resolve(const char *hostname, ip4_addr_t *out)
{
	net_ensure_ready();

	struct dns_wait w = { 0 };
	err_t e = dns_gethostbyname(hostname, out, on_found, &w);

	if (e == ERR_OK)
		return 0; // already cached, or hostname was itself an IP literal
	if (e != ERR_INPROGRESS)
		return -1;

	u32_t start = sys_now();
	while (!w.done) {
		net_poll();
		if ((u32_t)(sys_now() - start) >= DNS_BLOCK_TIMEOUT_MS)
			return -1;
	}

	if (!w.ok)
		return -1;

	*out = w.addr;
	return 0;
}

struct hostent *gethostbyname(const char *name)
{
	// Single-threaded unikernel, no concurrent callers to race --
	// same assumption net_shim.c's fixed socket table already
	// relies on -- so static storage (as the real gethostbyname()
	// contract itself requires; that's what gethostbyname_r() is
	// for) is fine here.
	static char name_buf[256];
	static ip4_addr_t addr;
	static char *addr_list[2];
	static char *alias_list[1];
	static struct hostent he;

	if (resolve(name, &addr) != 0) {
		h_errno = HOST_NOT_FOUND;
		return NULL;
	}

	strncpy(name_buf, name, sizeof(name_buf) - 1);
	name_buf[sizeof(name_buf) - 1] = '\0';

	addr_list[0] = (char *)&addr;
	addr_list[1] = NULL;
	alias_list[0] = NULL;

	he.h_name = name_buf;
	he.h_aliases = alias_list;
	he.h_addrtype = AF_INET;
	he.h_length = sizeof(addr);
	he.h_addr_list = addr_list;

	return &he;
}

// =============================================================================
// EOF
