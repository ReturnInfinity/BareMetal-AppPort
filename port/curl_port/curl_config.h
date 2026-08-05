/* curl_config.h -- hand-written build config for curl 8.21.0 on this port
 * (see scripts/get-curl.sh). curl is vendored unmodified; this is the
 * -DHAVE_CONFIG_H header its own lib/curl_setup.h pulls in instead of a
 * ./configure-generated one (there's no way to run curl's configure
 * against this freestanding, no-syscall-trap target).
 *
 * Method: started from `./configure`'s own output against a real
 * (glibc) Linux x86-64 host with every protocol/backend/threading
 * option this port can't support turned off -- see the flag list in
 * scripts/get-curl.sh's header -- then hand-corrected every HAVE_ and
 * USE_ macro that configure detects via the host's libc but this
 * port's musl+posix_shim.c stack doesn't actually back (getaddrinfo(),
 * gethostbyname_r(), threads, eventfd/pipe/socketpair, getrlimit, xattr,
 * a real wall clock, ...). Each correction below says which port file
 * it's tied to. When in doubt, a capability was left *off*: curl
 * degrades gracefully to a fallback (or a disabled feature) when a
 * HAVE_ macro is missing, whereas claiming one that isn't real fails
 * at link time or, worse, silently misbehaves at runtime.
 *
 * Scope this buys: plain HTTP and HTTPS (mbedTLS backend, see
 * port/tls_shim.c's neighbor vtls/mbedtls.c integration below) only.
 * No FTP/FILE/TELNET/TFTP/RTSP/DICT/GOPHER/LDAP(S)/POP3/IMAP/SMTP/
 * MQTT/SMB/WebSockets/IPFS -- this port has no use for them and
 * dropping them keeps the curl_*.o build list short (see build-app.sh).
 */

#ifndef CURL_PORT_CURL_CONFIG_H
#define CURL_PORT_CURL_CONFIG_H

/* ---------------------------------------------------------------------
 * Protocols and features not supported (or not wired up) on this port
 * --------------------------------------------------------------------- */

#define CURL_DISABLE_FTP 1
#define CURL_DISABLE_FILE 1
#define CURL_DISABLE_TELNET 1
#define CURL_DISABLE_TFTP 1
#define CURL_DISABLE_RTSP 1
#define CURL_DISABLE_DICT 1
#define CURL_DISABLE_GOPHER 1
#define CURL_DISABLE_LDAP 1
#define CURL_DISABLE_LDAPS 1
#define CURL_DISABLE_POP3 1
#define CURL_DISABLE_IMAP 1
#define CURL_DISABLE_SMTP 1
#define CURL_DISABLE_MQTT 1
#define CURL_DISABLE_IPFS 1
#define CURL_DISABLE_WEBSOCKETS 1
#define CURL_DISABLE_LIBCURL_OPTION 1 /* --libcurl C-code generation -- only used by the src/ CLI tool, which isn't built here */

/* DoH would recurse HTTP-over-DNS through this same stack for a
 * resolver this port doesn't have a working transport for anyway
 * (see the getaddrinfo note below) -- not worth it. */
#define CURL_DISABLE_DOH 1

/* alt-svc/HSTS cache persistence is file-based and keyed off real
 * wall-clock expiry timestamps, neither of which this port has (BMFS
 * has no subdirectories for a cache path to matter and there's no
 * clock but CLOCK_MONOTONIC -- see posix_shim.c). */
#define CURL_DISABLE_ALTSVC 1
#define CURL_DISABLE_HSTS 1

/* .netrc lookup wants a $HOME to search and BMFS's flat namespace
 * (see OPENISSUES.md) has no directory structure for one to live in. */
#define CURL_DISABLE_NETRC 1

/* No proxy support wired into net_shim.c/dns_shim.c -- this port talks
 * straight to the origin server. */
#define CURL_DISABLE_PROXY 1

/* NTLM/Kerberos/Negotiate/SASL-GSSAPI are all opt-in (CURL_ENABLE_NTLM,
 * or gated on HAVE_GSSAPI which is correctly left undefined below,
 * since there's no krb5/GSS-API library vendored here) -- leaving them
 * out needs no explicit CURL_DISABLE_*. Basic and Digest HTTP auth stay
 * enabled: both are self-contained (base64/MD5 curl already bundles),
 * no extra port support needed.
 */

/* ---------------------------------------------------------------------
 * TLS backend: mbedTLS, the same vendored copy port/tls_shim.c uses
 * (see scripts/get-mbedtls.sh, build-app.sh's MBEDTLS_CFLAGS). curl's
 * own vtls/mbedtls.c talks to mbedTLS directly (its own net_sockets.c
 * glue over this port's socket() family, its own RNG via
 * mbedtls_ctr_drbg fed by port/mbedtls_port/entropy_hardware_poll.c's
 * RDRAND source) -- tls_shim.c itself isn't reused here, it's a
 * different, narrower wrapper built for crawler.c/https_crawler.c.
 * --------------------------------------------------------------------- */

#define USE_MBEDTLS 1
#define CURL_DEFAULT_SSL_BACKEND "mbedtls"

/* ---------------------------------------------------------------------
 * Name resolution: gethostbyname() only (dns_shim.c), no getaddrinfo()
 * --------------------------------------------------------------------- */

/* musl *does* link a real getaddrinfo()/freeaddrinfo() (unlike
 * gethostbyname(), dns_shim.c doesn't shadow it) -- but musl's
 * getaddrinfo() reads /etc/resolv.conf, which nothing on this BMFS
 * image writes, so (exactly like dns_shim.c's own file-header comment
 * explains for why it shadows gethostbyname() at all) it would just
 * fall back to querying 127.0.0.1, which nothing answers. Leaving
 * HAVE_GETADDRINFO undefined routes curl's resolver through the older
 * gethostbyname()-based path in lib/hostip.c/lib/curl_addrinfo.c
 * instead, which *does* hit dns_shim.c's real (working) resolver. */
/* #undef HAVE_GETADDRINFO */
/* #undef HAVE_GETADDRINFO_THREADSAFE */
/* #undef HAVE_FREEADDRINFO */

/* dns_shim.c provides plain (non-reentrant) gethostbyname() only, by
 * design (single-threaded port, see its file header) -- no
 * gethostbyname_r(). Leaving these undefined makes curl call plain
 * gethostbyname() under its own lock instead of a _r variant that
 * doesn't exist here. */
/* #undef HAVE_GETHOSTBYNAME_R */
/* #undef HAVE_GETHOSTBYNAME_R_6 */

/* uname() isn't implemented (OPENISSUES.md), and musl's gethostname()
 * is backed by it. */
/* #undef HAVE_GETHOSTNAME */

/* #undef USE_IPV6 */ /* matches LWIP_IPV6=0 in port/lwip_port/lwipopts.h */

/* ---------------------------------------------------------------------
 * Threading: none. Single-threaded port (OPENISSUES.md: pthread_create
 * isn't wired up, no clone()). Leaving HAVE_THREADS_POSIX undefined
 * compiles lib/curl_threads.c's Curl_mutex_* down to no-ops instead of
 * real pthread_mutex_* calls that would otherwise (harmlessly, since
 * nothing ever contends them, but needlessly) depend on musl's
 * futex-based mutex fast path working here.
 * --------------------------------------------------------------------- */

/* #undef HAVE_THREADS_POSIX */

/* ---------------------------------------------------------------------
 * Sockets / I/O this port actually backs
 * --------------------------------------------------------------------- */

#define HAVE_SOCKET 1
#define HAVE_RECV 1
#define HAVE_SEND 1
#define HAVE_STRUCT_SOCKADDR_STORAGE 1
#define HAVE_SA_FAMILY_T 1
#define HAVE_ARPA_INET_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_NETDB_H 1
#define HAVE_NET_IF_H 1
/* #undef HAVE_SYS_SOCKIO_H */
#define HAVE_INET_PTON 1
#define HAVE_INET_NTOP 1
#define HAVE_ACCEPT4 1 /* SYS_accept4 -- posix_shim.c's sys_accept4() */

/* select()/poll() -- both wired up in posix_shim.c (see its
 * "select()/poll()" section): every fd this port recognizes is
 * reported ready immediately, with the real blocking happening for
 * real inside whichever recv()/send() follows. Good enough for
 * libcurl's easy-interface transfer loop (lib/select.c's
 * Curl_socket_check()), which is the only consumer that matters here. */
#define HAVE_SELECT 1
#define HAVE_STRUCT_TIMEVAL 1
#define HAVE_SUSECONDS_T 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_POLL 1
#define HAVE_POLL_H 1
#define HAVE_SYS_POLL_H 1

/* fcntl() is an accept-and-ignore stub in posix_shim.c (every socket
 * is unconditionally blocking regardless of O_NONBLOCK) -- but it
 * always returns success, so curlx/nonblock.c's F_SETFL/O_NONBLOCK
 * path "works" (silently becomes a no-op) rather than falling through
 * to an ioctl(FIONBIO) path this port doesn't back either. */
#define HAVE_FCNTL 1
#define HAVE_FCNTL_H 1
#define HAVE_FCNTL_O_NONBLOCK 1

/* getsockname()/getpeername()/sendmsg()/sendmmsg() -- no SYS_* case
 * for any of these in posix_shim.c's dispatcher (-ENOSYS). Only used
 * by curl for local/peer-address introspection (CURLINFO_LOCAL_IP and
 * friends) and QUIC/zerocopy sends, neither load-bearing for a plain
 * HTTP(S) GET. */
/* #undef HAVE_GETSOCKNAME */
/* #undef HAVE_GETPEERNAME */
/* #undef HAVE_SENDMSG */
/* #undef HAVE_SENDMMSG */

/* No AF_UNIX (socketpair() isn't wired up -- net_shim.c is IPv4 TCP/UDP
 * only), no eventfd()/pipe()/pipe2() either -- posix_shim.c doesn't
 * handle any of their syscalls. These only back curl_multi_wakeup(),
 * for interrupting a curl_multi_poll() from another thread -- moot on
 * a single-threaded port; lib/multi_ntfy.c degrades to "no wakeup
 * mechanism" when none of these are available. */
/* #undef HAVE_SOCKETPAIR */
/* #undef HAVE_SYS_UN_H */
/* #undef HAVE_EVENTFD */
/* #undef HAVE_SYS_EVENTFD_H */
/* #undef HAVE_PIPE */
/* #undef HAVE_PIPE2 */

/* The graceful "no wakeup mechanism" degradation the comment above
 * describes doesn't actually happen just from leaving the HAVE_*
 * probes above undefined: lib/multihandle.h's ENABLE_WAKEUP is gated
 * on this separate, curl-internal feature-disable macro (not any
 * HAVE_* capability probe), and defaults to *on* unless told
 * otherwise. Left on, Curl_multi_handle() (lib/multi.c) calls
 * Curl_wakeup_init() (lib/socketpair.c), whose only non-eventfd/pipe
 * implementation left standing here is a real socketpair()/loopback
 * fallback this port has none of -- every curl_easy_perform() then
 * fails Curl_multi_handle() and surfaces as an oddly generic
 * CURLE_OUT_OF_MEMORY (easy.c's easy_perform(), on Curl_multi_handle()
 * returning NULL) with no other diagnostic, for a connection that
 * never even reached the network. */
#define CURL_DISABLE_SOCKETPAIR 1

/* No interface enumeration (single hard-coded NIC, net_glue.c) and no
 * ioctl(SIOCGIFADDR/SIOCGIFINDEX) support in posix_shim.c's sys_ioctl()
 * (TIOCGWINSZ only) -- only matters for CURLOPT_INTERFACE by name. */
/* #undef HAVE_GETIFADDRS */
/* #undef HAVE_IFADDRS_H */
/* #undef HAVE_IF_NAMETOINDEX */
/* #undef HAVE_IOCTL_SIOCGIFADDR */

/* ---------------------------------------------------------------------
 * Time -- see posix_shim.c's "Time" section. CLOCK_MONOTONIC (backed by
 * b_system(TIMECOUNTER, ...), the same source lwIP's sys_now() already
 * uses) is real; CLOCK_REALTIME/gettimeofday()/time() are not -- there
 * is still no wall-clock/RTC source on this port (OPENISSUES.md).
 * --------------------------------------------------------------------- */

#define HAVE_CLOCK_GETTIME_MONOTONIC 1
/* #undef HAVE_CLOCK_GETTIME_MONOTONIC_RAW */
/* #undef HAVE_GETTIMEOFDAY */
/* #undef HAVE_ALARM */

/* gmtime_r()/localtime_r() are real, working musl library functions
 * (no syscall involved) -- just never fed a meaningful wall-clock
 * time_t here. Harmless to leave enabled: only reached parsing dates
 * out of HTTP response headers (e.g. a "Date:"/"Last-Modified:" the
 * *server* sent), never to learn what time it "is" locally. */
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1

/* ---------------------------------------------------------------------
 * Everything else not implemented by posix_shim.c/bmfs.c -- see
 * OPENISSUES.md for the underlying reasons (grouped as: no permissions/
 * users, no process introspection, no rlimits, no xattr, no tty).
 * --------------------------------------------------------------------- */

/* #undef HAVE_GETEUID */
/* #undef HAVE_GETPWUID */
/* #undef HAVE_GETPWUID_R */
/* #undef HAVE_PWD_H */
/* #undef HAVE_GETPPID */
/* #undef HAVE_GETRLIMIT */
/* #undef HAVE_SETRLIMIT */
/* #undef HAVE_SYS_RESOURCE_H */
/* #undef HAVE_FSETXATTR */
/* #undef HAVE_FSETXATTR_5 */
/* #undef HAVE_FSETXATTR_6 */
/* #undef HAVE_SYS_XATTR_H */
/* #undef HAVE_TERMIOS_H */
/* #undef HAVE_SIGINTERRUPT */
/* #undef HAVE_UTIME */
/* #undef HAVE_UTIMES */
/* #undef HAVE_UTIME_H */
/* #undef HAVE_DIRENT_H */
/* #undef HAVE_OPENDIR */
/* #undef HAVE_DLFCN_H */
/* #undef HAVE_ARC4RANDOM */
/* #undef HAVE_BASENAME */
/* #undef HAVE_LIBGEN_H */
/* #undef HAVE_FNMATCH */
/* #undef HAVE_SCHED_YIELD */
/* #undef HAVE_MEMSET_EXPLICIT */
/* #undef HAVE_MEMSET_S */

/* Real, working musl library functions/headers needing no syscall
 * support this port lacks -- kept as detected. */
#define HAVE_SIGACTION 1
#define HAVE_SIGNAL 1
#define HAVE_SIGSETJMP 1
/* musl's sysroot doesn't vendor a stdatomic.h (own float.h/limits.h/
 * stdarg.h/stdbool.h are present, this one isn't) -- and there's no
 * reason for a single-threaded port to want lock-free atomics anyway.
 * lib/easy_lock.h falls back to an unsynchronized no-op lock when
 * neither this nor HAVE_THREADS_POSIX (also unavailable, see above) is
 * defined, which is the accurate description of this port. */
/* #undef HAVE_STDATOMIC_H */
/* #undef HAVE_ATOMIC */
#define HAVE_STDBOOL_H 1
#define HAVE_BOOL_T 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRCASECMP 1
#define HAVE_INTTYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_PARAM_H 1
/* #undef HAVE_LINUX_TCP_H */ /* musl's sysroot doesn't vendor this uapi header -- <netinet/tcp.h> (kept below) already covers what curl needs */
#define HAVE_NETINET_UDP_H 1
#define HAVE_LOCALE_H 1
#define HAVE_SETLOCALE 1
#define HAVE_DECL_FSEEKO 1
#define HAVE_FSEEKO 1

/* musl's strerror_r() is the POSIX (int-returning) form, not glibc's
 * (char*-returning) extension. */
#define HAVE_STRERROR_R 1
#define HAVE_POSIX_STRERROR_R 1
/* #undef HAVE_GLIBC_STRERROR_R */

/* ---------------------------------------------------------------------
 * Sizes -- x86-64 musl is LP64, same as x86-64 glibc: identical to
 * what a native ./configure run reports.
 * --------------------------------------------------------------------- */

#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_OFF_T 8
#define SIZEOF_SIZE_T 8
#define SIZEOF_TIME_T 8
#define SIZEOF_CURL_OFF_T 8
#define SIZEOF_CURL_SOCKET_T 4

#define GETHOSTNAME_TYPE_ARG2 size_t

/* ---------------------------------------------------------------------
 * Package identity -- cosmetic (curl -V banner / default User-Agent
 * fallback), not derived from a real configure/git-describe run here.
 * --------------------------------------------------------------------- */

#define CURL_OS "x86_64-baremetal"
#define PACKAGE "curl"
#define PACKAGE_NAME "curl"
#define PACKAGE_TARNAME "curl"
#define PACKAGE_VERSION "8.21.0"
#define PACKAGE_STRING "curl 8.21.0"
#define PACKAGE_BUGREPORT "a suitable curl mailing list: https://curl.se/mail/"
#define PACKAGE_URL ""
#define VERSION "8.21.0"

#define STDC_HEADERS 1

#endif /* CURL_PORT_CURL_CONFIG_H */
