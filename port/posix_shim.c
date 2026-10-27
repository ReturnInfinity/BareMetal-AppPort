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
#include <sys/utsname.h>

#include "libBareMetal.h"
#include "posix_shim.h"
#include "ext4_shim.h"
#include "net_shim.h"
#include "net_glue.h"
#include "thread_shim.h"

// -----------------------------------------------------------------------
// Heap (brk / anonymous mmap)
//
// There is no demand paging here: the app's mapped window is a single
// contiguous region that starts out sized by the microVM's configured
// RAM (see FIRECRACKER.md), and whatever we hand out has to already be
// backed by real, mapped memory. This is a bump allocator starting
// right after .bss (see __bss_stop in c.ld) and capped at the top of
// that mapped window, queried via b_system(FREE_MEMORY, 0, 0) (app RAM
// in MiB, counted from __image_base -- see c.ld). It does not
// touch/zero memory itself, so the cap costs nothing until used.
//
// The window is not fixed for good, though: when the arena runs dry,
// heap_grow() asks the kernel for more with b_system(GROW_MEMORY, mib)
// and the kernel's virtio-mem driver hot-plugs RAM from Firecracker and
// appends it to the same window, so heap_end simply moves up. How much
// can be added is the host's call (Firecracker's /hotplug/memory
// requested_size, see baremetal.sh's MEMHOTPLUG_* settings); once that
// is exhausted, or on a VM with no virtio-mem device, GROW_MEMORY
// returns the unchanged total and the allocation fails the way it
// always did.
//
// mallocng routes any single allocation >=128KB through mmap()
// regardless of whether brk() is working, so mmap() draws from this
// same arena rather than failing those allocations outright. There is
// no real per-page unmapping available, so munmap()'d ranges can't be
// handed back to the OS -- instead they're kept on an address-sorted,
// coalescing free list (see below) and reused by later mmap() calls
// before the arena is bumped any further.
//
// brk() draws from this *same* arena too (sys_brk(), below) -- despite
// mallocng's large allocations being mmap()-only, mallocng still calls
// brk() itself, on its own schedule, to grow the small fixed-size pool
// it carves its internal "struct meta" bookkeeping objects from (one
// struct meta per allocation, including the mmap()'d ones -- see
// alloc_meta() in mallocng's malloc.c). It remembers the break it last
// set and later grows it by one page at a time from that remembered
// value, with no way to know that mmap() calls for large objects have
// meanwhile moved heap_cur far ahead of it. sys_brk() has to reject
// (rather than honor) any such now-stale, behind-heap_cur target --
// see its comment -- or that periodic one-page growth would rewind
// heap_cur backward and silently reuse address space still live under
// an earlier mmap() allocation.
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

// Try to push heap_end up by at least `need` bytes by hot-plugging more
// RAM through the kernel (see the section comment above). The kernel
// rounds the request up to whole virtio-mem blocks and clamps it to what
// the host currently allows, so the window may grow by more or less than
// asked -- callers re-check the space they need afterwards. Returns 1 if
// heap_end moved at all, 0 if nothing could be added.
static int heap_grow(u64 need)
{
	u64 want_mib = (need + 1024 * 1024 - 1) / (1024 * 1024);
	if (want_mib == 0)
		want_mib = 1;

	// heap_end is always __image_base + (app RAM in MiB), see heap_init()
	u64 have_mib = ((u64)heap_end - (u64)__image_base) / (1024 * 1024);
	u64 now_mib = b_system(GROW_MEMORY, want_mib, 0);
	if (now_mib <= have_mib)
		return 0;

	heap_end = __image_base + now_mib * 1024 * 1024;
	return 1;
}

static long sys_brk(long addr)
{
	heap_init();

	// A break past the current ceiling isn't necessarily a failure any
	// more: grow the window first, then apply the usual checks. musl's
	// mallocng falls back to mmap() if brk() can't deliver, and that
	// path grows the same way (heap_alloc()), so this mostly just keeps
	// its one-page-at-a-time meta-area growth cheap.
	if ((char *)addr > heap_end)
		heap_grow((u64)addr - (u64)heap_end);

	// heap_cur is a single bump pointer shared between brk() (this
	// function) and mmap()'s own bump path (heap_alloc(), below) --
	// see this section's file header. musl's mallocng calls brk()
	// on its own schedule, independent of and interleaved with its
	// mmap() calls for large (>=128KB) objects: it remembers the
	// break it last set (glibc/mallocng-internal state, not queried
	// from us again) and later grows it by exactly one page at a
	// time from that remembered value -- with no idea that mmap()
	// calls for large allocations in between have since moved
	// heap_cur far ahead of it. If that stale, smaller target were
	// honored here, it would silently rewind heap_cur backward,
	// re-handing-out address space already live under a still-in-use
	// mmap() allocation -- corrupting it. Real brk()/mmap() are
	// independent address ranges on Linux, so this collision can't
	// happen there; the fix on this shared-arena port is to simply
	// never let brk() move the shared pointer backward -- a request
	// at or behind the current heap_cur is treated as a no-op (and
	// still reports the true current break), just like it would if
	// mmap() hadn't touched the shared arena in between.
	char *req = (char *)addr;
	if (req >= heap_cur && req <= heap_end)
		heap_cur = req;

	return (long)heap_cur;
}

// Reports how far a failed allocation overshot the remaining arena, so
// an OOM in the field says how many more bytes were needed instead of
// just that it happened.
static void report_oom(u64 short_by)
{
	char buf[80];
	size_t len = 0;

	static const char pre[] = "posix_shim: out of memory (short by ";
	memcpy(buf, pre, sizeof(pre) - 1);
	len += sizeof(pre) - 1;

	char digits[20];
	int n = 0;
	u64 v = short_by;
	if (v == 0) {
		digits[n++] = '0';
	} else {
		while (v) {
			digits[n++] = (char)('0' + (v % 10));
			v /= 10;
		}
	}
	while (n > 0)
		buf[len++] = digits[--n];

	static const char post[] = " bytes)\n";
	memcpy(buf + len, post, sizeof(post) - 1);
	len += sizeof(post) - 1;

	b_output(buf, len);
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
	// Short of room? Ask the kernel for more before giving up. Each
	// heap_grow() that succeeds moves heap_end, so loop until the
	// request fits or the host has nothing more to give.
	for (;;) {
		u64 short_by;
		if ((u64)p > (u64)heap_end)
			short_by = ((u64)p - (u64)heap_end) + n;
		else if (n > (u64)heap_end - (u64)p)
			short_by = n - ((u64)heap_end - (u64)p);
		else
			break;
		if (!heap_grow(short_by)) {
			report_oom(short_by);
			return 0;
		}
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

// -----------------------------------------------------------------------
// /dev/null and std fd redirection
//
// open("/dev/null") gets an fd from its own small range (reads return
// EOF, writes are discarded), and dup2()/dup3() can point a std fd
// 0-2 at one of those -- or at another std fd -- which is all a
// program needs to do the usual "detach stdin/stdout from the
// terminal" dance at startup (lighttpd's server.c does exactly this
// and refuses to start without it). That's the whole of it: there's
// no general fd table to dup arbitrary fds within, so dup2() onto any
// other fd number, or of an EXT2/socket fd, is still -ENOSYS, and
// there's still no dup() at all (see SYS_fcntl's F_DUPFD comment in
// the dispatcher below).
// -----------------------------------------------------------------------

#define DEVNULL_FD_BASE 90 // between ext4_shim.c's 3.. and net_shim.c's 100..
#define DEVNULL_MAX     4

static int devnull_used[DEVNULL_MAX];

static int devnull_is_fd(long fd)
{
	return fd >= DEVNULL_FD_BASE && fd < DEVNULL_FD_BASE + DEVNULL_MAX && devnull_used[fd - DEVNULL_FD_BASE];
}

// What std fd 0/1/2 currently refers to.
enum { STD_CONSOLE_IN, STD_CONSOLE_OUT, STD_DEVNULL };
static int std_target[3] = { STD_CONSOLE_IN, STD_CONSOLE_OUT, STD_CONSOLE_OUT };

static int std_is_devnull(long fd)
{
	return fd >= 0 && fd <= 2 && std_target[fd] == STD_DEVNULL;
}

static long devnull_open(void)
{
	for (int i = 0; i < DEVNULL_MAX; i++) {
		if (!devnull_used[i]) {
			devnull_used[i] = 1;
			return DEVNULL_FD_BASE + i;
		}
	}
	return -EMFILE;
}

static long sys_dup3(long oldfd, long newfd, long flags)
{
	if (flags & ~O_CLOEXEC)
		return -EINVAL;
	if (newfd < 0 || newfd > 2)
		return -ENOSYS; // see above: only std fds can be redirected

	int target;
	if (devnull_is_fd(oldfd))
		target = STD_DEVNULL;
	else if (oldfd >= 0 && oldfd <= 2)
		target = std_target[oldfd];
	else if (oldfd < 0)
		return -EBADF;
	else
		return -ENOSYS;

	std_target[newfd] = target;
	return newfd;
}

static long sys_write(long fd, long buf, long len)
{
	if (devnull_is_fd(fd) || std_is_devnull(fd))
		return len;
	if (ext4_shim_is_fd(fd))
		return ext4_shim_write(fd, (const void *)buf, (size_t)len);
	if (net_shim_is_fd(fd))
		return net_shim_send(fd, (const void *)buf, (size_t)len, 0);
	if (fd != 1 && fd != 2)
		return -EBADF;
	if (len > 0)
		b_output((const char *)buf, (u64)len);
	return len;
}

// One byte of console input that poll()/select() already pulled out
// of b_input() to learn fd 0 was readable (b_input() has no peek), or
// -1. sys_read() hands it back first.
static int stdin_peek = -1;

static int stdin_readable(void)
{
	if (stdin_peek < 0) {
		u8 c = b_input();
		if (c)
			stdin_peek = c;
	}
	return stdin_peek >= 0;
}

static long sys_read(long fd, long buf, long len)
{
	if (devnull_is_fd(fd) || std_is_devnull(fd))
		return 0;
	if (ext4_shim_is_fd(fd))
		return ext4_shim_read(fd, (void *)buf, (size_t)len);
	if (net_shim_is_fd(fd))
		return net_shim_recv(fd, (void *)buf, (size_t)len, 0);
	if (fd != 0)
		return -EBADF;
	if (len <= 0)
		return 0;

	unsigned char *p = (unsigned char *)buf;
	u8 c;

	if (stdin_peek >= 0) {
		c = (u8)stdin_peek;
		stdin_peek = -1;
		goto have_first;
	}

	// Block for the first byte, then greedily drain whatever else
	// is immediately available without blocking. This spin stays
	// T_RUNNING the whole time (b_input() never yields), so it's
	// reached by thread_shim.c's ordinary running-thread signal
	// delivery on the very next timer tick -- no separate wake needed,
	// just poll for the resulting EINTR each time around.
	for (;;) {
		c = b_input();
		if (c)
			break;
		if (thread_shim_take_eintr())
			return -EINTR;
	}
have_first:
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
	if (ext4_shim_is_fd(fd))
		return ext4_shim_close(fd);
	if (net_shim_is_fd(fd))
		return net_shim_close(fd);
	if (devnull_is_fd(fd)) {
		devnull_used[fd - DEVNULL_FD_BASE] = 0;
		return 0;
	}
	if (fd == 0 || fd == 1 || fd == 2)
		return 0;
	return -EBADF;
}

// fd 0-2 are reported as a character device so musl's stdio treats
// them as a tty-like stream rather than a regular file. Real EXT2
// files are reported as a regular file or directory (see
// ext4_shim_fstat_fd()). Sockets are reported as S_IFSOCK with no
// further detail (nothing currently inspects socket fstat() results
// beyond the type bits).
static long sys_fstat(long fd, long stbuf)
{
	if (ext4_shim_is_fd(fd))
		return ext4_shim_fstat_fd(fd, (void *)stbuf);
	if (net_shim_is_fd(fd)) {
		struct stat *st = (struct stat *)stbuf;
		memset(st, 0, sizeof(*st));
		st->st_mode = S_IFSOCK | 0666;
		return 0;
	}
	if (fd != 0 && fd != 1 && fd != 2 && !devnull_is_fd(fd))
		return -EBADF;

	struct stat *st = (struct stat *)stbuf;
	memset(st, 0, sizeof(*st));
	st->st_mode = (devnull_is_fd(fd) || std_is_devnull(fd)) ? S_IFCHR | 0666 : S_IFCHR | 0620;

	return 0;
}

static long sys_lseek(long fd, long offset, long whence)
{
	if (ext4_shim_is_fd(fd))
		return ext4_shim_lseek(fd, offset, (int)whence);
	if (devnull_is_fd(fd) || std_is_devnull(fd))
		return 0; // as on Linux
	return -ESPIPE; // std fds 0-2 are streams, not seekable
}

static long sys_open(long dirfd, const char *path, long flags, long mode)
{
	if (path && strcmp(path, "/dev/null") == 0)
		return devnull_open();
	return ext4_shim_open(dirfd, path, (int)flags, (int)mode);
}

// Added for SQLite's VFS (port/sqlite_port/sqlite_vfs.c), the first
// port/*.c caller that needs it -- journal finalization shrinks a
// file's logical size without touching its data.
static long sys_ftruncate(long fd, long length)
{
	if (ext4_shim_is_fd(fd))
		return ext4_shim_truncate(fd, (size_t)length);
	return -EBADF;
}

static long sys_unlink(long dirfd, const char *path)
{
	return ext4_shim_unlink(dirfd, path);
}

static long sys_mkdir(long dirfd, const char *path)
{
	return ext4_shim_mkdir(dirfd, path);
}

static long sys_rmdir(long dirfd, const char *path)
{
	return ext4_shim_rmdir(dirfd, path);
}

// musl's rename() issues SYS_rename directly on this arch, renameat()
// SYS_renameat; renameat2() only with flags=0 (no RENAME_NOREPLACE/
// RENAME_EXCHANGE -- ext4_shim_rename() has no atomic primitive to
// build either on).
static long sys_renameat2(long olddirfd, const char *oldpath, long newdirfd, const char *newpath, long flags)
{
	if (flags)
		return -EINVAL;
	return ext4_shim_rename(olddirfd, oldpath, newdirfd, newpath);
}

// rmdir() has its own direct syscall number on this arch (see
// arch/x86_64/bits/syscall.h.in), but unlinkat(..., AT_REMOVEDIR) is
// how a program calling the *at() form removes a directory instead of
// a file -- musl's own rmdir() never issues this, only an explicit
// unlinkat() call would.
static long sys_unlinkat(long dirfd, const char *path, long flags)
{
	if (flags & AT_REMOVEDIR)
		return ext4_shim_rmdir(dirfd, path);
	return ext4_shim_unlink(dirfd, path);
}

static long sys_chdir(const char *path)
{
	return ext4_shim_chdir(path);
}

static long sys_fchdir(long fd)
{
	return ext4_shim_fchdir(fd);
}

static long sys_getcwd(char *buf, size_t size)
{
	return ext4_shim_getcwd(buf, size);
}

// uname() -- OPENISSUES.md's Process model section documents this as
// cut (no pid/uid/process-table model to report), and pyconfig_
// baremetal.h leaves HAVE_UNAME undefined so Python-level os.uname()
// stays off -- but musl's gethostname() (unistd.h) is implemented
// purely in terms of uname()'s nodename field (see musl's own
// src/unistd/gethostname.c), and Lib/socket.py's getfqdn() calls
// gethostname() for any of the common "bind to everything" addresses
// ("0.0.0.0", "::", or no host at all) -- exactly what
// http.server.HTTPServer.server_bind() does on every plain `HTTPServer(
// ("0.0.0.0", port), ...)`. So unlike getpid()/getuid()/etc (genuinely
// meaningless with no process/user model), a hostname is real,
// expected input to ordinary networking code -- answered here with a
// fixed, honest placeholder rather than -ENOSYS, the same "static but
// real" choice WALLCLOCK-backed clocks already make elsewhere in this
// file.
static long sys_uname(long buf_addr)
{
	struct utsname *u = (struct utsname *)buf_addr;
	if (!u)
		return -EFAULT;

	memset(u, 0, sizeof(*u));
	strcpy(u->sysname, "BareMetal");
	strcpy(u->nodename, "baremetal");
	strcpy(u->release, "1.0");
	strcpy(u->version, "BareMetal-Firecracker");
	strcpy(u->machine, "x86_64");

	return 0;
}

static long sys_getdents(long fd, void *buf, size_t len)
{
	if (!ext4_shim_is_fd(fd))
		return -EBADF;
	return ext4_shim_getdents(fd, buf, (size_t)len);
}

static long sys_symlink(const char *target, long dirfd, const char *path)
{
	return ext4_shim_symlink(target, dirfd, path);
}

static long sys_readlink(long dirfd, const char *path, char *buf, size_t bufsize)
{
	return ext4_shim_readlink(dirfd, path, buf, bufsize);
}

// x86_64 musl's stat()/lstat()/fstatat() all funnel through fstatat()
// (aliased to SYS_newfstatat -- see ext4_shim_fstatat()). follow
// resolves a trailing symlink to its target (stat()'s behavior)
// rather than reporting the link itself (lstat()'s behavior).
static long sys_fstatat(long dirfd, long path, long kstbuf, long flags)
{
	int follow = !(flags & AT_SYMLINK_NOFOLLOW);
	return ext4_shim_fstatat(dirfd, (const char *)path, (void *)kstbuf, follow);
}

// musl's __stdout_write only checks the return code of this ioctl
// (success => stay line-buffered, failure => switch to full
// buffering); the winsize contents are never read, so nothing needs
// to be filled in.
// Sockets have their own FIONBIO/FIONREAD (net_shim_ioctl()).
static long sys_ioctl(long fd, long req, long arg)
{
	if (net_shim_is_fd(fd))
		return net_shim_ioctl(fd, req, arg);
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
// wired up here to the same source.
//
// b_system(WALLCLOCK, ...) returns seconds since the Unix epoch (read
// from the RTC at boot); there is no sub-second component wired up,
// so tv_nsec is always 0. CLOCK_REALTIME is backed by that.
//
// CLOCK_PROCESS_CPUTIME_ID -- there's no real per-process CPU-time
// accounting to back this with (see OPENISSUES.md's "Process model"
// section: no process table, and thread_shim.c doesn't track
// scheduled-vs-blocked time per thread either), but returning -EINVAL
// here means musl's clock() (src/time/clock.c) always fails, which
// silently breaks anything timing itself this way (e.g. CPython's
// time.process_time() fallback -- see pyconfig_baremetal.h). Since
// this port is single-process, wall time since boot is at least a
// reasonable stand-in, so this is wired to the same TIMECOUNTER
// source as CLOCK_MONOTONIC -- same "static but real" approximation
// WALLCLOCK-backed clocks already make above, not true CPU-only
// accounting (it keeps advancing through sleeps/blocking calls too).
// -----------------------------------------------------------------------

static long sys_clock_gettime(long clk_id, long ts_addr)
{
	struct timespec *ts = (struct timespec *)ts_addr;

	switch (clk_id) {
	case CLOCK_REALTIME:
	case CLOCK_REALTIME_COARSE:
		ts->tv_sec = (long)b_system(WALLCLOCK, 0, 0);
		ts->tv_nsec = 0;
		return 0;
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_MONOTONIC_COARSE:
	case CLOCK_PROCESS_CPUTIME_ID: {
		u64 ns = b_system(TIMECOUNTER, 0, 0);
		ts->tv_sec = (long)(ns / 1000000000ULL);
		ts->tv_nsec = (long)(ns % 1000000000ULL);
		return 0;
	}
	default:
		return -EINVAL;
	}
}

// nanosleep()/clock_nanosleep() -- thread_shim_sleep_until() (see that
// function's comment for why this can't just chain raw b_system(SLEEP,
// ...) HLTs: that would stop every other thread from running for the
// entire sleep) in NET_POLL_INTERVAL_NS-sized chunks -- net_poll() is
// still called between chunks so lwIP's timers/retransmits keep getting
// serviced during a long sleep instead of stalling for its whole
// duration.
//
// A caught signal now legitimately interrupts a sleep early (see
// thread_shim_sleep_until()'s own comment on why that's correct
// regardless of SA_RESTART) -- reported via that function's -EINTR
// return. *rem is left zeroed only for the "slept the full duration"
// case; an EINTR return reports real remaining time.
#define NET_POLL_INTERVAL_NS 10000000ULL // 10ms

static long sleep_until_ns(u64 target_ns, long rem_addr)
{
	u64 now_ns;

	while ((now_ns = b_system(TIMECOUNTER, 0, 0)) < target_ns) {
		u64 remaining_ns = target_ns - now_ns;
		u64 chunk_ns = remaining_ns < NET_POLL_INTERVAL_NS ? remaining_ns : NET_POLL_INTERVAL_NS;
		long rc = thread_shim_sleep_until(now_ns + chunk_ns);
		net_poll();
		if (rc == -EINTR) {
			now_ns = b_system(TIMECOUNTER, 0, 0);
			if (rem_addr) {
				struct timespec *rem = (struct timespec *)rem_addr;
				u64 left_ns = now_ns < target_ns ? target_ns - now_ns : 0;
				rem->tv_sec = left_ns / 1000000000ULL;
				rem->tv_nsec = left_ns % 1000000000ULL;
			}
			return -EINTR;
		}
	}

	if (rem_addr) {
		struct timespec *rem = (struct timespec *)rem_addr;
		rem->tv_sec = 0;
		rem->tv_nsec = 0;
	}
	return 0;
}

static long sys_nanosleep(long req_addr, long rem_addr)
{
	const struct timespec *req = (const struct timespec *)req_addr;

	if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L)
		return -EINVAL;

	u64 dur_ns = (u64)req->tv_sec * 1000000000ULL + (u64)req->tv_nsec;
	u64 now_ns = b_system(TIMECOUNTER, 0, 0);
	return sleep_until_ns(now_ns + dur_ns, rem_addr);
}

static long sys_clock_nanosleep(long clk_id, long flags, long req_addr, long rem_addr)
{
	const struct timespec *req = (const struct timespec *)req_addr;

	switch (clk_id) {
	case CLOCK_REALTIME:
	case CLOCK_REALTIME_COARSE:
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_MONOTONIC_COARSE:
		break;
	default:
		return -EINVAL;
	}

	if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L)
		return -EINVAL;

	u64 req_ns = (u64)req->tv_sec * 1000000000ULL + (u64)req->tv_nsec;

	if (flags & TIMER_ABSTIME) {
		// Absolute deadline. CLOCK_MONOTONIC's timeline *is*
		// TIMECOUNTER (see sys_clock_gettime() above), so req_ns
		// is already a TIMECOUNTER target for that clock. There's
		// no wall-clock/TIMECOUNTER conversion wired up here yet,
		// so a CLOCK_REALTIME absolute deadline is treated the
		// same way for now -- good enough for the common case of
		// sleeping until "now + a bit", off otherwise.
		return sleep_until_ns(req_ns, rem_addr);
	}

	u64 now_ns = b_system(TIMECOUNTER, 0, 0);
	return sleep_until_ns(now_ns + req_ns, rem_addr);
}

// -----------------------------------------------------------------------
// getrandom() -- same RDRAND-with-RDTSC-fallback technique crt0.c's
// fill_random() already uses to seed musl's stack-protector canary and
// mallocng's hardening secret (see this port's OPENISSUES.md, "Missing
// common syscalls"), just generalized to an arbitrary-length buffer and
// exposed as a real syscall. Duplicated here rather than shared with
// crt0.c (which can't call into this file -- it runs before .bss is
// zeroed) or the other RDRAND call sites (sqlite_vfs.c,
// entropy_hardware_poll.c, randombytes_baremetal.c) -- same
// "duplicated rather than shared" choice sqlite_vfs.c's own copy
// already makes, see that file's header.
//
// GRND_RANDOM/GRND_NONBLOCK/GRND_INSECURE are accepted and ignored:
// there's no blocking entropy pool here to distinguish between --
// RDRAND itself never blocks, and the RDTSC fallback is unconditional
// on RDRAND being absent or exhausted, not a caller-selectable mode.
// -----------------------------------------------------------------------

static int has_rdrand(void)
{
	unsigned int eax, ebx, ecx, edx;
	__asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
	return (ecx >> 30) & 1;
}

static long sys_getrandom(long buf_addr, long buflen, long flags)
{
	unsigned char *buf = (unsigned char *)buf_addr;
	int rdrand_ok = has_rdrand();
	long n = 0;

	(void)flags; // GRND_RANDOM/GRND_NONBLOCK/GRND_INSECURE -- see header above

	if (buflen < 0)
		return -EINVAL;

	while (n < buflen) {
		unsigned long v = 0;
		int ok = 0;

		if (rdrand_ok) {
			for (int tries = 0; tries < 10 && !ok; tries++)
				__asm__ volatile ("rdrand %0\n\tsetc %b1" : "=r"(v), "=q"(ok) :: "cc");
		}

		if (!ok) {
			unsigned int lo, hi;
			__asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
			v = ((unsigned long)hi << 32 | lo) ^ (unsigned long)buf ^ (unsigned long)n;
		}

		long chunk = buflen - n < (long)sizeof(v) ? buflen - n : (long)sizeof(v);
		memcpy(buf + n, &v, chunk);
		n += chunk;
	}

	return n;
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

// SOCK_NONBLOCK is honored on the new socket; SOCK_CLOEXEC is
// meaningless here (no exec) and ignored.
static long sys_accept4(long fd, long addr, long addrlenp, long flags)
{
	return net_shim_accept(fd, (void *)addr, (socklen_t *)addrlenp, flags);
}

static long sys_connect(long fd, long addr, long addrlen)
{
	return net_shim_connect(fd, (const void *)addr, addrlen);
}

static long sys_getsockname(long fd, long addr, long addrlenp)
{
	return net_shim_getsockname(fd, (void *)addr, (socklen_t *)addrlenp);
}

static long sys_getpeername(long fd, long addr, long addrlenp)
{
	return net_shim_getpeername(fd, (void *)addr, (socklen_t *)addrlenp);
}

static long sys_sendto(long fd, long buf, long len, long flags, long addr, long addrlen)
{
	return net_shim_sendto(fd, (const void *)buf, (size_t)len, flags, (const void *)addr, addrlen);
}

static long sys_recvfrom(long fd, long buf, long len, long flags, long addr, long addrlenp)
{
	return net_shim_recvfrom(fd, (void *)buf, (size_t)len, flags, (void *)addr, (socklen_t *)addrlenp);
}

// UIO_MAXIOV, Linux's cap on msg_iovlen -- more is -EMSGSIZE there too.
#define SHIM_MAX_IOV 1024

static long sys_sendmsg(long fd, long msg, long flags)
{
	if (!net_shim_is_fd(fd))
		return -ENOTSOCK;
	if (((const struct msghdr *)msg)->msg_iovlen > SHIM_MAX_IOV)
		return -EMSGSIZE;
	return net_shim_sendmsg(fd, (const struct msghdr *)msg, flags);
}

static long sys_recvmsg(long fd, long msg, long flags)
{
	if (!net_shim_is_fd(fd))
		return -ENOTSOCK;
	if (((const struct msghdr *)msg)->msg_iovlen > SHIM_MAX_IOV)
		return -EMSGSIZE;
	return net_shim_recvmsg(fd, (struct msghdr *)msg, flags);
}

// SO_RCVTIMEO/SO_SNDTIMEO are wired through to net_shim.c (they bound
// how long its blocking accept/connect/send/recv loops run -- see its
// file header); every other option, and every non-socket fd, is still
// an accept-and-ignore stub -- there's nothing else on this port
// (SO_REUSEADDR, TCP_NODELAY, ...) that actually has a knob behind it.
static long sys_setsockopt(long fd, long level, long optname, long optval, long optlen)
{
	if (net_shim_is_fd(fd))
		return net_shim_setsockopt(fd, level, optname, (const void *)optval, optlen);
	return 0;
}

static long sys_getsockopt(long fd, long level, long optname, long optval, long optlenp)
{
	if (net_shim_is_fd(fd))
		return net_shim_getsockopt(fd, level, optname, (void *)optval, (socklen_t *)optlenp);
	return 0;
}

// -----------------------------------------------------------------------
// select()/poll()
//
// A real readiness wait. Each pass runs one net_poll() (via
// net_shim_pump(), so lwIP's callbacks update every socket's state)
// and then asks each fd whether the call it's being polled for would
// complete without waiting:
//
//   - sockets: net_shim_poll() -- data/EOF/a pending connection for
//     POLLIN, send-buffer room or a finished connect() for POLLOUT,
//     plus POLLERR/POLLHUP on failure (see its comment);
//   - EXT2 files: always readable and writable, as regular files are on
//     Linux (ext4_shim_read/write never wait on anything external);
//   - fd 0: readable once a console byte is actually waiting (pulled
//     out of b_input() into stdin_peek -- see sys_read());
//   - fd 1/2: always writable.
//
// If nothing is ready and the timeout hasn't run out, the caller sleeps
// POLL_WAIT_INTERVAL_NS via thread_shim_sleep_until() -- yielding to
// other threads, same as net_shim.c's blocking loops -- and tries
// again. The interval is shorter than net_shim.c's 10ms because an
// event-loop server goes through poll() at least once per request, so
// the wake-up latency here is added to every one of them; 1ms is the
// kernel's tick, the shortest sleep that means anything. A caught
// signal ends the wait with -EINTR. The timeout isn't written back
// (Linux's select() does that; POSIX leaves it unspecified).
// -----------------------------------------------------------------------

#define POLL_WAIT_INTERVAL_NS 1000000ULL // 1ms

#define POLL_READ_EVENTS  (POLLIN | POLLRDNORM)
#define POLL_WRITE_EVENTS (POLLOUT | POLLWRNORM)

static int fd_is_valid(long fd)
{
	return fd == 0 || fd == 1 || fd == 2 || ext4_shim_is_fd(fd) || net_shim_is_fd(fd) || devnull_is_fd(fd);
}

// fd's poll() revents for events right now; POLLNVAL if fd isn't open.
static int fd_revents(long fd, int events)
{
	if (net_shim_is_fd(fd))
		return net_shim_poll(fd, events);
	if (ext4_shim_is_fd(fd) || devnull_is_fd(fd) || std_is_devnull(fd))
		return events & (POLL_READ_EVENTS | POLL_WRITE_EVENTS);
	if (fd == 0)
		return (events & POLL_READ_EVENTS) && stdin_readable() ? events & POLL_READ_EVENTS : 0;
	if (fd == 1 || fd == 2)
		return events & POLL_WRITE_EVENTS;
	return POLLNVAL;
}

// One poll pass over a caller-specific set of fds -- returns the number
// ready (or a negative errno), filling in the caller's results.
typedef long (*poll_scan_fn)(void *ctx);

// timeout_ns < 0 waits indefinitely; 0 checks once without waiting.
static long poll_wait(poll_scan_fn scan, void *ctx, long long timeout_ns)
{
	u64 deadline = 0;
	if (timeout_ns > 0)
		deadline = b_system(TIMECOUNTER, 0, 0) + (u64)timeout_ns;

	for (;;) {
		net_shim_pump();
		long n = scan(ctx);
		if (n != 0 || timeout_ns == 0)
			return n;

		u64 now = b_system(TIMECOUNTER, 0, 0);
		u64 wake = now + POLL_WAIT_INTERVAL_NS;
		if (timeout_ns > 0) {
			if (now >= deadline)
				return 0;
			if (wake > deadline)
				wake = deadline;
		}
		if (thread_shim_sleep_until(wake) == -EINTR)
			return -EINTR;
	}
}

struct poll_ctx {
	struct pollfd *fds;
	long nfds;
};

static long poll_scan(void *arg)
{
	struct poll_ctx *c = arg;
	long ready = 0;

	for (long i = 0; i < c->nfds; i++) {
		struct pollfd *p = &c->fds[i];
		p->revents = 0;
		if (p->fd < 0)
			continue; // negative fd: POSIX says ignore this entry
		p->revents = (short)fd_revents(p->fd, p->events);
		if (p->revents)
			ready++;
	}
	return ready;
}

static long sys_poll(long fds_addr, long nfds, long timeout)
{
	if (nfds < 0)
		return -EINVAL;

	struct poll_ctx c = { (struct pollfd *)fds_addr, nfds };
	return poll_wait(poll_scan, &c, timeout < 0 ? -1 : timeout * 1000000LL);
}

// select()'s fd_sets are in/out, so each pass scans the caller's
// original sets (in) into scratch copies (out), and only the final,
// returning pass's results are copied back.
struct select_ctx {
	long nfds;
	fd_set *rfds, *wfds, *efds;
	fd_set rout, wout;
};

static long select_scan(void *arg)
{
	struct select_ctx *c = arg;
	long ready = 0;

	FD_ZERO(&c->rout);
	FD_ZERO(&c->wout);

	for (long fd = 0; fd < c->nfds; fd++) {
		int events = 0;
		if (c->rfds && FD_ISSET(fd, c->rfds))
			events |= POLLIN;
		if (c->wfds && FD_ISSET(fd, c->wfds))
			events |= POLLOUT;
		if (!events)
			continue;

		// Same mapping as Linux's select(): hangup/error count as
		// readable, error as writable.
		int r = fd_revents(fd, events);
		if ((events & POLLIN) && (r & (POLLIN | POLLHUP | POLLERR))) {
			FD_SET(fd, &c->rout);
			ready++;
		}
		if ((events & POLLOUT) && (r & (POLLOUT | POLLERR))) {
			FD_SET(fd, &c->wout);
			ready++;
		}
	}
	return ready;
}

static long sys_select(long nfds, long readfds_addr, long writefds_addr, long exceptfds_addr, long timeout_addr)
{
	if (nfds < 0 || nfds > FD_SETSIZE)
		return -EINVAL;

	struct select_ctx c;
	c.nfds = nfds;
	c.rfds = (fd_set *)readfds_addr;
	c.wfds = (fd_set *)writefds_addr;
	c.efds = (fd_set *)exceptfds_addr;

	for (long fd = 0; fd < nfds; fd++) {
		int wanted = (c.rfds && FD_ISSET(fd, c.rfds)) || (c.wfds && FD_ISSET(fd, c.wfds)) ||
			(c.efds && FD_ISSET(fd, c.efds));
		if (wanted && !fd_is_valid(fd))
			return -EBADF;
	}

	long long timeout_ns = -1;
	if (timeout_addr) {
		const struct timeval *tv = (const struct timeval *)timeout_addr;
		if (tv->tv_sec < 0 || tv->tv_usec < 0)
			return -EINVAL;
		timeout_ns = tv->tv_sec * 1000000000LL + tv->tv_usec * 1000LL;
	}

	long n = poll_wait(select_scan, &c, timeout_ns);
	if (n < 0)
		return n;

	// No out-of-band data on this port, so the exception set never
	// has anything in it.
	if (c.rfds)
		*c.rfds = c.rout;
	if (c.wfds)
		*c.wfds = c.wout;
	if (c.efds)
		FD_ZERO(c.efds);
	return n;
}

// -----------------------------------------------------------------------
// Process / thread bootstrap
//
// arch_prctl(ARCH_SET_FS) is normally reached during startup, but on
// this port it's handled directly by a wrfsbase in
// src/thread/x86_64/__set_thread_area.s (ring 3-legal, no trap needed
// -- see that file and cpu.asm's CR4.FSGSBASE enable) and never comes
// through here. This case only exists in case something calls the
// arch_prctl() library function explicitly.
// -----------------------------------------------------------------------

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

static long sys_arch_prctl(long code, long addr)
{
	switch (code) {
	case ARCH_SET_FS: {
		__asm__ volatile ("wrfsbase %0" :: "r"((unsigned long)addr));
		return 0;
	}
	case ARCH_GET_FS: {
		unsigned long base;
		__asm__ volatile ("rdfsbase %0" : "=r"(base));
		*(unsigned long *)addr = base;
		return 0;
	}
	default:
		return -EINVAL;
	}
}

static long sys_set_tid_address(long addr)
{
	(void)addr;
	return thread_shim_current_tid(); // 1 outside thread_shim.c's scheduler, a real tid inside it
}

// exit()/_exit() call this from wherever they were invoked, deep in
// musl's call stack -- and musl's _Exit() is _Noreturn, spinning
// forever on the syscall rather than returning up through main() if
// it ever came back. So instead of returning normally, trap into the
// kernel via b_exit(): the app runs in ring 3, so it can no longer
// just unwind its own stack and `ret` into the kernel's ring 0 code
// the way it could back when app and kernel shared one privilege
// level and one stack. b_exit() (libBareMetal.c) never returns --
// the kernel's syscall gate resets RSP to its own stack and jumps
// straight to kernel.asm's app_finished, which is what makes the
// kernel shut down.
static long sys_exit(long code)
{
	(void)code;

	// Flushes the EXT2 superblock's free block/inode counters, which
	// lwext4 only writes back on unmount -- see ext4_shim_sync(). Has
	// to happen here rather than relying on the app to unmount itself:
	// this is the one path every exit (explicit exit()/_exit() and a
	// normal return from main(), which musl's __libc_start_main routes
	// through the same _Exit() call) is guaranteed to go through.
	ext4_shim_sync();

	b_exit();
	__builtin_unreachable();
}

// thread_shim.c's signal delivery (see its "Signals" section) calls
// this for a signal whose disposition is "terminate" -- reached from
// deep inside a CALLBACK_TIMER-driven call chain (possibly several
// thread_shim.c frames down), but that's fine: sys_exit() traps into
// the kernel via b_exit() and never returns through them, the same way
// a bare exit() call from arbitrarily deep inside any app callback
// already unwinds cleanly today.
void thread_shim_terminate_process(long code)
{
	sys_exit(code);
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
	case SYS_dup2:                 return a1 == a2 && a2 >= 0 && a2 <= 2 ? a2 : sys_dup3(a1, a2, 0);
	case SYS_dup3:                 return a1 == a2 ? -EINVAL : sys_dup3(a1, a2, a3);
	case SYS_fstat:                 return sys_fstat(a1, a2);
	case SYS_lseek:                  return sys_lseek(a1, a2, a3);
	case SYS_ioctl:                    return sys_ioctl(a1, a2, a3);
	case SYS_open:                      return sys_open(AT_FDCWD, (const char *)a1, a2, a3);
	case SYS_openat:                      return sys_open(a1, (const char *)a2, a3, a4);
	case SYS_unlink:                        return sys_unlink(AT_FDCWD, (const char *)a1);
	case SYS_unlinkat:                        return sys_unlinkat(a1, (const char *)a2, a3);
	case SYS_ftruncate:                         return sys_ftruncate(a1, a2);
	case SYS_mkdir:                               return sys_mkdir(AT_FDCWD, (const char *)a1);
	case SYS_mkdirat:                               return sys_mkdir(a1, (const char *)a2);
	case SYS_rename:                        return sys_renameat2(AT_FDCWD, (const char *)a1, AT_FDCWD, (const char *)a2, 0);
	case SYS_renameat:                      return sys_renameat2(a1, (const char *)a2, a3, (const char *)a4, 0);
	case SYS_renameat2:                     return sys_renameat2(a1, (const char *)a2, a3, (const char *)a4, a5);
	case SYS_rmdir:                                   return sys_rmdir(AT_FDCWD, (const char *)a1);
	case SYS_chdir:                                     return sys_chdir((const char *)a1);
	case SYS_fchdir:                                     return sys_fchdir(a1);
	case SYS_getcwd:                                       return sys_getcwd((char *)a1, (size_t)a2);
	case SYS_uname:                                        return sys_uname(a1);
	// musl's readdir() on this arch actually issues SYS_getdents64,
	// not the legacy SYS_getdents its own dirent.h still declares --
	// both land here since ext4_shim_getdents() fills musl's own
	// struct dirent layout either way (see its comment).
	case SYS_getdents:
	case SYS_getdents64:
		return sys_getdents(a1, (void *)a2, a3);
	case SYS_symlink:                                        return sys_symlink((const char *)a1, AT_FDCWD, (const char *)a2);
	case SYS_readlink:                                         return sys_readlink(AT_FDCWD, (const char *)a1, (char *)a2, (size_t)a3);
	// musl's __fstatat() takes the SYS_stat/SYS_lstat fast path for
	// plain stat(path)/lstat(path) (fd==AT_FDCWD, flag in {0,
	// AT_SYMLINK_NOFOLLOW}) and only falls through to the general
	// SYS_fstatat (aliased from SYS_newfstatat) case otherwise.
	case SYS_stat:
		return ext4_shim_fstatat(AT_FDCWD, (const char *)a1, (void *)a2, 1);
	case SYS_lstat:
		return ext4_shim_fstatat(AT_FDCWD, (const char *)a1, (void *)a2, 0);
	case SYS_newfstatat:                     return sys_fstatat(a1, a2, a3, a4);
	case SYS_clock_gettime:                    return sys_clock_gettime(a1, a2);
	case SYS_nanosleep:                       return sys_nanosleep(a1, a2);
	case SYS_clock_nanosleep:                return sys_clock_nanosleep(a1, a2, a3, a4);
	case SYS_getrandom:                     return sys_getrandom(a1, a2, a3);
	case SYS_brk:                             return sys_brk(a1);
	case SYS_mmap:                             return sys_mmap(a1, a2, a3, a4, a5, a6);
	case SYS_munmap:                            return sys_munmap(a1, a2);
	case SYS_arch_prctl:                         return sys_arch_prctl(a1, a2);
	case SYS_set_tid_address:                     return sys_set_tid_address(a1);
	// A bare SYS_exit (as opposed to SYS_exit_group -- see sys_exit()'s
	// own comment on _Exit()) only ever reaches here from musl's
	// __pthread_exit()'s final retry loop (pthread_create.c), i.e.
	// only from a worker thread created via thread_shim_clone() -- so
	// it terminates just that thread, not the whole process, exactly
	// like a real Linux clone()d thread. Reached from the original
	// thread (which never issues a bare SYS_exit in practice -- see
	// thread_shim.c's file header), it falls through to the same full
	// teardown SYS_exit_group does, rather than hang.
	case SYS_exit:
		if (thread_shim_is_worker_thread())
			thread_shim_exit_current(); // noreturn
		return sys_exit(a1);
	case SYS_exit_group:                             return sys_exit(a1);

	// Cooperative threads -- see thread_shim.c.
	case SYS_clone:       return thread_shim_clone(a1, a2, a3, a4, a5, a6);
	case SYS_futex:        return thread_shim_futex(a1, a2, a3, a4, a5, a6);
	case SYS_sched_yield:   return thread_shim_sched_yield();

	// Networking -- IPv4 TCP/UDP only, see net_shim.c.
	case SYS_socket:      return sys_socket(a1, a2, a3);
	case SYS_bind:         return sys_bind(a1, a2, a3);
	case SYS_listen:        return sys_listen(a1, a2);
	case SYS_accept:         return sys_accept4(a1, a2, a3, 0);
	case SYS_accept4:         return sys_accept4(a1, a2, a3, a4);
	case SYS_connect:          return sys_connect(a1, a2, a3);
	case SYS_getsockname:       return sys_getsockname(a1, a2, a3);
	case SYS_getpeername:        return sys_getpeername(a1, a2, a3);
	case SYS_sendto:            return sys_sendto(a1, a2, a3, a4, a5, a6);
	case SYS_recvfrom:           return sys_recvfrom(a1, a2, a3, a4, a5, a6);
	case SYS_sendmsg:             return sys_sendmsg(a1, a2, a3);
	case SYS_recvmsg:              return sys_recvmsg(a1, a2, a3);

	// x86-64 musl's select()/poll() library functions issue these two
	// syscalls directly (SYS_select/SYS_poll both exist on x86-64,
	// unlike some other archs where they're synthesized from
	// pselect6/ppoll) -- see the "select()/poll()" section above.
	case SYS_select:      return sys_select(a1, a2, a3, a4, a5);
	case SYS_poll:         return sys_poll(a1, a2, a3);

	// SO_RCVTIMEO/SO_SNDTIMEO are honored (see net_shim.c); every
	// other option is still accept-and-ignore -- see sys_setsockopt()/
	// sys_getsockopt() above.
	case SYS_setsockopt: return sys_setsockopt(a1, a2, a3, a4, a5);
	case SYS_getsockopt: return sys_getsockopt(a1, a2, a3, a4, a5);

	// No real fd flags/locking to speak of; accept and ignore rather
	// than fail callers (e.g. open(..., O_CLOEXEC)'s F_SETFD) that
	// don't check the result anyway. F_DUPFD/F_DUPFD_CLOEXEC are the
	// one exception: their return value isn't a status code but the
	// new fd number itself, and this port has no real dup() (no
	// SYS_dup case, HAVE_DUP left undefined -- see
	// pyconfig_baremetal.h) -- blindly returning 0 here would silently
	// hand back fd 0 (stdin) as a "successful" duplicate. Found via
	// Python/fileutils.c's _Py_dup(), which every uncaught-exception
	// traceback's source-line display goes through
	// (Parser/tokenizer.c's _PyTokenizer_FindEncodingFilename): the
	// bogus fd 0 made the tokenizer's encoding sniff read real stdin
	// instead of the failing fcntl cleanly, hanging the VM forever on
	// sys_read's blocking fd-0 path. Fail this specific case instead,
	// same "let it fail cleanly rather than lie" choice HAVE_DUP
	// already makes for real dup().
	//
	// Sockets are the other exception: O_NONBLOCK is real there, so
	// F_GETFL/F_SETFL go to net_shim_fcntl().
	case SYS_fcntl:
		if (net_shim_is_fd(a1))
			return net_shim_fcntl(a1, a2, a3);
		if (a2 == F_DUPFD || a2 == F_DUPFD_CLOEXEC)
			return -EINVAL;
		return 0;

	// Signals -- see thread_shim.c's "Signals" section for the
	// delivery mechanism (the same CALLBACK_TIMER injection point
	// thread_shim.c already uses for preemption).
	case SYS_rt_sigaction:  return thread_shim_rt_sigaction(a1, a2, a3, a4);
	case SYS_rt_sigprocmask: return thread_shim_rt_sigprocmask(a1, a2, a3, a4);
	case SYS_kill:           return thread_shim_kill(a1, a2);
	case SYS_tkill:          return thread_shim_tkill(a1, a2);
	case SYS_tgkill:         return thread_shim_tkill(a2, a3); // tgid (a1) ignored -- one thread group

	// No alternate signal stack (SA_ONSTACK is accepted but not acted
	// on -- deliver_pending_signals() calls handlers on the victim
	// thread's own stack regardless) and no robust-mutex list (this
	// port's mutexes aren't PTHREAD_MUTEX_ROBUST, see thread_shim.c's
	// header); accept and ignore rather than fail programs that merely
	// set these up defensively at startup.
	case SYS_sigaltstack:
	case SYS_set_robust_list:
		return 0;

	default:
		return -ENOSYS;
	}
}

// =============================================================================
// EOF
