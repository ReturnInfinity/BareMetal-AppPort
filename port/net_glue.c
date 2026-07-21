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

#include <stdlib.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"

#include "libBareMetal.h"
#include "net_glue.h"

#define BMOS_NET_IID 0

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

void net_init(void)
{
	srand((unsigned int)b_system(TIMECOUNTER, 0, 0));

	lwip_init();

	netif_add(&bmos_netif, IP4_ADDR_ANY, IP4_ADDR_ANY, IP4_ADDR_ANY,
		NULL, bmos_netif_init, ethernet_input);
	netif_set_default(&bmos_netif);
	netif_set_up(&bmos_netif);

	dhcp_start(&bmos_netif);
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
	return dhcp_supplied_address(&bmos_netif) != 0;
}

int net_wait_ready(unsigned timeout_ms)
{
	u32_t start = sys_now();

	while (!net_is_ready()) {
		net_poll();
		if ((u32_t)(sys_now() - start) >= timeout_ms)
			return 0;
	}

	return 1;
}

const char *net_ip_str(void)
{
	return ip4addr_ntoa(netif_ip4_addr(&bmos_netif));
}

// =============================================================================
// EOF
