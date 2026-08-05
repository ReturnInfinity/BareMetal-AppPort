#!/bin/bash
set -e

BOLD="\033[1m"
NORMAL="\033[0m"

# Fetches and patches musl 1.2.6, lwIP 2.2.0, and mbedTLS 3.6.6 into this
# directory, then builds musl's libc.a and the lwIP/mbedTLS object files
# that build-app.sh links against. Run once before ./build-app.sh.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo -e "${BOLD}Pulling libraries${NORMAL}"

"$SCRIPT_DIR/scripts/get-musl.sh"
"$SCRIPT_DIR/scripts/get-lwip.sh"
"$SCRIPT_DIR/scripts/get-mbedtls.sh"
"$SCRIPT_DIR/scripts/get-curl.sh"

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

mkdir -p "$BUILD_DIR"

echo -e "${BOLD}Building libraries${NORMAL}"

# Build musl's libc.a, and the merged header sysroot posix_shim.c/app
# sources compile against.
echo "Building musl"
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
echo "Building lwIP"
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
echo "Building mbedtls"
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
echo "Building curl"
for src in "$CURL_DIR"/lib/*.c "$CURL_DIR"/lib/curlx/*.c "$CURL_DIR"/lib/vauth/*.c "$CURL_DIR"/lib/vtls/*.c "$CURL_DIR"/lib/vquic/*.c; do
	obj="$BUILD_DIR/curl_$(basename "$src" .c).o"
	gcc $CURL_CFLAGS -o "$obj" "$src"
done

# echo -e "${BOLD}Library builds complete${NORMAL}"
