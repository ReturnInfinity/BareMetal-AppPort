#ifndef _NET_SHIM_H
#define _NET_SHIM_H

#include <stddef.h>
#include <sys/socket.h>

// True if fd refers to an open socket (as opposed to a std fd 0-2 or
// an EXT2 fd -- see ext4_shim.h).
int net_shim_is_fd(long fd);

long net_shim_socket(long domain, long type, long protocol);
long net_shim_bind(long fd, const void *addr, long addrlen);
long net_shim_listen(long fd, long backlog);
long net_shim_accept(long fd, void *addr, socklen_t *addrlen, long flags);
long net_shim_connect(long fd, const void *addr, long addrlen);
long net_shim_getsockname(long fd, void *addr, socklen_t *addrlen);
long net_shim_getpeername(long fd, void *addr, socklen_t *addrlen);
long net_shim_send(long fd, const void *buf, size_t len, long flags);
long net_shim_recv(long fd, void *buf, size_t len, long flags);
long net_shim_sendto(long fd, const void *buf, size_t len, long flags, const void *addr, long addrlen);
long net_shim_recvfrom(long fd, void *buf, size_t len, long flags, void *addr, socklen_t *addrlenp);
long net_shim_sendmsg(long fd, const struct msghdr *msg, long flags);
long net_shim_recvmsg(long fd, struct msghdr *msg, long flags);
long net_shim_close(long fd);
long net_shim_setsockopt(long fd, long level, long optname, const void *optval, long optlen);
long net_shim_getsockopt(long fd, long level, long optname, void *optval, socklen_t *optlenp);
long net_shim_fcntl(long fd, long cmd, long arg);
long net_shim_ioctl(long fd, long req, long arg);

// poll()/select() support (see posix_shim.c): net_shim_pump() runs one
// net_poll() pass if any socket is open; net_shim_poll() then reports
// fd's current readiness as poll() revents for the requested events.
void net_shim_pump(void);
int net_shim_poll(long fd, int events);

#endif
