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

// Fills a Linux struct kstat (see bmfs.c) for path -- used to back
// the fstatat() syscall (aliased from SYS_newfstatat), which is what
// x86_64 musl's stat()/lstat()/fstatat() actually issue.
long bmfs_fstatat(const char *path, void *kstbuf);

#endif
