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

CURL_DIR="$BUILD_DIR/curl-8.21.0"
CURL_INC="$CURL_DIR/include"
CURL_LIB="$CURL_DIR/lib"
CURL_PORT="port/curl_port"

SQLITE_DIR="$BUILD_DIR/sqlite-3.46.1"
SQLITE_INC="$SQLITE_DIR"
SQLITE_PORT="port/sqlite_port"

SODIUM_DIR="$BUILD_DIR/libsodium-1.0.22/src/libsodium"
SODIUM_INC="$SODIUM_DIR/include"
SODIUM_PORT="port/libsodium_port"

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

if ! compgen -G "$BUILD_DIR/curl_*.o" >/dev/null; then
	echo "error: curl objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
	exit 1
fi

if [ ! -f "$BUILD_DIR/sqlite_sqlite3.o" ]; then
	echo "error: $BUILD_DIR/sqlite_sqlite3.o is missing -- run ./setup.sh first." >&2
	exit 1
fi

if ! compgen -G "$BUILD_DIR/sodium_*.o" >/dev/null; then
	echo "error: libsodium objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
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
#
# -ffunction-sections/-fdata-sections pair with -Wl,--gc-sections below
# (see setup.sh's matching CFLAGS comment) -- lwIP/mbedTLS/curl are
# linked into every app regardless of use, so without these an app
# using none of libcurl still paid for all of it. Applied here too
# (not just in setup.sh) since crt0.o/posix_shim.o/the app's own
# sources/etc. are compiled fresh by *this* script every build.
CFLAGS="-c -m64 -nostdlib -nostartfiles -nodefaultlibs -ffreestanding -fno-pic -fno-pie -mcmodel=large -falign-functions=16 -fomit-frame-pointer -mno-red-zone -fno-builtin -fno-stack-protector -ffunction-sections -fdata-sections -nostdinc -isystem $MUSL_INC"

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

# See setup.sh's matching CURL_CFLAGS comment for what each flag here
# is for -- curl's own lib/*.c objects are prebuilt by setup.sh (like
# lwIP's/mbedTLS's), this is only needed again here for -DCURL_STATICLIB
# and -I $CURL_INC, which any *app* using libcurl (e.g. curltest.c)
# also needs for its own #include <curl/curl.h>.
#
# -I $SQLITE_INC is the equivalent for sqlite3.h -- SQLITE_INC points
# straight at the amalgamation directory (sqlite3.h sits next to
# sqlite3.c there, no separate include/ subtree the way curl/mbedTLS
# have). No -D flags needed here: SQLITE_CUSTOM_INCLUDE/SQLITE_OS_OTHER
# etc. (see setup.sh's SQLITE_CFLAGS comment) only matter to sqlite3.c
# itself, not to the plain public sqlite3.h an app #include's.
#
# -I $SODIUM_INC is the equivalent for <sodium.h> (it in turn quote-
# includes "sodium/version.h" etc, resolved relative to its own
# directory -- see setup.sh's SODIUM_CFLAGS comment). -DSODIUM_STATIC
# matches setup.sh's SODIUM_CFLAGS -- an app's own #include <sodium.h>
# needs the same define export.h does its SODIUM_EXPORT expansion on.
APP_CFLAGS="$CFLAGS -DCURL_STATICLIB -I $CURL_INC -I $SQLITE_INC -DSODIUM_STATIC -I $SODIUM_INC"

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
gcc $CFLAGS -I "$SQLITE_INC" -o "$BUILD_DIR/sqlite_vfs.o" "$SQLITE_PORT/sqlite_vfs.c"
gcc $CFLAGS -I "$SODIUM_INC" -I "$SODIUM_INC/sodium" -o "$BUILD_DIR/randombytes_baremetal.o" "$SODIUM_PORT/randombytes_baremetal.c"
gcc $CFLAGS -o "$BUILD_DIR/libBareMetal.o" "$PORT/libBareMetal.c"

APP_OBJS=""
for src in "${APP_SRCS[@]}"; do
	obj="$BUILD_DIR/$(basename "$src" .c).o"
	gcc $APP_CFLAGS -o "$obj" "$src"
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

CURL_OBJS=""
for obj in "$BUILD_DIR"/curl_*.o; do
	CURL_OBJS="$CURL_OBJS $obj"
done

# Unlike LWIP_OBJS/MBEDTLS_OBJS/CURL_OBJS above, this isn't a glob:
# sqlite3.c is one file, so setup.sh only ever produces the one
# sqlite_sqlite3.o here -- a "sqlite_*.o" glob would also pick up this
# script's own sqlite_vfs.o (built just above, from port/sqlite_port/,
# already named separately into the ld command below), double-linking
# it into a "multiple definition" error.
SQLITE_OBJS="$BUILD_DIR/sqlite_sqlite3.o"

# Like LWIP_OBJS/MBEDTLS_OBJS/CURL_OBJS above: the libsodium_*.o files are
# built once by setup.sh, just picked up here.
SODIUM_OBJS=""
for obj in "$BUILD_DIR"/sodium_*.o; do
	SODIUM_OBJS="$SODIUM_OBJS $obj"
done

echo "Linking..."

# Two stages, not a direct-to-binary `ld -T c.ld` link: c.ld's
# OUTPUT_FORMAT(binary) turns out to silently defeat --gc-sections --
# confirmed by comparing this same link with -oformat elf64-x86-64
# swapped in: identical inputs, but the binary-output link keeps every
# section (lwIP/mbedTLS/curl included, whether the app calls into them
# or not) while the ELF-output link correctly drops what's unreachable
# from _start. Rather than lose --gc-sections (see its CFLAGS comment
# above for why it matters -- lwIP/mbedTLS/curl are linked into every
# app regardless of use), link normally to an ELF intermediate first
# (where --gc-sections works as documented), then flatten *that*
# (already fully collected) to the raw binary c.ld's OUTPUT_FORMAT line
# would otherwise have produced directly. `objcopy -O binary` only
# copies allocated (SHF_ALLOC) sections in address order and leaves
# .bss as file-less NOBITS either way, so this changes nothing about
# the resulting layout -- __bss_start/__bss_stop/__image_base and
# everything crt0.c/posix_shim.c compute from them are unaffected.
# --no-warn-rwx-segments: c.ld deliberately maps .text/.rodata/.data/
# .bss into one combined region with no permission split (ring-0, no
# paging/protection to enforce one anyway -- see posix_shim.c's heap
# comment) -- expected here, not a mistake ld should flag.
ld --gc-sections --no-warn-rwx-segments --oformat elf64-x86-64 -T "$PORT/c.ld" -o "$BUILD_DIR/$APP_NAME.elf" "$BUILD_DIR/crt0.o" "$BUILD_DIR/posix_shim.o" \
	"$BUILD_DIR/bmfs.o" "$BUILD_DIR/net_glue.o" "$BUILD_DIR/net_shim.o" \
	"$BUILD_DIR/dns_shim.o" "$BUILD_DIR/tls_shim.o" "$BUILD_DIR/entropy_hardware_poll.o" \
	"$BUILD_DIR/sqlite_vfs.o" "$BUILD_DIR/randombytes_baremetal.o" \
	"$BUILD_DIR/libBareMetal.o" $APP_OBJS $LWIP_OBJS $MBEDTLS_OBJS $CURL_OBJS $SQLITE_OBJS $SODIUM_OBJS "$MUSL_LIB" "$LIBGCC"
objcopy -O binary "$BUILD_DIR/$APP_NAME.elf" "$APP_NAME"

echo "Built $APP_NAME"
