#ifndef _NET_GLUE_H
#define _NET_GLUE_H

// Bring up the (single) network interface: query the MAC address,
// register with lwIP, and configure an address. If a Firecracker-style
// "ip=" boot param string is found at a fixed memory address, that
// static address/gateway/netmask is used directly; otherwise falls
// back to starting the DHCP client.
void net_init(void);

// Drain any pending RX frames into lwIP and service lwIP's timers.
// Must be called frequently by anything that blocks on the network
// -- see net_shim.c.
void net_poll(void);

// True once the netif has an address (statically configured, or DHCP
// has bound one).
int net_is_ready(void);

// Spin net_poll() until an address is ready or timeout_ms elapses.
// Returns 1 if ready, 0 on timeout.
int net_wait_ready(unsigned timeout_ms);

// Lazily brings the interface up and waits for DHCP the first time
// anything actually needs the network (net_shim.c's first socket()
// call, or dns_shim.c's first name lookup), rather than doing it
// unconditionally at process startup -- most programs never touch
// the network, and DHCP takes real time. Safe to call repeatedly
// (and from more than one of those first-touch call sites) -- only
// the first call does any work.
void net_ensure_ready(void);

// The netif's address as a string (e.g. "172.19.0.42"), or "0.0.0.0"
// if not yet configured.
const char *net_ip_str(void);

#endif
