// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// bmfs.c -- minimal BMFS (BareMetal File System) reader/writer, used
// by posix_shim.c to back open/read/write/close/lseek/fstat/unlink.
// The kernel itself has no filesystem support (see libBareMetal.h's
// b_nvs_read/b_nvs_write -- raw 4096-byte sector I/O only), so this
// parses the on-disk BMFS layout directly.
//
// Format (reverse-engineered from disk.img and
// payload/BareMetal-Monitor/src/monitor.asm -- BMFS has no vendored
// spec in this repo):
//   sector 0:  superblock. Magic "BMFS" at byte offset 1024.
//   sector 1:  directory table. 64 entries x 64 bytes.
//   sector N:  file data, in 2MiB-block units (1 block = 512 sectors).
//
// Directory entry (64 bytes):
//   0x00  32B  filename, NUL-padded, no leading '/', no subdirectories
//   0x20   8B  starting block (2MiB units)
//   0x28   8B  reserved blocks (2MiB units)
//   0x30   8B  file size, bytes
//   0x38   8B  unused
//
// Limitations: flat namespace (no directories); a file's capacity is
// fixed at creation (BMFS_DEFAULT_BLOCKS, 2MiB) and never grown past
// that; directory changes are only flushed to disk on close(); no
// access()/chmod()/timestamps (there's no clock source wired up).
// =============================================================================

#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

#include "libBareMetal.h"
#include "bmfs.h"

#define BMFS_DRIVE             0
#define BMFS_SECTOR_BYTES      4096ULL
#define BMFS_BLOCK_SECTORS     512ULL                                  /* 2MiB / 4096 */
#define BMFS_BLOCK_BYTES       (BMFS_BLOCK_SECTORS * BMFS_SECTOR_BYTES) /* 2MiB */
#define BMFS_DIR_SECTOR        1ULL
#define BMFS_DIR_ENTRIES       64
#define BMFS_FIRST_DATA_BLOCK  1ULL /* block 0 holds the superblock + directory */
#define BMFS_DEFAULT_BLOCKS    1ULL /* reservation given to newly created files */

#define BMFS_FD_BASE  3
#define BMFS_MAX_OPEN 8

struct bmfs_dirent {
	char name[32];
	u64 start_block;
	u64 reserved_blocks;
	u64 size;
	u64 unused;
};

struct bmfs_file {
	int used;
	int dirty;
	int append;
	int dir_index;
	u64 start_block;
	u64 reserved_blocks;
	u64 size;
	u64 pos;
};

static struct bmfs_dirent dir[BMFS_DIR_ENTRIES];
static int dir_loaded = 0;
static struct bmfs_file files[BMFS_MAX_OPEN];
static unsigned char scratch[BMFS_SECTOR_BYTES];

static void dir_load(void)
{
	if (dir_loaded)
		return;
	b_nvs_read(dir, BMFS_DIR_SECTOR, 1, BMFS_DRIVE);
	dir_loaded = 1;
}

static void dir_flush(void)
{
	b_nvs_write(dir, BMFS_DIR_SECTOR, 1, BMFS_DRIVE);
}

static int dir_find(const char *name)
{
	for (int i = 0; i < BMFS_DIR_ENTRIES; i++)
		if (dir[i].name[0] && strncmp(dir[i].name, name, sizeof(dir[i].name)) == 0)
			return i;
	return -1;
}

static int dir_find_free_slot(void)
{
	for (int i = 0; i < BMFS_DIR_ENTRIES; i++)
		if (dir[i].name[0] == 0)
			return i;
	return -1;
}

// Simple bump allocator over block numbers: the highest (start +
// reserved) of any existing entry. Matches the layout already used by
// every file this disk image was built with (see FIRECRACKER.md /
// bmfs tool output -- files are laid out back to back).
static u64 dir_next_free_block(void)
{
	u64 next = BMFS_FIRST_DATA_BLOCK;
	for (int i = 0; i < BMFS_DIR_ENTRIES; i++) {
		if (!dir[i].name[0])
			continue;
		u64 end = dir[i].start_block + dir[i].reserved_blocks;
		if (end > next)
			next = end;
	}
	return next;
}

// path -> BMFS filename: strip one leading '/'. BMFS is flat, so
// anything with an embedded '/' or too long for the 31-char (+NUL)
// name field is rejected by the caller.
static const char *bmfs_name(const char *path)
{
	if (path[0] == '/')
		path++;
	return path;
}

int bmfs_is_fd(long fd)
{
	return fd >= BMFS_FD_BASE && fd < BMFS_FD_BASE + BMFS_MAX_OPEN
		&& files[fd - BMFS_FD_BASE].used;
}

long bmfs_open(const char *path, int flags, int mode)
{
	(void)mode; // no permission model

	const char *name = bmfs_name(path);
	size_t len = strnlen(name, sizeof(dir[0].name));
	if (len == 0 || len >= sizeof(dir[0].name) || strchr(name, '/'))
		return -ENOENT;

	dir_load();

	int slot = -1;
	for (int i = 0; i < BMFS_MAX_OPEN; i++) {
		if (!files[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0)
		return -EMFILE;

	int di = dir_find(name);

	if (di < 0) {
		if (!(flags & O_CREAT))
			return -ENOENT;

		di = dir_find_free_slot();
		if (di < 0)
			return -ENOSPC;

		struct bmfs_dirent *e = &dir[di];
		memset(e, 0, sizeof(*e));
		memcpy(e->name, name, len);
		e->start_block = dir_next_free_block();
		e->reserved_blocks = BMFS_DEFAULT_BLOCKS;
		e->size = 0;
		dir_flush();
	} else if ((flags & O_CREAT) && (flags & O_EXCL)) {
		return -EEXIST;
	}

	struct bmfs_dirent *e = &dir[di];

	if (flags & O_TRUNC) {
		e->size = 0;
		dir_flush();
	}

	struct bmfs_file *f = &files[slot];
	f->used = 1;
	f->dirty = 0;
	f->append = (flags & O_APPEND) != 0;
	f->dir_index = di;
	f->start_block = e->start_block;
	f->reserved_blocks = e->reserved_blocks;
	f->size = e->size;
	f->pos = f->append ? f->size : 0;

	return BMFS_FD_BASE + slot;
}

// Reads/writes are done one sector at a time through a scratch buffer
// so callers can use arbitrary byte offsets/lengths without the
// caller's buffer needing to be sector-aligned.

static long bmfs_pread(struct bmfs_file *f, void *buf, size_t len, u64 off)
{
	if (off >= f->size)
		return 0;
	if (off + len > f->size)
		len = f->size - off;

	u64 base = f->start_block * BMFS_BLOCK_BYTES + off;
	unsigned char *dst = buf;
	size_t remaining = len;

	while (remaining) {
		u64 sector = base / BMFS_SECTOR_BYTES;
		u64 sector_off = base % BMFS_SECTOR_BYTES;
		size_t chunk = BMFS_SECTOR_BYTES - sector_off;
		if (chunk > remaining)
			chunk = remaining;

		b_nvs_read(scratch, sector, 1, BMFS_DRIVE);
		memcpy(dst, scratch + sector_off, chunk);

		dst += chunk;
		base += chunk;
		remaining -= chunk;
	}

	return (long)len;
}

static long bmfs_pwrite(struct bmfs_file *f, const void *buf, size_t len, u64 off)
{
	u64 cap = f->reserved_blocks * BMFS_BLOCK_BYTES;
	if (off >= cap)
		return -ENOSPC;
	if (off + len > cap)
		len = cap - off;
	if (len == 0)
		return -ENOSPC;

	u64 base = f->start_block * BMFS_BLOCK_BYTES + off;
	const unsigned char *src = buf;
	size_t remaining = len;

	while (remaining) {
		u64 sector = base / BMFS_SECTOR_BYTES;
		u64 sector_off = base % BMFS_SECTOR_BYTES;
		size_t chunk = BMFS_SECTOR_BYTES - sector_off;
		if (chunk > remaining)
			chunk = remaining;

		if (chunk < BMFS_SECTOR_BYTES)
			b_nvs_read(scratch, sector, 1, BMFS_DRIVE);
		memcpy(scratch + sector_off, src, chunk);
		b_nvs_write(scratch, sector, 1, BMFS_DRIVE);

		src += chunk;
		base += chunk;
		remaining -= chunk;
	}

	if (off + len > f->size) {
		f->size = off + len;
		f->dirty = 1;
	}

	return (long)len;
}

long bmfs_read(long fd, void *buf, size_t len)
{
	struct bmfs_file *f = &files[fd - BMFS_FD_BASE];
	long n = bmfs_pread(f, buf, len, f->pos);
	if (n > 0)
		f->pos += (u64)n;
	return n;
}

long bmfs_write(long fd, const void *buf, size_t len)
{
	struct bmfs_file *f = &files[fd - BMFS_FD_BASE];

	// O_APPEND: every write goes to the current end of file,
	// regardless of the fd's seek position -- otherwise a write
	// following an lseek() would land (and have its capacity
	// checked) at the wrong offset, silently overwriting existing
	// data instead of extending the file or correctly hitting
	// -ENOSPC once truly full.
	if (f->append)
		f->pos = f->size;

	long n = bmfs_pwrite(f, buf, len, f->pos);
	if (n > 0)
		f->pos += (u64)n;
	return n;
}

long bmfs_close(long fd)
{
	struct bmfs_file *f = &files[fd - BMFS_FD_BASE];

	if (f->dirty) {
		dir[f->dir_index].size = f->size;
		dir_flush();
	}
	f->used = 0;

	return 0;
}

long bmfs_lseek(long fd, long offset, int whence)
{
	struct bmfs_file *f = &files[fd - BMFS_FD_BASE];
	long base;

	switch (whence) {
	case 0: base = 0; break;              /* SEEK_SET */
	case 1: base = (long)f->pos; break;   /* SEEK_CUR */
	case 2: base = (long)f->size; break;  /* SEEK_END */
	default: return -EINVAL;
	}

	long np = base + offset;
	if (np < 0)
		return -EINVAL;

	f->pos = (u64)np;
	return np;
}

long bmfs_fstat_fd(long fd, void *stbuf)
{
	struct bmfs_file *f = &files[fd - BMFS_FD_BASE];
	struct stat *st = stbuf;

	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFREG | 0644;
	st->st_size = (off_t)f->size;
	st->st_blksize = BMFS_SECTOR_BYTES;
	st->st_blocks = (f->reserved_blocks * BMFS_BLOCK_BYTES) / 512;

	return 0;
}

// x86_64 musl has no SYS_stat in this build; stat()/lstat()/
// fstatat() all funnel through fstatat(), which musl's own
// src/internal/syscall.h aliases to SYS_newfstatat (there is no
// SYS_fstatat on this arch either, just the alias -- confirmed by
// disassembling the actual compiled call site, not by reading the
// header comments, which are easy to misread here). That path fills
// a "struct kstat" (arch/x86_64/kstat.h), which is what this mirrors
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

// dirfd/flags are ignored -- BMFS is flat, so there's no meaningful
// "relative to this directory fd" to honor, and no distinct symlinks
// for AT_SYMLINK_NOFOLLOW to matter.
long bmfs_fstatat(const char *path, void *kstbuf)
{
	const char *name = bmfs_name(path);

	dir_load();
	int di = dir_find(name);
	if (di < 0)
		return -ENOENT;

	struct linux_kstat *st = kstbuf;
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFREG | 0644;
	st->st_nlink = 1;
	st->st_ino = (unsigned long)(di + 1); // no real inode numbers; directory slot + 1
	st->st_size = (long)dir[di].size;
	st->st_blksize = BMFS_SECTOR_BYTES;
	st->st_blocks = (long)((dir[di].reserved_blocks * BMFS_BLOCK_BYTES) / 512);
	// No clock source is wired up (see b_system(TIMECOUNTER,...) if
	// that changes), so atime/mtime/ctime are left zeroed.

	return 0;
}

long bmfs_unlink(const char *path)
{
	const char *name = bmfs_name(path);

	dir_load();
	int di = dir_find(name);
	if (di < 0)
		return -ENOENT;

	memset(&dir[di], 0, sizeof(dir[di]));
	dir_flush();

	return 0;
}

// =============================================================================
// EOF
