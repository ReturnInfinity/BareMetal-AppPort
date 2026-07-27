#ifndef _EXT4_SHIM_H
#define _EXT4_SHIM_H

#include <stddef.h>

// True if fd refers to an open EXT2-backed file (as opposed to a std
// fd 0-2, which posix_shim.c handles itself).
int ext4_shim_is_fd(long fd);

long ext4_shim_open(const char *path, int flags, int mode);
long ext4_shim_read(long fd, void *buf, size_t len);
long ext4_shim_write(long fd, const void *buf, size_t len);
long ext4_shim_close(long fd);
long ext4_shim_lseek(long fd, long offset, int whence);
long ext4_shim_fstat_fd(long fd, void *stbuf);
long ext4_shim_unlink(const char *path);

// Fills a Linux struct kstat (see ext4_shim.c) for path -- used to
// back the fstatat() syscall (aliased from SYS_newfstatat), which is
// what x86_64 musl's stat()/lstat()/fstatat() actually issue.
long ext4_shim_fstatat(const char *path, void *kstbuf);

// Closes any still-open files and unmounts, flushing the superblock's
// free block/inode counters to disk (lwext4 keeps these accurate in
// memory but only writes them back on ext4_umount(), not on every
// individual ext4_fclose()/ext4_fremove()). Must be called before the
// app exits -- see posix_shim.c's sys_exit() -- or e2fsck will find
// (harmless, auto-fixable, but avoidable) stale free-space counts on
// next mount.
void ext4_shim_sync(void);

#endif
