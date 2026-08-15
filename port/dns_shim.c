// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// dns_shim.c -- provides the standard gethostbyname()/getaddrinfo()/
// getnameinfo() (declared in <netdb.h>) backed by lwIP's raw
// dns_gethostbyname(), the same callback-plus-net_poll()-loop pattern
// net_shim.c uses for TCP/UDP.
//
// This deliberately shadows musl's own versions rather than leaving
// them in place: musl's getaddrinfo()/gethostbyname() read
// /etc/resolv.conf for server addresses (nothing writes that file on
// this port's EXT2 image, so it'd fall back to querying 127.0.0.1,
// which nothing listens on) instead of the DNS servers net_glue.c
// already configures from the Firecracker "ip=" param, DHCP, or the
// 8.8.8.8/1.1.1.1 fallback. Since this object is linked ahead of
// libc.a (see build-app.sh), the linker resolves all three against
// these definitions and never pulls musl's in -- which also means
// freeaddrinfo() has to be shadowed too (see its own comment below):
// leaving musl's real one linked in would free() our results as if
// they came from musl's own getaddrinfo() allocator.
//
// Scope: no gethostbyname_r(), no IPv6/AF_UNSPEC (LWIP_IPV6=0
// anyway). getaddrinfo() always returns a single addrinfo node --
// real implementations can return one per ai_socktype when the
// caller leaves hints.ai_socktype at 0 (e.g. one SOCK_STREAM and one
// SOCK_DGRAM); this port has no use for that and defaults an
// unspecified socktype to SOCK_STREAM instead. getnameinfo() only
// ever produces numeric host/serv strings: reverse (PTR) DNS lookups
// would need lwIP's lower-level dns_query() API rather than the
// dns_gethostbyname() this file already builds on, and nothing in
// this port's apps needs a hostname back from an address.
// =============================================================================

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
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

	// Static, not a stack local: &w is handed to lwIP as a callback
	// context that outlives this call in lwIP's own bookkeeping --
	// dns_gethostbyname()'s ERR_INPROGRESS path means lwIP has
	// queued the query and will call on_found() whenever *it*
	// decides the query is done (resolved, or lwIP's own internal
	// retry/timeout gives up), which is not bounded by our
	// DNS_BLOCK_TIMEOUT_MS wait loop below. If our loop gives up
	// first, this function returns and a stack-local w's frame would
	// be gone by the time that late callback fires -- writing
	// through a dangling pointer into whatever now occupies that
	// stack slot. Static storage means a late callback just harmlessly
	// writes into memory nobody's reading anymore instead.
	static struct dns_wait w;
	memset(&w, 0, sizeof(w));
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

// Numeric-literal check ahead of a real resolve() call, same "AI_NUMERICHOST
// must not touch the network" contract getaddrinfo(3) documents. Deliberately
// reuses ip4addr_aton()'s permissive legacy-BSD parsing (accepts "10",
// "0x7f.1" etc, not just strict a.b.c.d) rather than net_glue.c's stricter
// is_dotted_quad() -- resolve()/gethostbyname() above already accept
// whatever ip4addr_aton() accepts (dns_gethostbyname() checks it internally
// before ever queuing a query), so getaddrinfo() being pickier about what
// counts as "numeric" than gethostbyname() is inconsistent for no benefit.
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
	static struct addrinfo ai;
	static struct sockaddr_in sa;
	static char canon_buf[256];

	int flags = hints ? hints->ai_flags : 0;
	int family = hints ? hints->ai_family : AF_UNSPEC;

	if (family != AF_UNSPEC && family != AF_INET)
		return EAI_FAMILY; // no IPv6 anywhere on this port (LWIP_IPV6=0)
	if (!node && !service)
		return EAI_NONAME;

	// No /etc/services on this port's EXT2 image -- numeric ports only.
	unsigned long port = 0;
	if (service && *service) {
		char *end;
		port = strtoul(service, &end, 10);
		if (*end != '\0' || port > 65535)
			return EAI_SERVICE;
	}

	ip4_addr_t addr;
	if (!node) {
		// AI_PASSIVE (bind/listen): "any" address. Otherwise (connect
		// with no explicit host): loopback, per getaddrinfo(3).
		if (flags & AI_PASSIVE)
			ip4_addr_set_any(&addr);
		else if (!ip4addr_aton("127.0.0.1", &addr))
			return EAI_FAIL;
	} else if (!ip4addr_aton(node, &addr)) {
		if (flags & AI_NUMERICHOST)
			return EAI_NONAME;
		if (resolve(node, &addr) != 0)
			return EAI_NONAME;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = addr.addr;

	memset(&ai, 0, sizeof(ai));
	ai.ai_family = AF_INET;
	ai.ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
	ai.ai_protocol = hints ? hints->ai_protocol : 0;
	ai.ai_addrlen = sizeof(sa);
	ai.ai_addr = (struct sockaddr *)&sa;

	if (flags & AI_CANONNAME) {
		strncpy(canon_buf, node ? node : "0.0.0.0", sizeof(canon_buf) - 1);
		canon_buf[sizeof(canon_buf) - 1] = '\0';
		ai.ai_canonname = canon_buf;
	}

	*res = &ai;
	return 0;
}

// getaddrinfo() above never malloc()s -- it hands back a pointer to static
// storage, the same "single-threaded unikernel, no concurrent callers to
// race" rationale gethostbyname()'s static storage already relies on. This
// must still shadow musl's real freeaddrinfo(): that one assumes the
// lookup.h "struct aibuf" layout musl's own getaddrinfo() allocates (one
// malloc'd block with a refcount header ahead of the addrinfo array) and
// walks backwards from the pointer it's given to find that header --
// running it over our static struct addrinfo would read/underflow memory
// that was never part of any such block.
void freeaddrinfo(struct addrinfo *ai)
{
	(void)ai;
}

// Numeric-only: see this file's header for why reverse (PTR) lookups aren't
// implemented. NI_NAMEREQD asks for a real hostname or failure, and a real
// hostname is exactly what this can't produce -- so it fails as specified
// rather than silently returning a numeric string in place of a name.
int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags)
{
	if (sa->sa_family != AF_INET || salen < sizeof(struct sockaddr_in))
		return EAI_FAMILY;

	const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

	if (host && hostlen) {
		if (flags & NI_NAMEREQD)
			return EAI_NONAME;
		if (!inet_ntop(AF_INET, &sin->sin_addr, host, hostlen))
			return EAI_OVERFLOW;
	}

	if (serv && servlen) {
		char buf[6];
		int n = snprintf(buf, sizeof(buf), "%u", ntohs(sin->sin_port));
		if (n < 0 || (size_t)n >= servlen)
			return EAI_OVERFLOW;
		memcpy(serv, buf, (size_t)n + 1);
	}

	return 0;
}

// =============================================================================
// EOF
