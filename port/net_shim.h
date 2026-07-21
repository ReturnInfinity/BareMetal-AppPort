#ifndef _NET_SHIM_H
#define _NET_SHIM_H

#include <stddef.h>
#include <sys/socket.h>

// True if fd refers to an open socket (as opposed to a std fd 0-2 or
// a BMFS fd -- see bmfs.h).
int net_shim_is_fd(long fd);

long net_shim_socket(long domain, long type, long protocol);
long net_shim_bind(long fd, const void *addr, long addrlen);
long net_shim_listen(long fd, long backlog);
long net_shim_accept(long fd, void *addr, socklen_t *addrlen);
long net_shim_connect(long fd, const void *addr, long addrlen);
long net_shim_send(long fd, const void *buf, size_t len, long flags);
long net_shim_recv(long fd, void *buf, size_t len, long flags);
long net_shim_close(long fd);

#endif
