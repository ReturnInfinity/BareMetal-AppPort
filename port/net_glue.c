// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// net_glue.c -- wires lwIP's Ethernet netif to the one raw NIC
// BareMetal exposes (b_net_tx/b_net_rx, interface id 0 -- see
// libBareMetal.h). There is no OS here for lwIP to run its own
// tcpip.c thread on, so this also stands in for that: net_poll()
// drains pending RX frames and services lwIP's timers, and is meant
// to be called from net_shim.c's blocking socket loops rather than
// from any kind of background thread.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"

#include "libBareMetal.h"
#include "net_glue.h"

#define BMOS_NET_IID 0

// Firecracker (via its "boot from raw kernel args" path) writes the
// whole Linux kernel command line here for guests that have no other
// way to read it, e.g.:
//   console=ttyS0 reboot=k panic=1 pci=off ip=172.16.0.2::172.16.0.1:255.255.0.0::eth0:off root=/dev/vda rw
// so "ip=" has to be found as a token amid the other params, not
// assumed to be a prefix of the buffer. Its own format (see Linux's
// Documentation/admin-guide/nfs/nfsroot.rst) is:
//   ip=<client-ip>:<server-ip>:<gw-ip>:<netmask>:<hostname>:<device>:<autoconf>
// plus two more (non-standard) fields some distros' initrds append:
//   :<dns0-ip>:<dns1-ip>
// client-ip, gw-ip, and netmask are required; dns0-ip/dns1-ip are
// optional. When present and parseable, this replaces DHCP entirely.
#define FC_IP_PARAM_ADDR ((const char *)0x5a00UL)
#define FC_IP_PARAM_MAXLEN 256

// Used whenever neither the Firecracker "ip=" param nor DHCP hands us
// a DNS server: public resolvers, so lookups still work rather than
// failing outright with no DNS configured at all.
#define FALLBACK_DNS0 "8.8.8.8"
#define FALLBACK_DNS1 "1.1.1.1"

static struct netif bmos_netif;

unsigned int bmos_lwip_rand(void)
{
	return (unsigned int)rand();
}

// b_system(TIMECOUNTER, ...) returns nanoseconds (see
// src/BareMetal/drivers/timer.asm's kvm_ns); sys_now() wants
// milliseconds. Wraps every ~49 days, which is fine -- lwIP only
// ever compares differences of sys_now(), never treats it as
// absolute.
u32_t sys_now(void)
{
	return (u32_t)(b_system(TIMECOUNTER, 0, 0) / 1000000ULL);
}

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
	(void)netif;

	// b_net_tx() takes one contiguous buffer; a pbuf can be a chain
	// (e.g. lwIP building a TCP segment as a separate header pbuf +
	// referenced payload), so linearize it first. 1522 = max
	// Ethernet frame size the kernel accepts (see net.asm).
	static unsigned char txbuf[1522];
	if (p->tot_len > sizeof(txbuf))
		return ERR_BUF;

	pbuf_copy_partial(p, txbuf, p->tot_len, 0);
	b_net_tx(txbuf, p->tot_len, BMOS_NET_IID);

	return ERR_OK;
}

static err_t bmos_netif_init(struct netif *netif)
{
	netif->name[0] = 'e';
	netif->name[1] = 'n';
	netif->output = etharp_output;
	netif->linkoutput = low_level_output;
	netif->mtu = 1500;
	netif->hwaddr_len = ETH_HWADDR_LEN;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

	// b_system(NET_STATUS, 0, iid) returns the MAC in bits 47-0 of
	// RAX, most significant octet first (see b_net_status in
	// net.asm: bswap + shift leaves byte0 in bits 47-40, ...,
	// byte5 in bits 7-0).
	u64 mac = b_system(NET_STATUS, 0, BMOS_NET_IID);
	netif->hwaddr[0] = (mac >> 40) & 0xFF;
	netif->hwaddr[1] = (mac >> 32) & 0xFF;
	netif->hwaddr[2] = (mac >> 24) & 0xFF;
	netif->hwaddr[3] = (mac >> 16) & 0xFF;
	netif->hwaddr[4] = (mac >> 8) & 0xFF;
	netif->hwaddr[5] = mac & 0xFF;

	return ERR_OK;
}

// Copies the NUL-terminated string at FC_IP_PARAM_ADDR (capped at
// FC_IP_PARAM_MAXLEN, in case it's uninitialized/non-Firecracker memory
// with no NUL in range) and parses the "ip=" token out of it as a
// Linux-style kernel command line. Returns 1 and fills *ip/*gw/*mask
// on success, 0 otherwise (no "ip=" token, or client-ip/gw-ip/netmask
// missing or unparseable).
//
// *dns0/*dns1 are always zeroed first and only filled in if the
// cmdline's optional dns0-ip/dns1-ip fields (the 8th/9th ':'-separated
// fields -- an extension some distros' initrds add on top of the base
// nfsroot ip= format) are present and parseable; the caller treats a
// zeroed (any) address as "not provided" the same way it does for a
// DHCP lease that came with no DNS option.
static int fc_parse_ip_param(ip4_addr_t *ip, ip4_addr_t *gw, ip4_addr_t *mask, ip4_addr_t *dns0, ip4_addr_t *dns1)
{
	const char *src = FC_IP_PARAM_ADDR;
	char buf[FC_IP_PARAM_MAXLEN];
	size_t i;

	ip4_addr_set_zero(dns0);
	ip4_addr_set_zero(dns1);

	for (i = 0; i < sizeof(buf) - 1 && src[i] != '\0'; i++)
		buf[i] = src[i];
	buf[i] = '\0';

	printf("net: fc cmdline @ %p: \"%s\"\n", (const void *)src, buf);

	// "ip=" can appear anywhere among the other kernel params (e.g.
	// "console=ttyS0 ... ip=172.16.0.2::172.16.0.1:255.255.0.0::eth0:off
	// pci=off root=..."), so find it as a whole token -- at the start
	// of the line, or preceded by whitespace -- rather than assuming
	// it's a prefix of the buffer.
	char *tok = buf;
	for (;;) {
		tok = strstr(tok, "ip=");
		if (tok == NULL || tok == buf || tok[-1] == ' ')
			break;
		tok++;
	}

	if (tok == NULL) {
		printf("net: fc cmdline: no \"ip=\" token found, falling back to DHCP\n");
		return 0;
	}

	// Split on ':' in place: fields[0]=client-ip, [1]=server-ip,
	// [2]=gw-ip, [3]=netmask, [4]=hostname, [5]=device, [6]=autoconf,
	// [7]=dns0-ip, [8]=dns1-ip (the last two are a non-standard
	// extension some distros' initrds add on top of the base
	// nfsroot ip= format -- optional, see the fc_parse_ip_param()
	// comment above) (plus whatever follows in the cmdline after
	// that -- unused).
	char *fields[9] = { 0 };
	char *p = tok + 3;
	int n = 0;

	fields[n++] = p;
	while (n < 9 && (p = strchr(p, ':')) != NULL) {
		*p = '\0';
		p++;
		fields[n++] = p;
	}

	if (n < 4) {
		printf("net: fc ip param: only %d field(s), need at least 4 (client-ip:server-ip:gw-ip:netmask), falling back to DHCP\n", n);
		return 0;
	}

	printf("net: fc ip param fields: client-ip=\"%s\" server-ip=\"%s\" gw-ip=\"%s\" netmask=\"%s\"\n",
		fields[0], fields[1], fields[2], fields[3]);

	if (fields[0][0] == '\0' || fields[2][0] == '\0' || fields[3][0] == '\0') {
		printf("net: fc ip param: client-ip/gw-ip/netmask empty, falling back to DHCP\n");
		return 0;
	}

	if (!ip4addr_aton(fields[0], ip)) {
		printf("net: fc ip param: client-ip \"%s\" unparseable, falling back to DHCP\n", fields[0]);
		return 0;
	}
	if (!ip4addr_aton(fields[2], gw)) {
		printf("net: fc ip param: gw-ip \"%s\" unparseable, falling back to DHCP\n", fields[2]);
		return 0;
	}
	if (!ip4addr_aton(fields[3], mask)) {
		printf("net: fc ip param: netmask \"%s\" unparseable, falling back to DHCP\n", fields[3]);
		return 0;
	}

	// dns0-ip/dns1-ip are optional -- an unparseable or absent one just
	// leaves the corresponding *dns0/*dns1 zeroed (already done above),
	// it doesn't fail the whole "ip=" param the way client-ip/gw-ip/
	// netmask do.
	if (n > 7 && fields[7][0] != '\0' && ip4addr_aton(fields[7], dns0))
		printf("net: fc ip param: dns0-ip=\"%s\"\n", fields[7]);
	if (n > 8 && fields[8][0] != '\0' && ip4addr_aton(fields[8], dns1))
		printf("net: fc ip param: dns1-ip=\"%s\"\n", fields[8]);

	return 1;
}

void net_init(void)
{
	srand((unsigned int)b_system(TIMECOUNTER, 0, 0));

	lwip_init();

	ip4_addr_t ip, gw, mask, dns0, dns1;
	if (fc_parse_ip_param(&ip, &gw, &mask, &dns0, &dns1)) {
		// ip4addr_ntoa() returns a pointer to a single shared static
		// buffer, so each address needs its own buffer here -- calling
		// it three times as inline printf args would just print the
		// last address three times.
		char ip_s[IP4ADDR_STRLEN_MAX], gw_s[IP4ADDR_STRLEN_MAX], mask_s[IP4ADDR_STRLEN_MAX];
		ip4addr_ntoa_r(&ip, ip_s, sizeof(ip_s));
		ip4addr_ntoa_r(&gw, gw_s, sizeof(gw_s));
		ip4addr_ntoa_r(&mask, mask_s, sizeof(mask_s));
		printf("net: using fc static address ip=%s gw=%s mask=%s\n", ip_s, gw_s, mask_s);
		netif_add(&bmos_netif, &ip, &mask, &gw,
			NULL, bmos_netif_init, ethernet_input);
		netif_set_default(&bmos_netif);
		netif_set_up(&bmos_netif);

		// DHCP's own DNS options are applied as part of dhcp_bind() (see
		// dhcp_handle_ack() in lwIP), so this static path is the one
		// case where nothing else sets these -- do it here rather than
		// leaving it to net_wait_ready()'s fallback, so a cmdline-
		// provided DNS server is preferred over the 8.8.8.8/1.1.1.1
		// fallback.
		if (!ip4_addr_isany_val(dns0))
			dns_setserver(0, &dns0);
		if (!ip4_addr_isany_val(dns1))
			dns_setserver(1, &dns1);
	} else {
		netif_add(&bmos_netif, IP4_ADDR_ANY, IP4_ADDR_ANY, IP4_ADDR_ANY,
			NULL, bmos_netif_init, ethernet_input);
		netif_set_default(&bmos_netif);
		netif_set_up(&bmos_netif);

		dhcp_start(&bmos_netif);
	}
}

void net_poll(void)
{
	void *buf;
	u64 len;

	// b_net_rx() never blocks -- it returns len=0 immediately when
	// nothing is pending (see net_virtio_mmio_poll) -- so this drains
	// whatever's queued and returns rather than waiting.
	while ((len = b_net_rx(&buf, BMOS_NET_IID)) != 0) {
		struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_RAM);
		if (p) {
			pbuf_take(p, buf, (u16_t)len);
			if (bmos_netif.input(p, &bmos_netif) != ERR_OK)
				pbuf_free(p);
		}
	}

	sys_check_timeouts();
}

int net_is_ready(void)
{
	// Covers both paths: DHCP sets the netif's IPv4 address once bound,
	// and the static Firecracker path sets it immediately in net_init().
	return netif_is_up(&bmos_netif) && !ip4_addr_isany_val(*netif_ip4_addr(&bmos_netif));
}

// Fills in whichever of the two DNS server slots is still unset (i.e.
// neither the fc "ip=" param nor a DHCP lease's DNS option populated
// it) with a public resolver, so name resolution still works rather
// than silently having no DNS server configured at all. Per-slot
// rather than all-or-nothing, so e.g. a DHCP lease that gave exactly
// one DNS server keeps it in slot 0 and only slot 1 gets a fallback.
static void dns_apply_fallback(void)
{
	ip4_addr_t fallback;

	if (ip4_addr_isany_val(*dns_getserver(0)) && ip4addr_aton(FALLBACK_DNS0, &fallback)) {
		printf("net: no DNS server provided by fc/DHCP, falling back to %s\n", FALLBACK_DNS0);
		dns_setserver(0, &fallback);
	}
	if (ip4_addr_isany_val(*dns_getserver(1)) && ip4addr_aton(FALLBACK_DNS1, &fallback)) {
		printf("net: no DNS server provided by fc/DHCP, falling back to %s\n", FALLBACK_DNS1);
		dns_setserver(1, &fallback);
	}
}

int net_wait_ready(unsigned timeout_ms)
{
	u32_t start = sys_now();

	while (!net_is_ready()) {
		net_poll();
		if ((u32_t)(sys_now() - start) >= timeout_ms)
			return 0;
	}

	dns_apply_fallback();

	return 1;
}

const char *net_ip_str(void)
{
	return ip4addr_ntoa(netif_ip4_addr(&bmos_netif));
}

#define NET_ENSURE_TIMEOUT_MS 30000

static int net_ready;

void net_ensure_ready(void)
{
	if (net_ready)
		return;
	net_init();
	net_wait_ready(NET_ENSURE_TIMEOUT_MS);
	net_ready = 1;
}

// =============================================================================
// EOF
