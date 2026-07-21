#!/usr/bin/env bash
set -e

# Build a BareMetal app against the musl/lwIP port in this directory.
#
# Usage: ./build-app.sh yourapp.c [otherfile.c ...]
# The output is named after the first source file given, with a .app
# extension (e.g. myapp.c -> myapp.app).

BUILD_DIR="build"

MUSL_DIR="$BUILD_DIR/musl-1.2.6"
MUSL_INC="$MUSL_DIR/sysroot/usr/local/musl/include"
MUSL_LIB="$MUSL_DIR/lib/libc.a"

LWIP_DIR="$BUILD_DIR/lwip-2.2.0"
LWIP_INC="$LWIP_DIR/src/include"
LWIP_PORT="port/lwip_port"

PORT="port"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ $# -eq 0 ]; then
	echo "usage: $0 yourapp.c [otherfile.c ...]" >&2
	exit 1
fi

if [ ! -f "$MUSL_DIR/config.mak" ]; then
	echo "error: $MUSL_DIR is missing -- run ./setup.sh first." >&2
	exit 1
fi

if [ ! -d "$LWIP_DIR" ]; then
	echo "error: $LWIP_DIR is missing -- run ./setup.sh first." >&2
	exit 1
fi

mkdir -p "$BUILD_DIR"

APP_SRCS=("$@")
APP_NAME="$(basename "${APP_SRCS[0]}" .c).app"

# BareMetal apps are flat binaries loaded at a fixed high-canonical
# address (0xFFFF800000000000, see port/c.ld), with no syscall trap and
# no dynamic linker. PIC/PIE codegen (GOT-relative loads with nothing
# to resolve them against) and the small code model's 32-bit-relative
# address assumptions both break under that, hence -fno-pic -fno-pie
# -mcmodel=large. See port/posix_shim.c and port/crt0.c for the
# syscall/startup side of the port.
CFLAGS="-c -m64 -nostdlib -nostartfiles -nodefaultlibs -ffreestanding -fno-pic -fno-pie -mcmodel=large -falign-functions=16 -fomit-frame-pointer -mno-red-zone -fno-builtin -fno-stack-protector -nostdinc -isystem $MUSL_INC"

# lwIP headers pull in musl's (via -isystem above) for size_t/
# stdint/etc., plus its own lwip/ and netif/ trees, plus our port's
# lwipopts.h and arch/{cc,sys_arch}.h -- see port/net_glue.c/net_shim.c.
LWIP_CFLAGS="$CFLAGS -I $LWIP_INC -I $LWIP_PORT"

# Build musl's libc.a, and the merged header sysroot posix_shim.c/
# app sources compile against.
make -C "$MUSL_DIR" lib/libc.a
make -C "$MUSL_DIR" install-headers DESTDIR="$(pwd)/$MUSL_DIR/sysroot"

gcc $CFLAGS -o "$BUILD_DIR/crt0.o" "$PORT/crt0.c"
gcc $CFLAGS -o "$BUILD_DIR/posix_shim.o" "$PORT/posix_shim.c"
gcc $CFLAGS -o "$BUILD_DIR/bmfs.o" "$PORT/bmfs.c"
gcc $LWIP_CFLAGS -o "$BUILD_DIR/net_glue.o" "$PORT/net_glue.c"
gcc $LWIP_CFLAGS -o "$BUILD_DIR/net_shim.o" "$PORT/net_shim.c"
gcc $CFLAGS -o "$BUILD_DIR/libBareMetal.o" "$PORT/libBareMetal.c"

APP_OBJS=""
for src in "${APP_SRCS[@]}"; do
	obj="$BUILD_DIR/$(basename "$src" .c).o"
	gcc $CFLAGS -o "$obj" "$src"
	APP_OBJS="$APP_OBJS $obj"
done

# lwIP core: IPv4 + Ethernet + ARP + DHCP + TCP (+ UDP, which DHCP
# needs internally) only -- no IPv6, no AutoIP/IGMP/raw sockets/ACD
# (see port/lwip_port/lwipopts.h), so their source files aren't built.
LWIP_SRCS="
	core/def.c core/inet_chksum.c core/init.c core/ip.c core/mem.c
	core/memp.c core/netif.c core/pbuf.c core/stats.c core/sys.c
	core/tcp.c core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c
	core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c
	core/ipv4/ip4_addr.c core/ipv4/ip4.c core/ipv4/ip4_frag.c
	netif/ethernet.c
"
LWIP_OBJS=""
for src in $LWIP_SRCS; do
	obj="$BUILD_DIR/lwip_$(basename "$src" .c).o"
	gcc $LWIP_CFLAGS -o "$obj" "$LWIP_DIR/src/$src"
	LWIP_OBJS="$LWIP_OBJS $obj"
done

ld -T "$PORT/c.ld" -o "$APP_NAME" "$BUILD_DIR/crt0.o" "$BUILD_DIR/posix_shim.o" \
	"$BUILD_DIR/bmfs.o" "$BUILD_DIR/net_glue.o" "$BUILD_DIR/net_shim.o" \
	"$BUILD_DIR/libBareMetal.o" $APP_OBJS $LWIP_OBJS "$MUSL_LIB"

echo "Built $APP_NAME"
