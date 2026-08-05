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
#include <sys/select.h>
#include <poll.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

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
// no real per-page unmapping available, so munmap()'d ranges can't be
// handed back to the OS -- instead they're kept on an address-sorted,
// coalescing free list (see below) and reused by later mmap() calls
// before the arena is bumped any further. brk()'s own sub-heap doesn't
// need this: musl already reuses freed small objects internally and
// only ever shrinks brk() from the top.
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

	// mmap() (the only caller of heap_alloc() -- see sys_mmap() below)
	// is contractually required to return page-aligned addresses.
	// musl's allocator (mallocng) depends on that: it's mmap()-only,
	// never brk()-based, and its "meta group" bookkeeping does
	// alignment-based pointer arithmetic on every mmap() return that
	// silently miscomputes if the base isn't actually page-aligned --
	// not a crash, just increasingly wrong internal accounting that
	// can eventually surface as a spurious malloc() failure ("out of
	// memory" from something like curltest.c's curl_easy_perform(),
	// which allocates far more, and far more repeatedly, than any
	// other app here ever has) long before the arena is really
	// exhausted. heap_cur only starts life at __bss_stop
	// (heap_init()), an arbitrary linker-computed address with no
	// alignment guarantee beyond c.ld's ALIGN(16) -- nowhere near
	// mmap()'s page-alignment contract -- so it's rounded up here
	// before ever being handed out. n itself is already a page
	// multiple (sys_mmap() rounds it before calling in), so once this
	// aligns the first call's start, heap_cur (= p + n) stays
	// page-aligned on every call after.
	char *p = (char *)(((u64)heap_cur + 4095) & ~(u64)4095);

	// Done as a u64 subtraction/comparison, not "p + n < p" pointer
	// arithmetic: overflowing a pointer is undefined behavior, so a
	// compiler is entitled to assume it never happens and fold that
	// comparison away - which silently defeats the overflow check.
	// The alignment bump above can itself push p past heap_end, which
	// would otherwise underflow this subtraction into a huge bogus
	// "remaining" instead of correctly reporting exhaustion.
	if ((u64)p > (u64)heap_end) {
		static const char msg[] = "posix_shim: out of memory\n";
		b_output(msg, sizeof(msg) - 1);
		return 0;
	}
	u64 remaining = (u64)heap_end - (u64)p;
	if (n > remaining) {
		static const char msg[] = "posix_shim: out of memory\n";
		b_output(msg, sizeof(msg) - 1);
		return 0;
	}

	heap_cur = p + n;
	return p;
}

// -----------------------------------------------------------------------
// mmap() free list
//
// Freed mmap() ranges can't be handed back to the OS (see above), so
// they're kept here instead and reused by later mmap() calls. Blocks
// are kept address-sorted so adjacent frees coalesce into one larger
// block -- otherwise an alloc/free churn cycle would fragment the
// arena into pieces too small to satisfy the next request even though
// the total free space is there. Node headers live inline in the freed
// memory itself (safe: nothing else references it once freed), so this
// costs no separate bookkeeping storage. brk() never touches this list;
// it only ever grows/shrinks the arena from the tail, same as before.
// -----------------------------------------------------------------------

struct free_block {
	struct free_block *next;
	size_t size;
};

static struct free_block *free_list = 0;

static void free_list_insert(void *addr, size_t size)
{
	struct free_block *blk = (struct free_block *)addr;
	blk->size = size;

	struct free_block *prev = 0;
	struct free_block *cur = free_list;
	while (cur && (char *)cur < (char *)addr) {
		prev = cur;
		cur = cur->next;
	}

	blk->next = cur;
	if (prev)
		prev->next = blk;
	else
		free_list = blk;

	// Coalesce with the following block first: merging into blk
	// here doesn't disturb the addresses free_list_insert() already
	// used to link blk in, whereas merging into prev below would.
	if (blk->next && (char *)blk + blk->size == (char *)blk->next) {
		blk->size += blk->next->size;
		blk->next = blk->next->next;
	}

	if (prev && (char *)prev + prev->size == (char *)blk) {
		prev->size += blk->size;
		prev->next = blk->next;
	}
}

// First-fit: good enough for the large (>=128KB), comparatively
// infrequent allocations mmap() actually serves.
static void *free_list_alloc(size_t n)
{
	struct free_block **pp = &free_list;

	while (*pp) {
		struct free_block *cur = *pp;
		if (cur->size >= n) {
			size_t remaining = cur->size - n;
			if (remaining >= sizeof(struct free_block)) {
				struct free_block *rest = (struct free_block *)((char *)cur + n);
				rest->size = remaining;
				rest->next = cur->next;
				*pp = rest;
			} else {
				*pp = cur->next;
			}
			return cur;
		}
		pp = &cur->next;
	}

	return 0;
}

static long sys_mmap(long addr, long len, long prot, long flags, long fd, long off)
{
	(void)addr; (void)prot; (void)fd; (void)off;

	if (!(flags & MAP_ANONYMOUS) || len <= 0)
		return -ENODEV;

	size_t n = ((size_t)len + 4095) & ~(size_t)4095;

	// Anonymous mmap() is contractually zero-filled, and musl relies
	// on that (e.g. calloc() skips its own memset for mmap-backed
	// allocations). heap_alloc()'s bump path gets that for free --
	// it only ever hands out untouched backing RAM -- but a
	// free-list block may still hold a prior allocation's data (plus
	// our own free_block header), so it has to be re-zeroed here.
	void *p = free_list_alloc(n);
	if (p)
		memset(p, 0, n);
	else
		p = heap_alloc(n);
	if (!p)
		return -ENOMEM;

	return (long)p;
}

static long sys_munmap(long addr, long len)
{
	if (!addr || len <= 0)
		return 0;

	size_t n = ((size_t)len + 4095) & ~(size_t)4095;
	free_list_insert((void *)addr, n);
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
// Time
//
// b_system(TIMECOUNTER, ...) (nanoseconds since boot -- see
// libBareMetal.h) is already this port's internal clock source for
// lwIP's sys_now() (net_glue.c), the heap/TLS RNG seed, etc. -- it was
// just never exposed to application code as a syscall (see
// OPENISSUES.md). libcurl needs *some* working clock_gettime() for its
// own timeout/pacing bookkeeping (Curl_now()), so CLOCK_MONOTONIC is
// wired up here to the same source. CLOCK_REALTIME is deliberately
// left unimplemented (-EINVAL below, same as any other unhandled
// clk_id) rather than answering with boot-relative time mislabeled as
// wall-clock time -- there's still no RTC/wall-clock source on this
// port, matching MBEDTLS_HAVE_TIME being left off in
// baremetal_mbedtls_config.h for the same reason.
// -----------------------------------------------------------------------

static long sys_clock_gettime(long clk_id, long ts_addr)
{
	struct timespec *ts = (struct timespec *)ts_addr;

	switch (clk_id) {
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_MONOTONIC_COARSE: {
		u64 ns = b_system(TIMECOUNTER, 0, 0);
		ts->tv_sec = (long)(ns / 1000000000ULL);
		ts->tv_nsec = (long)(ns % 1000000000ULL);
		return 0;
	}
	default:
		return -EINVAL;
	}
}

// -----------------------------------------------------------------------
// Networking (sockets) -- see net_shim.c/net_glue.c. IPv4 TCP/UDP only.
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
	return net_shim_sendto(fd, (const void *)buf, (size_t)len, flags, (const void *)addr, addrlen);
}

static long sys_recvfrom(long fd, long buf, long len, long flags, long addr, long addrlenp)
{
	return net_shim_recvfrom(fd, (void *)buf, (size_t)len, flags, (void *)addr, (socklen_t *)addrlenp);
}

// -----------------------------------------------------------------------
// select()/poll()
//
// Every blocking I/O call in this port (bmfs_read/write, net_shim_send/
// recv) already blocks internally for real (net_shim's up-to-30s
// timeout) -- there is no non-blocking mode for a caller to actually
// need readiness-multiplexing for (see OPENISSUES.md). So rather than
// leaving these as -ENOSYS (which would break libcurl's easy-interface
// transfer loop -- lib/select.c's Curl_socket_check() -- and anything
// else that unconditionally calls select()/poll() before a read/write
// as a matter of course), both are shimmed as an immediate "yes,
// whatever you asked about is ready" instead of a real wait: every
// fd this port recognizes (std fd 0-2, a BMFS fd, a socket fd) is
// reported ready for whatever of read/write the caller asked about,
// with the real blocking then happening for real inside the read()/
// write()/recv()/send() call that follows. This is honest about not
// being a real multiplexer (a caller juggling several fds to learn
// *which one* has data first won't get that -- it'll get "ready" for
// all of them and then block for real on whichever it tries), but is
// exactly the semantics this port's single-blocking-connection-at-a-
// time callers (libcurl's easy interface included) actually drive.
// -----------------------------------------------------------------------

static int fd_is_valid(long fd)
{
	return fd == 0 || fd == 1 || fd == 2 || bmfs_is_fd(fd) || net_shim_is_fd(fd);
}

static long sys_select(long nfds, long readfds_addr, long writefds_addr, long exceptfds_addr, long timeout_addr)
{
	fd_set *rfds = (fd_set *)readfds_addr;
	fd_set *wfds = (fd_set *)writefds_addr;
	fd_set *efds = (fd_set *)exceptfds_addr;
	(void)timeout_addr;

	if (nfds < 0 || nfds > FD_SETSIZE)
		return -EINVAL;

	long ready = 0;
	for (long fd = 0; fd < nfds; fd++) {
		int wanted = (rfds && FD_ISSET(fd, rfds)) || (wfds && FD_ISSET(fd, wfds));
		if (!wanted)
			continue;
		if (!fd_is_valid(fd)) {
			if (rfds)
				FD_CLR(fd, rfds);
			if (wfds)
				FD_CLR(fd, wfds);
			continue;
		}
		ready++;
	}
	if (efds)
		FD_ZERO(efds);

	return ready;
}

static long sys_poll(long fds_addr, long nfds, long timeout)
{
	(void)timeout;
	struct pollfd *fds = (struct pollfd *)fds_addr;
	long ready = 0;

	for (long i = 0; i < nfds; i++) {
		fds[i].revents = 0;

		if (fds[i].fd < 0)
			continue; // negative fd: POSIX says ignore this entry

		if (!fd_is_valid(fds[i].fd)) {
			fds[i].revents = POLLNVAL;
			ready++;
			continue;
		}

		fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
		if (fds[i].revents)
			ready++;
	}

	return ready;
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

// exit()/_exit() call this from wherever they were invoked, deep in
// musl's call stack -- and musl's _Exit() is _Noreturn, spinning
// forever on the syscall rather than returning up through main() if
// it ever came back. So instead of returning normally, unwind RSP
// straight back to _start's entry point (crt0.c) in one shot and
// perform its "pop rbp; ret" ourselves. That lands back in
// BareMetal's kernel right after it `call`ed into the app, exactly
// as if the app had returned normally -- which is what makes the
// kernel shut down.
extern void *__bmos_entry_sp;

static long sys_exit(long code)
{
	(void)code;
	__asm__ volatile (
		"movq __bmos_entry_sp(%%rip), %%rsp\n\t"
		"popq %%rbp\n\t"
		"ret\n\t"
		::: "memory"
	);
	__builtin_unreachable();
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
	case SYS_clock_gettime:                    return sys_clock_gettime(a1, a2);
	case SYS_brk:                             return sys_brk(a1);
	case SYS_mmap:                             return sys_mmap(a1, a2, a3, a4, a5, a6);
	case SYS_munmap:                            return sys_munmap(a1, a2);
	case SYS_arch_prctl:                         return sys_arch_prctl(a1, a2);
	case SYS_set_tid_address:                     return sys_set_tid_address(a1);
	case SYS_exit:                                  return sys_exit(a1);
	case SYS_exit_group:                             return sys_exit(a1);

	// Networking -- IPv4 TCP/UDP only, see net_shim.c.
	case SYS_socket:      return sys_socket(a1, a2, a3);
	case SYS_bind:         return sys_bind(a1, a2, a3);
	case SYS_listen:        return sys_listen(a1, a2);
	case SYS_accept:         return sys_accept4(a1, a2, a3, 0);
	case SYS_accept4:         return sys_accept4(a1, a2, a3, a4);
	case SYS_connect:          return sys_connect(a1, a2, a3);
	case SYS_sendto:            return sys_sendto(a1, a2, a3, a4, a5, a6);
	case SYS_recvfrom:           return sys_recvfrom(a1, a2, a3, a4, a5, a6);

	// x86-64 musl's select()/poll() library functions issue these two
	// syscalls directly (SYS_select/SYS_poll both exist on x86-64,
	// unlike some other archs where they're synthesized from
	// pselect6/ppoll) -- see the "select()/poll()" section above.
	case SYS_select:      return sys_select(a1, a2, a3, a4, a5);
	case SYS_poll:         return sys_poll(a1, a2, a3);

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
