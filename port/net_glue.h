#ifndef _NET_GLUE_H
#define _NET_GLUE_H

// Bring up the (single) network interface: query the MAC address,
// register with lwIP, and start the DHCP client.
void net_init(void);

// Drain any pending RX frames into lwIP and service lwIP's timers.
// Must be called frequently by anything that blocks on the network
// -- see net_shim.c.
void net_poll(void);

// True once DHCP has bound an address.
int net_is_ready(void);

// Spin net_poll() until DHCP binds or timeout_ms elapses. Returns 1
// if bound, 0 on timeout.
int net_wait_ready(unsigned timeout_ms);

// The DHCP-assigned address as a string (e.g. "172.19.0.42"), or
// "0.0.0.0" if not yet bound.
const char *net_ip_str(void);

#endif
