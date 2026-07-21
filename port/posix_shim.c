// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// posix_shim.c -- translates the Linux syscalls musl issues into
// BareMetal kernel calls (libBareMetal.h). BareMetal apps run in
// ring 0, in a single flat address space, with no syscall trap and
// no process/fd/signal machinery -- so this is a thin, mostly-stub
// POSIX surface, not a full kernel syscall implementation. See
// musl-1.2.6/arch/x86_64/syscall_arch.h and
// musl-1.2.6/src/thread/x86_64/syscall_cp.s for the call site.
// =============================================================================

#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include "libBareMetal.h"
#include "posix_shim.h"
#include "bmfs.h"
#include "net_shim.h"

// -----------------------------------------------------------------------
// Heap (brk / anonymous mmap)
//
// There is no demand paging here: the app's mapped window is a single
// fixed-size region set by the microVM's configured RAM (see
// FIRECRACKER.md), and whatever we hand out has to already be backed
// by real, mapped memory. This is a bump allocator starting right
// after .bss (see __bss_stop in c.ld) and capped at the top of that
// mapped window, queried once via b_system(FREE_MEMORY, 0, 0) (app RAM
// in MiB, counted from __image_base -- see c.ld). It does not
// touch/zero memory itself, so the cap costs nothing until used.
//
// mallocng routes any single allocation >=128KB through mmap()
// regardless of whether brk() is working, so mmap() draws from this
// same arena rather than failing those allocations outright. There is
// no real per-page unmapping available, so munmap() is a no-op: the
// memory is simply never reclaimed.
// -----------------------------------------------------------------------

extern char __bss_stop[];
extern char __image_base[];

static char *heap_cur = 0;
static char *heap_end = 0;

static void heap_init(void)
{
	if (heap_cur)
		return;

	u64 app_ram_mib = b_system(FREE_MEMORY, 0, 0);
	heap_cur = __bss_stop;
	heap_end = __image_base + app_ram_mib * 1024 * 1024;
}

static long sys_brk(long addr)
{
	heap_init();

	char *req = (char *)addr;
	if (req >= (char *)__bss_stop && req <= heap_end)
		heap_cur = req;

	return (long)heap_cur;
}

// Bump-allocate n bytes from the tail of the same arena brk() grows.
// Returns 0 (never a valid heap address here) on exhaustion.
static void *heap_alloc(size_t n)
{
	heap_init();

	char *p = heap_cur;
	if (p + n > heap_end || p + n < p)
		return 0;

	heap_cur = p + n;
	return p;
}

static long sys_mmap(long addr, long len, long prot, long flags, long fd, long off)
{
	(void)addr; (void)prot; (void)fd; (void)off;

	if (!(flags & MAP_ANONYMOUS) || len <= 0)
		return -ENODEV;

	size_t n = ((size_t)len + 4095) & ~(size_t)4095;
	void *p = heap_alloc(n);
	if (!p)
		return -ENOMEM;

	return (long)p;
}

static long sys_munmap(long addr, long len)
{
	(void)addr; (void)len;
	return 0;
}

// -----------------------------------------------------------------------
// I/O
// -----------------------------------------------------------------------

static long sys_write(long fd, long buf, long len)
{
	if (bmfs_is_fd(fd))
		return bmfs_write(fd, (const void *)buf, (size_t)len);
	if (net_shim_is_fd(fd))
		return net_shim_send(fd, (const void *)buf, (size_t)len, 0);
	if (fd != 1 && fd != 2)
		return -EBADF;
	if (len > 0)
		b_output((const char *)buf, (u64)len);
	return len;
}

static long sys_read(long fd, long buf, long len)
{
	if (bmfs_is_fd(fd))
		return bmfs_read(fd, (void *)buf, (size_t)len);
	if (net_shim_is_fd(fd))
		return net_shim_recv(fd, (void *)buf, (size_t)len, 0);
	if (fd != 0)
		return -EBADF;
	if (len <= 0)
		return 0;

	unsigned char *p = (unsigned char *)buf;
	u8 c;

	// Block for the first byte, then greedily drain whatever else
	// is immediately available without blocking.
	do {
		c = b_input();
	} while (!c);
	p[0] = c;

	long n = 1;
	while (n < len && (c = b_input()) != 0)
		p[n++] = c;

	return n;
}

static long sys_writev(long fd, long iov_addr, long iovcnt)
{
	const struct iovec *iov = (const struct iovec *)iov_addr;
	long total = 0;

	for (long i = 0; i < iovcnt; i++) {
		long n = sys_write(fd, (long)iov[i].iov_base, (long)iov[i].iov_len);
		if (n < 0)
			return total ? total : n;
		total += n;
		if ((size_t)n < iov[i].iov_len)
			break;
	}

	return total;
}

static long sys_readv(long fd, long iov_addr, long iovcnt)
{
	const struct iovec *iov = (const struct iovec *)iov_addr;
	long total = 0;

	for (long i = 0; i < iovcnt; i++) {
		long n = sys_read(fd, (long)iov[i].iov_base, (long)iov[i].iov_len);
		if (n < 0)
			return total ? total : n;
		total += n;
		if ((size_t)n < iov[i].iov_len)
			break;
	}

	return total;
}

static long sys_close(long fd)
{
	if (bmfs_is_fd(fd))
		return bmfs_close(fd);
	if (net_shim_is_fd(fd))
		return net_shim_close(fd);
	if (fd == 0 || fd == 1 || fd == 2)
		return 0;
	return -EBADF;
}

// fd 0-2 are reported as a character device so musl's stdio treats
// them as a tty-like stream rather than a regular file. Real BMFS
// files are reported as a regular file (see bmfs_fstat_fd()). Sockets
// are reported as S_IFSOCK with no further detail (nothing currently
// inspects socket fstat() results beyond the type bits).
static long sys_fstat(long fd, long stbuf)
{
	if (bmfs_is_fd(fd))
		return bmfs_fstat_fd(fd, (void *)stbuf);
	if (net_shim_is_fd(fd)) {
		struct stat *st = (struct stat *)stbuf;
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFSOCK | 0666;
		return 0;
	}
	if (fd != 0 && fd != 1 && fd != 2)
		return -EBADF;

	struct stat *st = (struct stat *)stbuf;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR | 0620;

	return 0;
}

static long sys_lseek(long fd, long offset, long whence)
{
	if (bmfs_is_fd(fd))
		return bmfs_lseek(fd, offset, (int)whence);
	return -ESPIPE; // std fds 0-2 are streams, not seekable
}

static long sys_open(const char *path, long flags, long mode)
{
	return bmfs_open(path, (int)flags, (int)mode);
}

static long sys_unlink(const char *path)
{
	return bmfs_unlink(path);
}

// x86_64 musl's stat()/lstat()/fstatat() all funnel through fstatat()
// (aliased to SYS_newfstatat -- see bmfs_fstatat()). dirfd/flags are
// ignored -- BMFS is flat, so there's no meaningful "relative to this
// directory fd" to honor.
static long sys_fstatat(long dirfd, long path, long kstbuf, long flags)
{
	(void)dirfd; (void)flags;
	return bmfs_fstatat((const char *)path, (void *)kstbuf);
}

// musl's __stdout_write only checks the return code of this ioctl
// (success => stay line-buffered, failure => switch to full
// buffering); the winsize contents are never read, so nothing needs
// to be filled in.
static long sys_ioctl(long fd, long req, long arg)
{
	(void)arg;
	if ((fd == 0 || fd == 1 || fd == 2) && req == TIOCGWINSZ)
		return 0;
	return -ENOTTY;
}

// -----------------------------------------------------------------------
// Networking (sockets) -- see net_shim.c/net_glue.c. IPv4 TCP only.
// -----------------------------------------------------------------------

static long sys_socket(long domain, long type, long protocol)
{
	return net_shim_socket(domain, type, protocol);
}

static long sys_bind(long fd, long addr, long addrlen)
{
	return net_shim_bind(fd, (const void *)addr, addrlen);
}

static long sys_listen(long fd, long backlog)
{
	return net_shim_listen(fd, backlog);
}

static long sys_accept4(long fd, long addr, long addrlenp, long flags)
{
	(void)flags; // no non-blocking/cloexec accept modes in this port
	return net_shim_accept(fd, (void *)addr, (socklen_t *)addrlenp);
}

static long sys_connect(long fd, long addr, long addrlen)
{
	return net_shim_connect(fd, (const void *)addr, addrlen);
}

static long sys_sendto(long fd, long buf, long len, long flags, long addr, long addrlen)
{
	(void)addr; (void)addrlen; // connected TCP sockets only -- destination is implicit
	return net_shim_send(fd, (const void *)buf, (size_t)len, flags);
}

static long sys_recvfrom(long fd, long buf, long len, long flags, long addr, long addrlenp)
{
	(void)addr; (void)addrlenp; // connected TCP sockets only -- no peer address to report
	return net_shim_recv(fd, (void *)buf, (size_t)len, flags);
}

// -----------------------------------------------------------------------
// Process / thread bootstrap
//
// arch_prctl(ARCH_SET_FS) is normally reached during startup, but on
// this port it's handled directly by a wrmsr in
// src/thread/x86_64/__set_thread_area.s (ring 0, no trap needed) and
// never comes through here. This case only exists in case something
// calls the arch_prctl() library function explicitly.
// -----------------------------------------------------------------------

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

static long sys_arch_prctl(long code, long addr)
{
	switch (code) {
	case ARCH_SET_FS: {
		unsigned lo = (unsigned)addr, hi = (unsigned)((unsigned long)addr >> 32);
		__asm__ volatile ("wrmsr" :: "c"(0xC0000100), "a"(lo), "d"(hi));
		return 0;
	}
	case ARCH_GET_FS: {
		unsigned lo, hi;
		__asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100));
		*(unsigned long *)addr = ((unsigned long)hi << 32) | lo;
		return 0;
	}
	default:
		return -EINVAL;
	}
}

static long sys_set_tid_address(long addr)
{
	(void)addr;
	return 1; // fake tid; there is only ever one thread
}

static long sys_exit(long code)
{
	(void)code;
	b_system(SHUTDOWN, 0, 0);
	for (;;)
		__asm__ volatile ("hlt");
}

// -----------------------------------------------------------------------
// Dispatcher
// -----------------------------------------------------------------------

long __bmos_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	switch (n) {
	case SYS_read:            return sys_read(a1, a2, a3);
	case SYS_write:            return sys_write(a1, a2, a3);
	case SYS_readv:             return sys_readv(a1, a2, a3);
	case SYS_writev:              return sys_writev(a1, a2, a3);
	case SYS_close:                return sys_close(a1);
	case SYS_fstat:                 return sys_fstat(a1, a2);
	case SYS_lseek:                  return sys_lseek(a1, a2, a3);
	case SYS_ioctl:                    return sys_ioctl(a1, a2, a3);
	case SYS_open:                      return sys_open((const char *)a1, a2, a3);
	case SYS_openat:                      return sys_open((const char *)a2, a3, a4); // AT_FDCWD-only: BMFS is flat, a1 (dirfd) is ignored
	case SYS_unlink:                        return sys_unlink((const char *)a1);
	// musl's __fstatat() takes the SYS_stat/SYS_lstat fast path for
	// plain stat(path)/lstat(path) (fd==AT_FDCWD, flag in {0,
	// AT_SYMLINK_NOFOLLOW}) and only falls through to the general
	// SYS_fstatat (aliased from SYS_newfstatat) case otherwise. BMFS
	// has no symlinks, so lstat behaves identically to stat.
	case SYS_stat:
	case SYS_lstat:
		return bmfs_fstatat((const char *)a1, (void *)a2);
	case SYS_newfstatat:                     return sys_fstatat(a1, a2, a3, a4);
	case SYS_brk:                             return sys_brk(a1);
	case SYS_mmap:                             return sys_mmap(a1, a2, a3, a4, a5, a6);
	case SYS_munmap:                            return sys_munmap(a1, a2);
	case SYS_arch_prctl:                         return sys_arch_prctl(a1, a2);
	case SYS_set_tid_address:                     return sys_set_tid_address(a1);
	case SYS_exit:                                  return sys_exit(a1);
	case SYS_exit_group:                             return sys_exit(a1);

	// Networking -- IPv4 TCP only, see net_shim.c.
	case SYS_socket:      return sys_socket(a1, a2, a3);
	case SYS_bind:         return sys_bind(a1, a2, a3);
	case SYS_listen:        return sys_listen(a1, a2);
	case SYS_accept:         return sys_accept4(a1, a2, a3, 0);
	case SYS_accept4:         return sys_accept4(a1, a2, a3, a4);
	case SYS_connect:          return sys_connect(a1, a2, a3);
	case SYS_sendto:            return sys_sendto(a1, a2, a3, a4, a5, a6);
	case SYS_recvfrom:           return sys_recvfrom(a1, a2, a3, a4, a5, a6);
	// No options are actually honored (e.g. SO_REUSEADDR, SO_RCVTIMEO);
	// accept and ignore rather than fail callers that merely set them
	// defensively.
	case SYS_setsockopt:
	case SYS_getsockopt:
		return 0;

	// No real fd flags/locking to speak of; accept and ignore rather
	// than fail callers (e.g. open(..., O_CLOEXEC)'s F_SETFD) that
	// don't check the result anyway.
	case SYS_fcntl:
		return 0;

	// No signal delivery, no thread list, on this port -- accept
	// and ignore rather than fail programs that merely try to set
	// these up defensively at startup.
	case SYS_rt_sigaction:
	case SYS_rt_sigprocmask:
	case SYS_sigaltstack:
	case SYS_set_robust_list:
		return 0;

	default:
		return -ENOSYS;
	}
}

// =============================================================================
// EOF
