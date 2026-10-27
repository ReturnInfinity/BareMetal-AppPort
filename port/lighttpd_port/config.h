/* config.h -- hand-written lighttpd 1.4.85 build config for BareMetal-AppPort
 *
 * Plays the role meson's generated config.h normally does (the same
 * role curl_config.h/pyconfig.h play for curl/CPython). Every HAVE_*
 * below comes from src/meson.build's probe list, decided by what
 * posix_shim.c/net_shim.c/ext4_shim.c actually back, not just by what
 * musl declares: a function musl has but whose syscall lands in the
 * dispatcher's -ENOSYS default is left undefined so lighttpd takes its
 * own fallback path instead of failing at run time.
 */
#ifndef LIGHTTPD_BAREMETAL_CONFIG_H
#define LIGHTTPD_BAREMETAL_CONFIG_H

#define PACKAGE_NAME "lighttpd"
#define PACKAGE_VERSION "1.4.85"
#define LIGHTTPD_VERSION_ID 0x10485
#define LIBRARY_DIR "/lighttpd/lib" /* unused: LIGHTTPD_STATIC */

/* Every module is linked in and listed in plugin-static.h -- there is
 * no dlopen() path worth using for lighttpd's plugins here. */
#define LIGHTTPD_STATIC 1

#define SIZEOF_LONG 8
#define SIZEOF_OFF_T 8

/* ---- Headers ---- */
#define HAVE_SYS_POLL_H 1
#define HAVE_POLL_H 1
#define HAVE_SYS_UN_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_GETOPT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_PWD_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SYS_RANDOM_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_MALLOC_H 1
/* musl has these headers, but nothing behind them works on this port:
 * syslog() needs an AF_UNIX /dev/log socket, prctl()/inotify/
 * sendfile()/epoll aren't in posix_shim.c's dispatcher, and there's
 * no file-backed mmap() (see HAVE_MMAP below). */
/* #undef HAVE_SYSLOG_H */
/* #undef HAVE_SYS_PRCTL_H */
/* #undef HAVE_SYS_INOTIFY_H */
/* #undef HAVE_SYS_SENDFILE_H */
/* #undef HAVE_SYS_EPOLL_H */
/* #undef HAVE_SYS_MMAN_H */
/* #undef HAVE_DLFCN_H */
/* musl-only absences */
/* #undef HAVE_SYS_LOADAVG_H */
/* #undef HAVE_SYS_EVENT_H */
/* #undef HAVE_LINUX_RANDOM_H */

/* crypt_r() is pure computation in musl -- fine for mod_auth's
 * htpasswd-style crypt() hashes. */
#define HAVE_CRYPT_H 1
#define HAVE_CRYPT_R 1

#define HAVE_SOCKLEN_T 1

/* ---- Functions ---- */
/* The event loop: poll() is a real readiness wait (posix_shim.c), so
 * fdevent_impl.c's poll backend is what runs. No epoll/kqueue. */
#define HAVE_POLL 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_SIGACTION 1
#define HAVE_SIGNAL 1
#define HAVE_WRITEV 1
#define HAVE_LSTAT 1
#define HAVE_INET_ATON 1
#define HAVE_INET_PTON 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_TIMEGM 1
#define HAVE_STRERROR_R 1
#define HAVE_MEMPCPY 1
#define HAVE_EXPLICIT_BZERO 1
#define HAVE_JRAND48 1
#define HAVE_SRANDOM 1
#define HAVE_MKOSTEMP 1
#define HAVE_GETENTROPY 1 /* musl -> SYS_getrandom, which posix_shim.c backs */
/* No process model: no fork()/posix_spawn() (so no CGI, no spawned
 * FastCGI/SCGI backends, no daemonizing -- lighttpd's own !HAVE_FORK
 * path forces dont_daemonize), no uids, no rlimits, no chroot. */
/* #undef HAVE_FORK */
/* #undef HAVE_POSIX_SPAWN */
/* #undef HAVE_GETUID */
/* #undef HAVE_ISSETUGID */
/* #undef HAVE_GETRLIMIT */
/* #undef HAVE_CHROOT */
/* Syscalls not in posix_shim.c's dispatcher (pread64/pwrite64/
 * preadv/pipe2/madvise/sendfile/splice/copy_file_range): lighttpd
 * falls back to lseek()+read()/write() and plain buffers. */
/* #undef HAVE_PREAD */
/* #undef HAVE_PREADV */
/* #undef HAVE_PREADV2 */
/* #undef HAVE_PWRITE */
/* #undef HAVE_PWRITEV */
/* #undef HAVE_PIPE2 */
/* #undef HAVE_MADVISE */
/* #undef HAVE_SENDFILE */
/* #undef HAVE_SPLICE */
/* #undef HAVE_COPY_FILE_RANGE */
/* posix_shim.c's mmap() is anonymous memory only -- a file-backed
 * mapping would just be heap with nothing read into it. */
/* #undef HAVE_MMAP */
/* #undef HAVE_EPOLL_CTL */
/* not in musl */
/* #undef HAVE_ARC4RANDOM_BUF */
/* #undef HAVE_GETLOADAVG */
/* #undef HAVE_MALLOC_TRIM */
/* #undef HAVE_MALLOPT */
/* #undef HAVE_EXPLICIT_MEMSET */
/* #undef HAVE_MEMSET_S */
/* #undef HAVE_MEMSET_EXPLICIT */
/* #undef HAVE_GETRANDOM */ /* needs <linux/random.h>; getentropy() covers it */

/* IPv4 only (LWIP_IPV6=0 in lwipopts.h). */
/* #undef HAVE_IPV6 */
#define HAVE_WEAK_SYMBOLS 1
#define HAVE_STRUCT_TM_GMTOFF 1

/* No optional libraries (zlib/brotli/zstd/pcre2/lua/xml/ldap/TLS). */

#endif
