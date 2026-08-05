#ifndef _BMFS_H
#define _BMFS_H

#include <stddef.h>

// True if fd refers to an open BMFS-backed file (as opposed to a std
// fd 0-2, which posix_shim.c handles itself).
int bmfs_is_fd(long fd);

long bmfs_open(const char *path, int flags, int mode);
long bmfs_read(long fd, void *buf, size_t len);
long bmfs_write(long fd, const void *buf, size_t len);
long bmfs_close(long fd);
long bmfs_lseek(long fd, long offset, int whence);
long bmfs_fstat_fd(long fd, void *stbuf);
long bmfs_unlink(const char *path);

// Sets a file's logical size (SQLite's VFS layer -- see
// port/sqlite_port/ -- uses this to finalize/shrink journal files).
// Only ever asked to shrink in practice (growth happens through
// bmfs_write() instead), so unlike a real ftruncate() this doesn't
// zero-fill on grow; it's still accepted (bounded by the file's fixed
// reservation) rather than rejected outright, since nothing about
// that case needs to fail.
long bmfs_truncate(long fd, size_t length);

// Fills a Linux struct kstat (see bmfs.c) for path -- used to back
// the fstatat() syscall (aliased from SYS_newfstatat), which is what
// x86_64 musl's stat()/lstat()/fstatat() actually issue.
long bmfs_fstatat(const char *path, void *kstbuf);

#endif
