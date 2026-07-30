#!/usr/bin/env bash
set -e

# Build a BareMetal app against the musl/lwIP/mbedTLS port in this directory.
#
# Usage: ./build-app.sh yourapp.c [otherfile.c ...]
# The output is named after the first source file given, with a .app
# extension (e.g. myapp.c -> myapp.app).

BUILD_DIR="build"

# Set BAREMETAL_DEBUG=TRUE in the environment to compile out net_glue.c's
# diagnostic printf's (fc cmdline parsing, DHCP/DNS fallback, etc). Left
# unset/FALSE, they print as normal.
BAREMETAL_DEBUG=TRUE

# mbedTLS's bignum code (library/bignum.c) does 128-by-64-bit division
# via __int128, which x86-64 has no single instruction for -- gcc emits
# a call to __udivti3, normally satisfied by libgcc. -nodefaultlibs
# above drops that along with libc, so it's pulled back in explicitly
# here, by static archive path rather than -lgcc (there's no linker
# search path set up to resolve -l flags, since we invoke ld directly,
# not gcc-as-linker-driver). Just the compiler support routines
# (soft-{div,mul} on wide integers, stack-protector helpers if ever
# needed) -- no libc, no runtime/exception-handling machinery.
LIBGCC="$(gcc -m64 -print-libgcc-file-name)"

MUSL_DIR="$BUILD_DIR/musl-1.2.6"
MUSL_INC="$MUSL_DIR/sysroot/usr/local/musl/include"
MUSL_LIB="$MUSL_DIR/lib/libc.a"

LWIP_DIR="$BUILD_DIR/lwip-2.2.0"
LWIP_INC="$LWIP_DIR/src/include"
LWIP_PORT="port/lwip_port"

MBEDTLS_DIR="$BUILD_DIR/mbedtls-3.6.6"
MBEDTLS_INC="$MBEDTLS_DIR/include"
MBEDTLS_PORT="port/mbedtls_port"

PORT="port"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ $# -eq 0 ]; then
	echo "usage: $0 yourapp.c [otherfile.c ...]" >&2
	exit 1
fi

if [ ! -f "$MUSL_LIB" ]; then
	echo "error: $MUSL_LIB is missing -- run ./setup.sh first." >&2
	exit 1
fi

if ! compgen -G "$BUILD_DIR/lwip_*.o" >/dev/null; then
	echo "error: lwIP objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
	exit 1
fi

if ! compgen -G "$BUILD_DIR/mbedtls_*.o" >/dev/null; then
	echo "error: mbedTLS objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
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


# mbedTLS headers pull in musl's the same way, plus mbedTLS's own
# include/ tree; -DMBEDTLS_CONFIG_FILE points every mbedTLS source file
# (library/ and our own) at our port/mbedtls_port/baremetal_mbedtls_config.h
# instead of mbedTLS's own default -- see that file for what's changed
# and why (short version: no clock, hardware RNG instead of
# /dev/urandom, TLS 1.2 legacy-crypto-API only). See port/tls_shim.c.
#
# Deliberately NOT named mbedtls_config.h: mbedTLS's own build_info.h
# does "#include MBEDTLS_CONFIG_FILE" as a computed *quote-form*
# include, and the compiler's search order for a quote-form include
# checks the directory of the file containing the #include (i.e.
# mbedTLS's own include/mbedtls/, where build_info.h lives) before it
# ever gets to any -I path. mbedTLS ships its own mbedtls_config.h
# right there, so naming this file identically means that stock
# desktop-oriented default (platform entropy via /dev/urandom, PSA
# crypto, file I/O, no BAREMETAL_ENTROPY_HARDWARE_ALT hook, etc.) wins
# silently every time, no matter what -I flags are passed -- gcc
# doesn't warn, it just quietly compiles every mbedTLS source (and
# this port's own tls_shim.c/entropy_hardware_poll.c) against the
# wrong config. A unique filename is the only fix; -iquote/-I ordering
# can't win against the "current file's own directory" search step.
MBEDTLS_CFLAGS="$CFLAGS -I $MBEDTLS_INC -I $MBEDTLS_PORT -DMBEDTLS_CONFIG_FILE=\"baremetal_mbedtls_config.h\""

echo "Building..."

gcc $CFLAGS -o "$BUILD_DIR/crt0.o" "$PORT/crt0.c"
gcc $CFLAGS -o "$BUILD_DIR/posix_shim.o" "$PORT/posix_shim.c"
gcc $CFLAGS -o "$BUILD_DIR/bmfs.o" "$PORT/bmfs.c"
NET_GLUE_CFLAGS="$LWIP_CFLAGS"
if [ "$BAREMETAL_DEBUG" = "TRUE" ]; then
	NET_GLUE_CFLAGS="$NET_GLUE_CFLAGS -DBAREMETAL_DEBUG=1"
fi
gcc $NET_GLUE_CFLAGS -o "$BUILD_DIR/net_glue.o" "$PORT/net_glue.c"
gcc $LWIP_CFLAGS -o "$BUILD_DIR/net_shim.o" "$PORT/net_shim.c"
gcc $LWIP_CFLAGS -o "$BUILD_DIR/dns_shim.o" "$PORT/dns_shim.c"
gcc $MBEDTLS_CFLAGS -o "$BUILD_DIR/tls_shim.o" "$PORT/tls_shim.c"
gcc $MBEDTLS_CFLAGS -o "$BUILD_DIR/entropy_hardware_poll.o" "$MBEDTLS_PORT/entropy_hardware_poll.c"
gcc $CFLAGS -o "$BUILD_DIR/libBareMetal.o" "$PORT/libBareMetal.c"

APP_OBJS=""
for src in "${APP_SRCS[@]}"; do
	obj="$BUILD_DIR/$(basename "$src" .c).o"
	gcc $CFLAGS -o "$obj" "$src"
	APP_OBJS="$APP_OBJS $obj"
done

# lwIP and mbedTLS object files are built once by setup.sh (checked for
# above), not per app build -- just pick up what's already there.
LWIP_OBJS=""
for obj in "$BUILD_DIR"/lwip_*.o; do
	LWIP_OBJS="$LWIP_OBJS $obj"
done

MBEDTLS_OBJS=""
for obj in "$BUILD_DIR"/mbedtls_*.o; do
	MBEDTLS_OBJS="$MBEDTLS_OBJS $obj"
done

echo "Linking..."
ld -T "$PORT/c.ld" -o "$APP_NAME" "$BUILD_DIR/crt0.o" "$BUILD_DIR/posix_shim.o" \
	"$BUILD_DIR/bmfs.o" "$BUILD_DIR/net_glue.o" "$BUILD_DIR/net_shim.o" \
	"$BUILD_DIR/dns_shim.o" "$BUILD_DIR/tls_shim.o" "$BUILD_DIR/entropy_hardware_poll.o" \
	"$BUILD_DIR/libBareMetal.o" $APP_OBJS $LWIP_OBJS $MBEDTLS_OBJS "$MUSL_LIB" "$LIBGCC"

echo "Built $APP_NAME"
