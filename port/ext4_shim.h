#ifndef _EXT4_SHIM_H
#define _EXT4_SHIM_H

#include <stddef.h>

// True if fd refers to an open EXT2-backed file or directory (as
// opposed to a std fd 0-2, which posix_shim.c handles itself).
int ext4_shim_is_fd(long fd);

// dirfd is either AT_FDCWD or one of this shim's own directory fds
// (see ext4_shim_getdents()) -- it's resolved against cwd or that
// directory's stored path before touching lwext4.
long ext4_shim_open(long dirfd, const char *path, int flags, int mode);
long ext4_shim_read(long fd, void *buf, size_t len);
long ext4_shim_write(long fd, const void *buf, size_t len);
long ext4_shim_close(long fd);
long ext4_shim_lseek(long fd, long offset, int whence);
long ext4_shim_fstat_fd(long fd, void *stbuf);
long ext4_shim_unlink(long dirfd, const char *path);

// Fills a Linux struct kstat (see ext4_shim.c) for path -- used to
// back the fstatat() syscall (aliased from SYS_newfstatat), which is
// what x86_64 musl's stat()/lstat()/fstatat() actually issue. follow
// resolves a trailing symlink to its target first (stat()'s
// behavior); when false, the raw inode at path itself is reported
// (lstat()'s behavior).
long ext4_shim_fstatat(long dirfd, const char *path, void *kstbuf, int follow);

// Creates a real EXT2 symlink at path pointing at target (not
// resolved or validated -- same "best effort" posture as the rest of
// this shim). See ext4_shim_fstatat()'s follow flag for the read side.
long ext4_shim_symlink(const char *target, long dirfd, const char *path);
long ext4_shim_readlink(long dirfd, const char *path, char *buf, size_t bufsize);

long ext4_shim_chdir(const char *path);
long ext4_shim_getcwd(char *buf, size_t size);
long ext4_shim_fchdir(long fd);

long ext4_shim_mkdir(long dirfd, const char *path);
long ext4_shim_rmdir(long dirfd, const char *path);

// Fills buf with as many Linux struct dirent records (see
// ext4_shim.c) as fit in len, used to back musl's readdir() (issued
// as SYS_getdents on this arch). Returns bytes written, or 0 at
// end-of-directory. Each entry's d_off is always 0 -- lwext4's
// directory iterator only supports rewind-to-0 (see
// ext4_shim_lseek()), so telldir()/seekdir() (which stash/replay this
// field) aren't meaningfully supported, only the common
// opendir()/readdir()/closedir() sequence.
long ext4_shim_getdents(long fd, void *buf, size_t len);

// Closes any still-open files and unmounts, flushing the superblock's
// free block/inode counters to disk (lwext4 keeps these accurate in
// memory but only writes them back on ext4_umount(), not on every
// individual ext4_fclose()/ext4_fremove()). Must be called before the
// app exits -- see posix_shim.c's sys_exit() -- or e2fsck will find
// (harmless, auto-fixable, but avoidable) stale free-space counts on
// next mount.
void ext4_shim_sync(void);

#endif
