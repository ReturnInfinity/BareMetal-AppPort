#!/bin/bash
set -e

BOLD="\033[1m"
NORMAL="\033[0m"

# Fetches and patches musl 1.2.6, lwIP 2.2.0, mbedTLS 3.6.6, and lwext4
# into this directory, then builds musl's libc.a and the lwIP/mbedTLS/
# lwext4 object files that build-app.sh links against. Run once before
# ./build-app.sh.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo -e "${BOLD}Pulling libraries${NORMAL}"

"$SCRIPT_DIR/scripts/get-musl.sh"
"$SCRIPT_DIR/scripts/get-lwip.sh"
"$SCRIPT_DIR/scripts/get-mbedtls.sh"
"$SCRIPT_DIR/scripts/get-curl.sh"
"$SCRIPT_DIR/scripts/get-sqlite.sh"
"$SCRIPT_DIR/scripts/get-libsodium.sh"
"$SCRIPT_DIR/scripts/get-lwext4.sh"

BUILD_DIR="build"

MUSL_DIR="$BUILD_DIR/musl-1.2.6"
MUSL_INC="$MUSL_DIR/sysroot/usr/local/musl/include"

LWIP_DIR="$BUILD_DIR/lwip-2.2.0"
LWIP_INC="$LWIP_DIR/src/include"
LWIP_PORT="port/lwip_port"

MBEDTLS_DIR="$BUILD_DIR/mbedtls-3.6.6"
MBEDTLS_INC="$MBEDTLS_DIR/include"
MBEDTLS_PORT="port/mbedtls_port"

CURL_DIR="$BUILD_DIR/curl-8.21.0"
CURL_INC="$CURL_DIR/include"
CURL_LIB="$CURL_DIR/lib"
CURL_PORT="port/curl_port"

SQLITE_DIR="$BUILD_DIR/sqlite-3.46.1"
SQLITE_PORT="port/sqlite_port"

SODIUM_DIR="$BUILD_DIR/libsodium-1.0.22/src/libsodium"
SODIUM_INC="$SODIUM_DIR/include"
SODIUM_PORT="port/libsodium_port"

LWEXT4_DIR="$BUILD_DIR/lwext4-58bcf89"
LWEXT4_INC="$LWEXT4_DIR/include"
LWEXT4_PORT="port/lwext4_port"

# Run a command, staying silent unless it fails -- then dump its output
# and abort. Keeps musl/lwIP's noisy per-file build logs off the screen
# on the (common) successful case.
run_quiet() {
	local log
	log="$(mktemp)"
	if ! "$@" >"$log" 2>&1; then
		cat "$log"
		rm -f "$log"
		exit 1
	fi
	rm -f "$log"
}

# Same freestanding/no-PIC/large-code-model flags build-app.sh uses for
# the app and port shims -- see that script's CFLAGS comment. musl/
# lwIP/mbedTLS/curl objects link directly into the same flat BareMetal
# binary, so they're built against this target too.
#
# -ffunction-sections/-fdata-sections put each function/global in its
# own linker section instead of one blob per translation unit, so
# build-app.sh's final `ld --gc-sections` can drop whatever a given
# app doesn't actually call -- lwIP/mbedTLS/curl are linked into every
# app regardless of use (see build-app.sh), so most apps only reach a
# small fraction of what setup.sh builds here. musl's own libc.a
# already builds this way (see port/musl_port/musl-1.2.6-config.mak's
# CFLAGS_AUTO); this makes lwIP/mbedTLS/curl match it.
CFLAGS="-c -m64 -nostdlib -nostartfiles -nodefaultlibs -ffreestanding -fno-pic -fno-pie -mcmodel=large -falign-functions=16 -fomit-frame-pointer -mno-red-zone -fno-builtin -fno-stack-protector -ffunction-sections -fdata-sections -nostdinc -isystem $MUSL_INC"
LWIP_CFLAGS="$CFLAGS -I $LWIP_INC -I $LWIP_PORT"
MBEDTLS_CFLAGS="$CFLAGS -I $MBEDTLS_INC -I $MBEDTLS_PORT -DMBEDTLS_CONFIG_FILE=\"baremetal_mbedtls_config.h\""

# -DHAVE_CONFIG_H points curl's own lib/curl_setup.h at
# port/curl_port/curl_config.h (see that file's header for how it was
# derived and why); -DBUILDING_LIBCURL is curl's own internal-vs-public
# marker; -DCURL_STATICLIB tells the *public* curl/curl.h (which
# doesn't go through curl_config.h -- see curl_config.h's CURL_STATICLIB
# comment) to skip dllexport/visibility decorations, matching this
# port's one-flat-static-binary model. -I $CURL_DIR/lib is needed
# because curl's own lib/vtls/mbedtls.c etc. quote-include sibling
# headers like "urldata.h" as if compiled from lib/ itself (matches
# curl's own upstream Makefile.am's AM_CPPFLAGS). mbedTLS's include
# path/config define are repeated here because vtls/mbedtls.c talks to
# mbedTLS directly, not through tls_shim.c.
CURL_CFLAGS="$CFLAGS -DHAVE_CONFIG_H -DBUILDING_LIBCURL -DCURL_STATICLIB -I $CURL_PORT -I $CURL_INC -I $CURL_LIB -I $MBEDTLS_INC -I $MBEDTLS_PORT -DMBEDTLS_CONFIG_FILE=\"baremetal_mbedtls_config.h\""

# -DSQLITE_CUSTOM_INCLUDE points sqlite3.c's own early-include hook at
# port/sqlite_port/sqlite_baremetal_config.h (see that file's header
# for what it sets and why -- SQLITE_OS_OTHER=1 chief among them,
# which is also why there's no "-I $SQLITE_PORT"-driven mbedTLS-style
# vtls integration here: unlike curl, sqlite3.c itself never needs
# anything else from this port -- port/sqlite_port/sqlite_vfs.c, built
# per-app in build-app.sh like tls_shim.c/net_shim.c, is what actually
# talks to posix_shim.c/ext4_shim.c). -DNDEBUG disables assert(): this
# port's abort() has nowhere clean to unwind to (no signal delivery --
# see OPENISSUES.md), and NDEBUG is SQLite's own documented stance for
# a release build anyway.
SQLITE_CFLAGS="$CFLAGS -I $SQLITE_PORT -DSQLITE_CUSTOM_INCLUDE=sqlite_baremetal_config.h -DNDEBUG"

# -I $SODIUM_INC/sodium matches libsodium's own src/libsodium/Makefile.am
# (libsodium_la_CPPFLAGS: "-I$(srcdir)/include/sodium") -- every internal
# .c file quote-includes its own public/private headers ("private/common.h",
# "crypto_stream_chacha20.h", "utils.h", ...) as if compiled from
# include/sodium/ itself. -DSODIUM_STATIC makes export.h's SODIUM_EXPORT
# expand to nothing instead of __attribute__((visibility("default")));
# this is a flat static link with no .so, so there's nothing for a
# visibility attribute to buy here. -DCONFIGURED=1 is libsodium's own
# documented escape hatch (see include/sodium/private/common.h) for
# exactly this situation -- a hand-built, non-./configure compile --
# silencing its "undocumented method" #warning; no actual config.h is
# generated or needed (see setup.sh's "Building libsodium" comment below
# for why not).
SODIUM_CFLAGS="$CFLAGS -DSODIUM_STATIC -DCONFIGURED=1 -I $SODIUM_INC -I $SODIUM_INC/sodium"

# lwext4 headers pull in musl's the same way, plus lwext4's own
# include/ tree. -DCONFIG_USE_DEFAULT_CFG=0 makes lwext4's own
# include/ext4_config.h pull in "generated/ext4_config.h" (a
# quote-form include, found here via the -I $LWEXT4_PORT below -- see
# port/lwext4_port/generated/ext4_config.h for what's changed and why:
# EXT2-only feature set, no journal/extents, musl's errno.h/fcntl.h
# codes instead of lwext4's own). Only used for lwext4's own vendored
# sources below -- port/ext4_shim.c and port/lwext4_port/
# blockdev_baremetal.c are our own port glue, built per-app in
# build-app.sh instead, like tls_shim.c/sqlite_vfs.c.
LWEXT4_CFLAGS="$CFLAGS -I $LWEXT4_INC -I $LWEXT4_PORT -DCONFIG_USE_DEFAULT_CFG=0"

mkdir -p "$BUILD_DIR"

echo -e "${BOLD}Building libraries${NORMAL}"

# Build musl's libc.a, and the merged header sysroot posix_shim.c/app
# sources compile against.
echo "- Building musl"
run_quiet make -C "$MUSL_DIR" lib/libc.a
run_quiet make -C "$MUSL_DIR" install-headers DESTDIR="$(pwd)/$MUSL_DIR/sysroot"

# lwIP core: IPv4 + Ethernet + ARP + DHCP + TCP + UDP + DNS only -- no
# IPv6, no AutoIP/IGMP/raw sockets/ACD (see port/lwip_port/lwipopts.h),
# so their source files aren't built.
LWIP_SRCS="
	core/def.c core/inet_chksum.c core/init.c core/ip.c core/mem.c
	core/memp.c core/netif.c core/pbuf.c core/stats.c core/sys.c
	core/tcp.c core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c
	core/dns.c
	core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c
	core/ipv4/ip4_addr.c core/ipv4/ip4.c core/ipv4/ip4_frag.c
	netif/ethernet.c
"
echo "- Building lwIP"
for src in $LWIP_SRCS; do
	obj="$BUILD_DIR/lwip_$(basename "$src" .c).o"
	run_quiet gcc $LWIP_CFLAGS -o "$obj" "$LWIP_DIR/src/$src"
done

# Unlike LWIP_SRCS above, this isn't a hand-picked subset: every
# library/*.c file in mbedTLS is individually guarded by
# "#if defined(MBEDTLS_<ITS_OWN_MODULE>_C)" around its entire contents
# (that's how mbedTLS's own Makefile/CMake builds it too), so a module
# our baremetal_mbedtls_config.h leaves disabled just compiles down to
# an empty translation unit -- no curated file list to keep in sync by
# hand.
echo "- Building mbedtls"
for src in "$MBEDTLS_DIR"/library/*.c; do
	obj="$BUILD_DIR/mbedtls_$(basename "$src" .c).o"
	gcc $MBEDTLS_CFLAGS -o "$obj" "$src"
done

# Like mbedTLS above (and unlike lwIP's hand-picked LWIP_SRCS list):
# every curl lib/*.c file -- protocol handlers (ftp.c, telnet.c, ...),
# TLS backends (vtls/openssl.c, vtls/gtls.c, ...), everything -- is
# individually guarded by its own "#ifndef CURL_DISABLE_FOO"/
# "#ifdef USE_FOO" wrapping the *entire* file body including its own
# #includes (verified against curl 8.21.0's actual source, not just
# assumed), so a module curl_config.h leaves off compiles down to an
# empty translation unit rather than needing a curated source list.
# vssh/ (SCP/SFTP: libssh/libssh2) is skipped entirely -- unlike
# vquic/ below, nothing outside vssh/ calls into it unconditionally,
# so there's no reason to build 0 useful bytes from an extra directory
# this port will never enable.
#
# vquic/ (HTTP/3: ngtcp2/nghttp3/quiche) *is* built even though HTTP/3
# itself is unreachable here (no QUIC/UDP-transport TLS, and this
# port's UDP support has no listen/accept -- see OPENISSUES.md):
# lib/vquic/vquic.c's Curl_conn_may_http3() is called unconditionally
# from lib/http.c/lib/cf-https-connect.c's generic connection-setup
# path, and provides its own "#else /* CURL_DISABLE_HTTP || !USE_HTTP3
# */" fallback stub for exactly this configuration -- the other
# vquic/*.c files (cf-ngtcp2*.c, cf-quiche.c, ...) still compile down
# to nothing, same as everywhere else in this list.
echo "- Building curl"
for src in "$CURL_DIR"/lib/*.c "$CURL_DIR"/lib/curlx/*.c "$CURL_DIR"/lib/vauth/*.c "$CURL_DIR"/lib/vtls/*.c "$CURL_DIR"/lib/vquic/*.c; do
	obj="$BUILD_DIR/curl_$(basename "$src" .c).o"
	gcc $CURL_CFLAGS -o "$obj" "$src"
done

# SQLite's amalgamation: one file, sqlite3.c, containing the whole
# library (core, VDBE, all the SQL functions/pragmas the default build
# includes, everything) -- no curated file list needed, unlike lwIP's
# LWIP_SRCS above. port/sqlite_port/sqlite_vfs.c (the VFS this amalgam
# calls into via SQLITE_OS_OTHER -- see SQLITE_CFLAGS's comment) is
# built per-app in build-app.sh instead, alongside this port's other
# own glue (tls_shim.c, net_shim.c, ext4_shim.c, ...), not here.
echo "- Building sqlite"
run_quiet gcc $SQLITE_CFLAGS -o "$BUILD_DIR/sqlite_sqlite3.o" "$SQLITE_DIR/sqlite3.c"

# Like mbedTLS/curl above: libsodium's own src/libsodium/Makefile.am
# unconditionally lists *every* implementation file for every primitive in
# libsodium_la_SOURCES -- portable "ref"/"donna"/"soft" C alongside SSE2/
# SSSE3/AVX2/AVX512/AES-NI/ARMv8-crypto variants -- and every one of the
# latter wraps its entire body in "#if defined(HAVE_AVX2INTRIN_H) && ..."
# (verified against libsodium 1.0.22's actual source), where the
# HAVE_*INTRIN_H/HAVE_TI_MODE/HAVE_AMD64_ASM/HAVE_AVX_ASM macros are
# ordinarily set by libsodium's own ./configure probing the host
# compiler. This port never runs that configure (no config.h is
# generated -- SODIUM_CFLAGS above passes none of those macros), so
# every SIMD/asm variant compiles down to an empty translation unit and
# each primitive's own runtime dispatcher (e.g. crypto_stream_chacha20's
# stream_chacha20.c) is left with only its ref/portable implementation to
# select -- the same fallback path libsodium itself takes on a compiler
# lacking those intrinsics, not a hand-trimmed subset. The only files
# skipped outright are randombytes/sysrandom/* and randombytes/internal/*:
# unlike the SIMD variants above, those aren't gated by compiler-capability
# macros -- they unconditionally want /dev/urandom, getrandom(), or
# getentropy(), none of which exist here (see OPENISSUES.md) -- so
# port/libsodium_port/randombytes_baremetal.c (built per-app in
# build-app.sh, like tls_shim.c/sqlite_vfs.c) stands in for them instead,
# the same way port/mbedtls_port/entropy_hardware_poll.c stands in for
# mbedTLS's platform entropy source: RDRAND, with an RDTSC fallback.
echo "- Building libsodium"
SODIUM_SRCS=$(find "$SODIUM_DIR" -name '*.c' \
	! -path '*/randombytes/sysrandom/*' \
	! -path '*/randombytes/internal/*')
for src in $SODIUM_SRCS; do
	obj="$BUILD_DIR/sodium_$(basename "$src" .c).o"
	gcc $SODIUM_CFLAGS -o "$obj" "$src"
done

# Like mbedTLS/curl/libsodium above: every src/*.c file in lwext4 is
# compiled unconditionally, whether or not the feature it implements
# (journaling, extents, xattrs) is enabled in our EXT2-only config
# (port/lwext4_port/generated/ext4_config.h) -- a disabled feature's
# file either compiles down to an empty translation unit (internally
# guarded by its own "#if CONFIG_..._ENABLE") or simply goes unused
# (still linked in, just never called), same posture as the others.
echo "- Building lwext4"
for src in "$LWEXT4_DIR"/src/*.c; do
	obj="$BUILD_DIR/lwext4_$(basename "$src" .c).o"
	gcc $LWEXT4_CFLAGS -o "$obj" "$src"
done

# echo -e "${BOLD}Library builds complete${NORMAL}"
