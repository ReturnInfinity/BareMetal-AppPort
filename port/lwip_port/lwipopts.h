#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

// lwIP config for the BareMetal-Firecracker port. Single NIC
// (virtio-net, iid 0), single-threaded, no preemption -- see
// net_glue.c/net_shim.c for how this gets driven (our own blocking
// poll loop, not lwIP's own tcpip.c/sockets.c, which need real OS
// threads we don't have).

// ---- No OS ----
// We drive everything from our own poll loop (net_glue.c); there is
// no tcpip.c thread, no mailboxes, no semaphores.
#define NO_SYS                     1
#define LWIP_NETCONN               0
#define LWIP_SOCKET                0
#define LWIP_TCPIP_CORE_LOCKING    0
// Only one execution context ever touches lwIP's state (our own
// synchronous poll loop -- see net_glue.c/net_shim.c), so the
// inter-task/task-vs-interrupt critical sections this guards don't
// apply; leaving it enabled would require us to implement
// sys_arch_protect()/sys_arch_unprotect() for no benefit.
#define SYS_LIGHTWEIGHT_PROT       0

// ---- Memory: route everything through musl's malloc/free (our own
// brk/mmap-backed heap from Phase 3) instead of lwIP's own static
// pools, so pbuf/pcb/etc. sizing isn't a second thing to tune. ----
#define MEM_LIBC_MALLOC            1
#define MEMP_MEM_MALLOC            1
#define MEM_ALIGNMENT              4

// ---- Protocols ----
#define LWIP_IPV4                  1
#define LWIP_IPV6                  0
#define LWIP_ARP                   1
#define LWIP_ETHERNET              1
#define LWIP_RAW                   0
#define LWIP_ICMP                  1
#define LWIP_IGMP                  0
#define IP_FORWARD                 0

#define LWIP_TCP                   1
#define LWIP_UDP                   1 // needed internally by the DHCP client

#define LWIP_DHCP                  1
#define LWIP_DHCP_DOES_ACD_CHECK   0 // skip acd.c: one less thing to vendor/build
#define LWIP_AUTOIP                0
#define LWIP_DNS                   0

// ---- Buffer/window sizing, kept modest: app RAM here is whatever
// the microVM was configured with (often just a few MiB -- see
// Phase 3's b_system(FREE_MEMORY,...) heap sizing). ----
#define TCP_MSS                    1460
#define TCP_WND                    (4 * TCP_MSS)
#define TCP_SND_BUF                (4 * TCP_MSS)
#define TCP_SND_QUEUELEN           ((4 * TCP_SND_BUF) / TCP_MSS)

// Our netif RX path always allocates PBUF_RAM (malloc-backed) pbufs
// sized to the actual frame, not a fixed PBUF_POOL -- see
// net_glue.c's netif poll function -- so PBUF_POOL_SIZE is unused.
#define PBUF_POOL_SIZE             0

#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK   0
#define LWIP_NETIF_HOSTNAME        0
#define LWIP_STATS                 0
#define LWIP_CHECKSUM_ON_COPY      1

#define LWIP_COMPAT_MUTEX          0

#endif
