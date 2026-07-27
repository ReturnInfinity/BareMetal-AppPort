// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// ext4_shim.c -- lwext4-backed POSIX file I/O, used by posix_shim.c to
// back open/read/write/close/lseek/fstat/unlink. Replaces the old
// bmfs.c (BMFS, BareMetal's own flat-namespace format): the disk is
// now expected to hold a plain EXT2 filesystem, read/written through
// lwext4 (see port/lwext4_port/) instead of a hand-rolled directory
// parser. Real directories/paths are now supported (lwext4 handles
// that), so unlike bmfs.c there's no flat-namespace name-length limit
// -- but there's still no clock source wired up, so timestamps are
// left at whatever lwext4 defaults to on creation.
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
#include <string.h>
#include <errno.h>

#include "ext4_shim.h"
#include "lwext4_port/blockdev_baremetal.h"

#define EXT4_SHIM_DEV_NAME  "ext2"
#define EXT4_SHIM_MOUNT_POINT "/"

#define EXT4_SHIM_FD_BASE  3
#define EXT4_SHIM_MAX_OPEN 8

// Long enough for any real path; longer ones are truncated rather
// than rejected outright (matches this port's general "best effort,
// no hard failures on cosmetic limits" posture elsewhere).
#define EXT4_SHIM_PATH_MAX 256

struct ext4_shim_file {
	int used;
	ext4_file f;
};

static struct ext4_shim_file files[EXT4_SHIM_MAX_OPEN];
static int mounted = 0;
static uint32_t block_size = 1024; // EXT2's smallest/default block size, until mount says otherwise

// lwext4 requires paths to start under the mount point ("/" here); a
// relative path (no leading '/') is treated as relative to that same
// root, since there's no real cwd concept on this port (see
// posix_shim.c's fstatat -- dirfd is always ignored).
static const char *ext4_shim_path(const char *in, char *out, size_t outsz)
{
	if (in[0] == '/') {
		size_t len = strnlen(in, outsz - 1);
		memcpy(out, in, len);
		out[len] = '\0';
	} else {
		out[0] = '/';
		size_t len = strnlen(in, outsz - 2);
		memcpy(out + 1, in, len);
		out[1 + len] = '\0';
	}
	return out;
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

long ext4_shim_open(const char *path, int flags, int mode)
{
	(void)mode; // no permission model, same as the old BMFS shim

	ext4_shim_mount();

	int slot = -1;
	for (int i = 0; i < EXT4_SHIM_MAX_OPEN; i++) {
		if (!files[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return -EMFILE;

	char pathbuf[EXT4_SHIM_PATH_MAX];
	const char *p = ext4_shim_path(path, pathbuf, sizeof(pathbuf));

	int r = ext4_fopen2(&files[slot].f, p, flags);
	if (r != EOK)
		return -r;

	files[slot].used = 1;
	return EXT4_SHIM_FD_BASE + slot;
}

long ext4_shim_read(long fd, void *buf, size_t len)
{
	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];
	size_t rcnt = 0;

	int r = ext4_fread(&sf->f, buf, len, &rcnt);
	if (r != EOK)
		return -r;

	return (long)rcnt;
}

long ext4_shim_write(long fd, const void *buf, size_t len)
{
	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];
	size_t wcnt = 0;

	int r = ext4_fwrite(&sf->f, buf, len, &wcnt);
	if (r != EOK)
		return -r;

	return (long)wcnt;
}

long ext4_shim_close(long fd)
{
	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];

	int r = ext4_fclose(&sf->f);
	sf->used = 0;

	return r == EOK ? 0 : -r;
}

long ext4_shim_lseek(long fd, long offset, int whence)
{
	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];

	int r = ext4_fseek(&sf->f, offset, (uint32_t)whence);
	if (r != EOK)
		return -r;

	return (long)ext4_ftell(&sf->f);
}

long ext4_shim_fstat_fd(long fd, void *stbuf)
{
	struct ext4_shim_file *sf = &files[fd - EXT4_SHIM_FD_BASE];
	struct stat *st = stbuf;
	uint64_t size = ext4_fsize(&sf->f);

	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFREG | 0644; // ext4_fopen2() only ever succeeds on a regular file
	st->st_size = (off_t)size;
	st->st_blksize = (blksize_t)block_size;
	st->st_blocks = (blkcnt_t)((size + 511) / 512);

	return 0;
}

long ext4_shim_unlink(const char *path)
{
	ext4_shim_mount();

	char pathbuf[EXT4_SHIM_PATH_MAX];
	const char *p = ext4_shim_path(path, pathbuf, sizeof(pathbuf));

	int r = ext4_fremove(p);
	return r == EOK ? 0 : -r;
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

// dirfd/flags are ignored -- see ext4_shim_path()'s comment on why
// there's no meaningful "relative to this directory fd" to honor.
long ext4_shim_fstatat(const char *path, void *kstbuf)
{
	ext4_shim_mount();

	char pathbuf[EXT4_SHIM_PATH_MAX];
	const char *p = ext4_shim_path(path, pathbuf, sizeof(pathbuf));

	struct ext4_sblock *sb;
	int r = ext4_get_sblock(EXT4_SHIM_MOUNT_POINT, &sb);
	if (r != EOK)
		return -EIO; // mount itself failed -- see ext4_shim_mount()

	uint32_t ino;
	struct ext4_inode inode;
	r = ext4_raw_inode_fill(p, &ino, &inode);
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
	// No clock source is wired up (see b_system(TIMECOUNTER,...) if
	// that changes), so atime/mtime/ctime are left zeroed.

	return 0;
}

// =============================================================================
// EOF
