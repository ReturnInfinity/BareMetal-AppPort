#!/bin/bash
set -e

BOLD="\033[1m"
NORMAL="\033[0m"

# Fetches and patches musl 1.2.6, lwIP 2.2.0, and mbedTLS 3.6.6 into this
# directory, then builds musl's libc.a and the lwIP/mbedTLS object files
# that build-app.sh links against. Run once before ./build-app.sh.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo -e "This is ${BOLD}Pulling libraries${NORMAL}"

"$SCRIPT_DIR/scripts/get-musl.sh"
"$SCRIPT_DIR/scripts/get-lwip.sh"
"$SCRIPT_DIR/scripts/get-mbedtls.sh"

BUILD_DIR="build"

MUSL_DIR="$BUILD_DIR/musl-1.2.6"
MUSL_INC="$MUSL_DIR/sysroot/usr/local/musl/include"

LWIP_DIR="$BUILD_DIR/lwip-2.2.0"
LWIP_INC="$LWIP_DIR/src/include"
LWIP_PORT="port/lwip_port"

MBEDTLS_DIR="$BUILD_DIR/mbedtls-3.6.6"
MBEDTLS_INC="$MBEDTLS_DIR/include"
MBEDTLS_PORT="port/mbedtls_port"

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
# lwIP/mbedTLS objects link directly into the same flat BareMetal
# binary, so they're built against this target too.
CFLAGS="-c -m64 -nostdlib -nostartfiles -nodefaultlibs -ffreestanding -fno-pic -fno-pie -mcmodel=large -falign-functions=16 -fomit-frame-pointer -mno-red-zone -fno-builtin -fno-stack-protector -nostdinc -isystem $MUSL_INC"
LWIP_CFLAGS="$CFLAGS -I $LWIP_INC -I $LWIP_PORT"
MBEDTLS_CFLAGS="$CFLAGS -I $MBEDTLS_INC -I $MBEDTLS_PORT -DMBEDTLS_CONFIG_FILE=\"baremetal_mbedtls_config.h\""

mkdir -p "$BUILD_DIR"

echo -e "This is ${BOLD}Building libraries${NORMAL}"

# Build musl's libc.a, and the merged header sysroot posix_shim.c/app
# sources compile against.
echo "Building musl..."
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
echo "Building lwIP..."
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
echo "Building mbedtls..."
for src in "$MBEDTLS_DIR"/library/*.c; do
	obj="$BUILD_DIR/mbedtls_$(basename "$src" .c).o"
	gcc $MBEDTLS_CFLAGS -o "$obj" "$src"
done

echo -e "This is ${BOLD}Library builds complete${NORMAL}"
