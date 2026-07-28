// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// ext4_shim.c -- lwext4-backed POSIX file I/O, used by posix_shim.c to
// back open/read/write/close/lseek/fstat/unlink/mkdir/rmdir/getdents
// and a cwd concept. Replaces the old bmfs.c (BMFS, BareMetal's own
// flat-namespace format): the disk is now expected to hold a plain
// EXT2 filesystem, read/written through lwext4 (see
// port/lwext4_port/) instead of a hand-rolled directory parser. Real
// directories/paths are now supported (lwext4 handles that), and
// atime/mtime/ctime are set from b_system(TIMECOUNTER) -- boot-
// relative, not wall-clock, since this port has no RTC/clock_gettime
// source, but still useful to tools that only compare timestamps
// against each other.
// =============================================================================

#include <ext4.h>
#include <ext4_super.h>
#include <ext4_inode.h>

// ext4_misc.h (pulled in transitively above) leaves this macro
// defined for its own file's use, but it collides with a real field
// name -- both in our own linux_kstat below and, worse, in musl's own
// bits/stat.h (struct stat's padding field is itself called
// __unused) -- so anything included after it that spells that
// identifier gets silently mangled instead of compiled.
#undef __unused

#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "libBareMetal.h"
#include "ext4_shim.h"
#include "lwext4_port/blockdev_baremetal.h"

#define EXT4_SHIM_DEV_NAME  "ext2"
#define EXT4_SHIM_MOUNT_POINT "/"

#define EXT4_SHIM_FD_BASE  3
#define EXT4_SHIM_MAX_OPEN 32

// Long enough for any real path; longer ones are truncated rather
// than rejected outright (matches this port's general "best effort,
// no hard failures on cosmetic limits" posture elsewhere).
#define EXT4_SHIM_PATH_MAX 256

struct ext4_shim_file {
	int used;
	int is_dir;
	ext4_file f;
	ext4_dir d;
	char path[EXT4_SHIM_PATH_MAX]; // resolved absolute path -- reused for openat()'s dirfd, fchdir(), and timestamp updates by path
};

static struct ext4_shim_file files[EXT4_SHIM_MAX_OPEN];
static int mounted = 0;
static uint32_t block_size = 1024; // EXT2's smallest/default block size, until mount says otherwise
static char cwd[EXT4_SHIM_PATH_MAX] = "/";

// A fixed anchor added to boot uptime to stand in for a real
// wall-clock epoch (2026-01-01 UTC) -- without it, ext2's one-second-
// granularity timestamp fields would read back as 0 for the (common,
// on a unikernel that boots and runs a whole program in well under a
// second) case of TIMECOUNTER not yet having reached its first full
// second, which is indistinguishable from "never set" and defeats the
// point of setting them at all. Boot-relative, not real, but never 0
// and still monotonically increasing with uptime -- see file banner
// comment.
#define EXT4_SHIM_EPOCH_BASE 1767225600UL

static uint32_t ext4_shim_now(void)
{
	return (uint32_t)(EXT4_SHIM_EPOCH_BASE + b_system(TIMECOUNTER, 0, 0) / 1000000000ULL);
}

// Joins a cwd-or-directory-fd base (always absolute) with a relative
// path component.
static void ext4_shim_join(const char *base, const char *rel, char *out, size_t outsz)
{
	size_t blen = strnlen(base, outsz - 1);
	if (blen == 1 && base[0] == '/')
		blen = 0; // avoid "//" when base is the root
	memcpy(out, base, blen);
	out[blen] = '/';
	size_t rlen = strnlen(rel, outsz - blen - 2);
	memcpy(out + blen + 1, rel, rlen);
	out[blen + 1 + rlen] = '\0';
}

// Resolves a path argument the way openat()/fstatat()/etc. expect:
// absolute paths ignore dirfd entirely; relative ones are joined
// against AT_FDCWD's cwd or, if dirfd names one of this shim's own
// open directory fds, that directory's own path. Returns NULL (caller
// should report -EBADF) if dirfd is neither AT_FDCWD nor a currently-
// open directory fd.
static const char *ext4_shim_resolve(long dirfd, const char *in, char *out, size_t outsz)
{
	if (in[0] == '/') {
		size_t len = strnlen(in, outsz - 1);
		memcpy(out, in, len);
		out[len] = '\0';
		return out;
	}

	if (dirfd == AT_FDCWD) {
		ext4_shim_join(cwd, in, out, outsz);
		return out;
	}

	if (ext4_shim_is_fd(dirfd) && files[dirfd - EXT4_SHIM_FD_BASE].is_dir) {
		ext4_shim_join(files[dirfd - EXT4_SHIM_FD_BASE].path, in, out, outsz);
		return out;
	}

	return 0;
}

// Same as ext4_shim_resolve(), fixed to AT_FDCWD -- for the plain
// (non-*at) calls musl issues directly on this arch (unlink(),
// mkdir(), rmdir(), chdir() all have their own non-dirfd syscall
// numbers here, see arch/x86_64/bits/syscall.h.in).
static const char *ext4_shim_path(const char *in, char *out, size_t outsz)
{
	return ext4_shim_resolve(AT_FDCWD, in, out, outsz);
}

static void ext4_shim_mount(void)
{
	if (mounted)
		return;
	mounted = 1;

	ext4_device_register(baremetal_blockdev_get(), EXT4_SHIM_DEV_NAME);
	if (ext4_mount(EXT4_SHIM_DEV_NAME, EXT4_SHIM_MOUNT_POINT, false) != EOK)
		return;

	struct ext4_sblock *sb;
	if (ext4_get_sblock(EXT4_SHIM_MOUNT_POINT, &sb) == EOK)
		block_size = ext4_sb_get_block_size(sb);
}

void ext4_shim_sync(void)
{
	if (!mounted)
		return;

	for (int i = 0; i < EXT4_SHIM_MAX_OPEN; i++) {
		if (files[i].used) {
			if (files[i].is_dir)
				ext4_dir_close(&files[i].d);
			else
				ext4_fclose(&files[i].f);
			files[i].used = 0;
		}
	}

	ext4_umount(EXT4_SHIM_MOUNT_POINT);
}

int ext4_shim_is_fd(long fd)
{
	return fd >= EXT4_SHIM_FD_BASE && fd < EXT4_SHIM_FD_BASE + EXT4_SHIM_MAX_OPEN
		&& files[fd - EXT4_SHIM_FD_BASE].used;
}

long ext4_shim_open(long dirfd, const char *path, int flags, int mode)
{
	(void)mode; // no permission model, same as the old BMFS shim

	ext4_shim_mount();

	char pathbuf[EXT4_SHIM_PATH_MAX];
	const char *p = ext4_shim_resolve(dirfd, path, pathbuf, sizeof(pathbuf));
	if (!p)
		return -EBADF;

	int slot = -1;
	for (int i = 0; i < EXT4_SHIM_MAX_OPEN; i++) {
		if (!files[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return -EMFILE;

	struct ext4_shim_file *sf = &files[slot];

	// A directory can only be opened for listing (this is also how
	// opendir() reaches this shim: musl's opendir() itself just calls
	// open(path, O_RDONLY|O_DIRECTORY)). ext4_fopen2() always forces
	// EXT4_DE_REG_FILE and fails EISDIR on a real directory, so route
	// those through ext4_dir_open() instead.
	if (ext4_inode_exist(p, EXT4_DE_DIR) == EOK) {
		if ((flags & O_ACCMODE) != O_RDONLY)
			return -EISDIR;

		int r = ext4_dir_open(&sf->d, p);
		if (r != EOK)
			return -r;

		sf->is_dir = 1;
	} else {
		int existed = !(flags & O_CREAT) || ext4_inode_exist(p, EXT4_DE_REG_FILE) == EOK;

		int r = ext4_fopen2(&sf->f, p, flags);
		if (r != EOK)
			return -r;

		sf->is_dir = 0;

		uint32_t now = ext4_shim_now();
		if (!existed) {
			ext4_ctime_set(p, now);
			ext4_mtime_set(p, now);
		}
		ext4_atime_set(p, now);
	}

	size_t plen = strnlen(p, sizeof(sf->path) - 1);
	memcpy(sf->path, p, plen);
	sf->path[plen] = '\0';

	sf->used = 1;
	return EXT4_SHIM_FD_BASE + slot;
}

long ext4_shim_read(long fd, void *buf, size_t len)
{
	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];
	if (sf->is_dir)
		return -EISDIR;

	size_t rcnt = 0;
	int r = ext4_fread(&sf->f, buf, len, &rcnt);
	if (r != EOK)
		return -r;

	return (long)rcnt;
}

long ext4_shim_write(long fd, const void *buf, size_t len)
{
	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];
	if (sf->is_dir)
		return -EISDIR;

	size_t wcnt = 0;
	int r = ext4_fwrite(&sf->f, buf, len, &wcnt);
	if (r != EOK)
		return -r;

	if (wcnt > 0) {
		uint32_t now = ext4_shim_now();
		ext4_mtime_set(sf->path, now);
		ext4_ctime_set(sf->path, now);
	}

	return (long)wcnt;
}

long ext4_shim_close(long fd)
{
	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];

	int r = sf->is_dir ? ext4_dir_close(&sf->d) : ext4_fclose(&sf->f);
	sf->used = 0;

	return r == EOK ? 0 : -r;
}

long ext4_shim_lseek(long fd, long offset, int whence)
{
	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];

	if (sf->is_dir) {
		// lwext4's directory iterator only supports rewind-to-0, not
		// arbitrary seek/tell positions -- this is the only lseek()
		// a directory fd should ever see, since musl's rewinddir()
		// is implemented as exactly lseek(fd, 0, SEEK_SET).
		if (offset == 0 && whence == SEEK_SET) {
			ext4_dir_entry_rewind(&sf->d);
			return 0;
		}
		return -EINVAL;
	}

	int r = ext4_fseek(&sf->f, offset, (uint32_t)whence);
	if (r != EOK)
		return -r;

	return (long)ext4_ftell(&sf->f);
}

long ext4_shim_fstat_fd(long fd, void *stbuf)
{
	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];
	struct stat *st = stbuf;

	memset(st, 0, sizeof(*st));

	if (sf->is_dir) {
		st->st_mode = S_IFDIR | 0755;
		st->st_blksize = (blksize_t)block_size;
	} else {
		uint64_t size = ext4_fsize(&sf->f);
		st->st_mode = S_IFREG | 0644; // ext4_fopen2() only ever succeeds on a regular file
		st->st_size = (off_t)size;
		st->st_blksize = (blksize_t)block_size;
		st->st_blocks = (blkcnt_t)((size + 511) / 512);
	}

	uint32_t atime = 0, mtime = 0, ctime = 0;
	ext4_atime_get(sf->path, &atime);
	ext4_mtime_get(sf->path, &mtime);
	ext4_ctime_get(sf->path, &ctime);
	st->st_atim.tv_sec = atime;
	st->st_mtim.tv_sec = mtime;
	st->st_ctim.tv_sec = ctime;

	return 0;
}

long ext4_shim_unlink(long dirfd, const char *path)
{
	ext4_shim_mount();

	char pathbuf[EXT4_SHIM_PATH_MAX];
	const char *p = ext4_shim_resolve(dirfd, path, pathbuf, sizeof(pathbuf));
	if (!p)
		return -EBADF;

	if (ext4_inode_exist(p, EXT4_DE_DIR) == EOK)
		return -EISDIR;

	int r = ext4_fremove(p);
	return r == EOK ? 0 : -r;
}

long ext4_shim_mkdir(long dirfd, const char *path)
{
	ext4_shim_mount();

	char pathbuf[EXT4_SHIM_PATH_MAX];
	const char *p = ext4_shim_resolve(dirfd, path, pathbuf, sizeof(pathbuf));
	if (!p)
		return -EBADF;

	int r = ext4_dir_mk(p);
	return r == EOK ? 0 : -r;
}

long ext4_shim_rmdir(long dirfd, const char *path)
{
	ext4_shim_mount();

	char pathbuf[EXT4_SHIM_PATH_MAX];
	const char *p = ext4_shim_resolve(dirfd, path, pathbuf, sizeof(pathbuf));
	if (!p)
		return -EBADF;

	int r = ext4_dir_rm(p);
	return r == EOK ? 0 : -r;
}

long ext4_shim_chdir(const char *path)
{
	ext4_shim_mount();

	char pathbuf[EXT4_SHIM_PATH_MAX];
	const char *p = ext4_shim_path(path, pathbuf, sizeof(pathbuf));

	if (ext4_inode_exist(p, EXT4_DE_DIR) != EOK)
		return -ENOTDIR;

	size_t len = strnlen(p, sizeof(cwd) - 1);
	memcpy(cwd, p, len);
	cwd[len] = '\0';
	return 0;
}

long ext4_shim_fchdir(long fd)
{
	if (!ext4_shim_is_fd(fd) || !files[fd - EXT4_SHIM_FD_BASE].is_dir)
		return -ENOTDIR;

	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];
	size_t len = strnlen(sf->path, sizeof(cwd) - 1);
	memcpy(cwd, sf->path, len);
	cwd[len] = '\0';
	return 0;
}

long ext4_shim_getcwd(char *buf, size_t size)
{
	size_t len = strlen(cwd);
	if (len + 1 > size)
		return -ERANGE;

	memcpy(buf, cwd, len + 1);
	return (long)(len + 1);
}

static unsigned char ext4_shim_dtype(uint8_t inode_type)
{
	switch (inode_type) {
	case EXT4_DE_REG_FILE: return DT_REG;
	case EXT4_DE_DIR:      return DT_DIR;
	case EXT4_DE_CHRDEV:   return DT_CHR;
	case EXT4_DE_BLKDEV:   return DT_BLK;
	case EXT4_DE_FIFO:     return DT_FIFO;
	case EXT4_DE_SOCK:     return DT_SOCK;
	case EXT4_DE_SYMLINK:  return DT_LNK;
	default:               return DT_UNKNOWN;
	}
}

// Fills buf with musl's own struct dirent layout (arch/generic/bits/
// dirent.h: ino_t, off_t, unsigned short reclen, unsigned char type,
// then the NUL-terminated name -- no interior padding, since a char
// array only needs 1-byte alignment). musl's readdir() on this arch
// actually issues SYS_getdents64 (see posix_shim.c), but casts the
// raw bytes straight to its own struct dirent* without any kernel-ABI
// translation, so this only has to satisfy musl's own layout, not the
// real Linux getdents()/getdents64() ABI (which differ from this and
// from each other).
long ext4_shim_getdents(long fd, void *buf, size_t len)
{
	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];
	if (!sf->is_dir)
		return -ENOTDIR;

	unsigned char *out = buf;
	size_t used = 0;

	for (;;) {
		uint64_t before = sf->d.next_off;
		const ext4_direntry *de = ext4_dir_entry_next(&sf->d);
		if (!de)
			break;

		size_t namelen = de->name_length;
		size_t reclen = (19 + namelen + 1 + 7) & ~(size_t)7; // header (8+8+2+1) + name + NUL, 8-aligned

		if (used + reclen > len) {
			if (used == 0)
				return -EINVAL; // caller's buffer can't hold even one entry
			// lwext4 has no "push back one entry" -- next_off is a
			// plain byte offset ext4_dir_iterator_init() re-seeks
			// from, so rewinding it to its pre-call value here makes
			// the next getdents() call re-read this same entry
			// instead of dropping it.
			sf->d.next_off = before;
			break;
		}

		unsigned long ino = de->inode;
		long off = 0; // no meaningful seek cookie -- see ext4_shim.h's comment on telldir()/seekdir() not being implemented
		unsigned short rl = (unsigned short)reclen;
		unsigned char type = ext4_shim_dtype(de->inode_type);

		memcpy(out + used, &ino, sizeof(ino));
		memcpy(out + used + 8, &off, sizeof(off));
		memcpy(out + used + 16, &rl, sizeof(rl));
		out[used + 18] = type;
		memcpy(out + used + 19, de->name, namelen);
		memset(out + used + 19 + namelen, 0, reclen - 19 - namelen);

		used += reclen;
	}

	return (long)used;
}

long ext4_shim_symlink(const char *target, long dirfd, const char *path)
{
	ext4_shim_mount();

	char pathbuf[EXT4_SHIM_PATH_MAX];
	const char *p = ext4_shim_resolve(dirfd, path, pathbuf, sizeof(pathbuf));
	if (!p)
		return -EBADF;

	int r = ext4_fsymlink(target, p);
	return r == EOK ? 0 : -r;
}

long ext4_shim_readlink(long dirfd, const char *path, char *buf, size_t bufsize)
{
	ext4_shim_mount();

	char pathbuf[EXT4_SHIM_PATH_MAX];
	const char *p = ext4_shim_resolve(dirfd, path, pathbuf, sizeof(pathbuf));
	if (!p)
		return -EBADF;

	size_t rcnt = 0;
	int r = ext4_readlink(p, buf, bufsize, &rcnt);
	if (r != EOK)
		return -r;

	return (long)rcnt;
}

// x86_64 musl has no SYS_stat in this build; stat()/lstat()/
// fstatat() all funnel through fstatat(), which musl's own
// src/internal/syscall.h aliases to SYS_newfstatat (there is no
// SYS_fstatat on this arch either, just the alias). That path fills a
// "struct kstat" (arch/x86_64/kstat.h), which is what this mirrors
// field-for-field -- there's no real kernel on the other end to be
// ABI-compatible with, just musl's own parsing of these bytes.
struct linux_kstat {
	unsigned long st_dev;
	unsigned long st_ino;
	unsigned long st_nlink;

	unsigned int st_mode;
	unsigned int st_uid;
	unsigned int st_gid;
	unsigned int __pad0;
	unsigned long st_rdev;
	long st_size;
	long st_blksize;
	long st_blocks;

	long st_atime_sec;
	long st_atime_nsec;
	long st_mtime_sec;
	long st_mtime_nsec;
	long st_ctime_sec;
	long st_ctime_nsec;
	long __unused[3];
};

// Resolves a possible chain of symlinks at p in place, up to a bounded
// depth. lwext4 supports real symlinks (ext4_fsymlink()/
// ext4_readlink()); this is what lets stat() (unlike lstat()) actually
// follow one to its target instead of reporting the link itself.
static int ext4_shim_follow(char *pathbuf, size_t outsz)
{
	for (int depth = 0; depth < 8; depth++) {
		if (ext4_inode_exist(pathbuf, EXT4_DE_SYMLINK) != EOK)
			return EOK; // not a symlink (or doesn't exist) -- let the caller's own lookup report the real outcome

		char target[EXT4_SHIM_PATH_MAX];
		size_t rcnt = 0;
		int r = ext4_readlink(pathbuf, target, sizeof(target) - 1, &rcnt);
		if (r != EOK)
			return r;
		target[rcnt] = '\0';

		if (target[0] == '/') {
			memcpy(pathbuf, target, rcnt + 1);
		} else {
			char *slash = strrchr(pathbuf, '/');
			size_t dirlen = slash ? (size_t)(slash - pathbuf + 1) : 0;
			if (dirlen + rcnt >= outsz)
				return ENAMETOOLONG;
			memmove(pathbuf + dirlen, target, rcnt + 1);
		}
	}
	return ELOOP;
}

// dirfd is resolved the same way ext4_shim_open() resolves it (cwd or
// one of this shim's own open directory fds). follow resolves a
// trailing symlink to its target (stat()'s behavior) rather than
// reporting the link itself (lstat()'s behavior) -- see
// ext4_shim_follow().
long ext4_shim_fstatat(long dirfd, const char *path, void *kstbuf, int follow)
{
	ext4_shim_mount();

	char pathbuf[EXT4_SHIM_PATH_MAX];
	const char *p = ext4_shim_resolve(dirfd, path, pathbuf, sizeof(pathbuf));
	if (!p)
		return -EBADF;

	if (follow) {
		int r = ext4_shim_follow(pathbuf, sizeof(pathbuf));
		if (r != EOK)
			return -r;
	}

	struct ext4_sblock *sb;
	int r = ext4_get_sblock(EXT4_SHIM_MOUNT_POINT, &sb);
	if (r != EOK)
		return -EIO; // mount itself failed -- see ext4_shim_mount()

	uint32_t ino;
	struct ext4_inode inode;
	r = ext4_raw_inode_fill(pathbuf, &ino, &inode);
	if (r != EOK)
		return -r;

	struct linux_kstat *st = kstbuf;
	memset(st, 0, sizeof(*st));
	st->st_ino = ino;
	st->st_nlink = ext4_inode_get_links_cnt(&inode);
	st->st_mode = ext4_inode_get_mode(sb, &inode);
	st->st_size = (long)ext4_inode_get_size(sb, &inode);
	st->st_blksize = (long)block_size;
	st->st_blocks = (long)ext4_inode_get_blocks_count(sb, &inode);

	uint32_t atime = 0, mtime = 0, ctime = 0;
	ext4_atime_get(pathbuf, &atime);
	ext4_mtime_get(pathbuf, &mtime);
	ext4_ctime_get(pathbuf, &ctime);
	st->st_atime_sec = atime;
	st->st_mtime_sec = mtime;
	st->st_ctime_sec = ctime;

	return 0;
}

// =============================================================================
// EOF
