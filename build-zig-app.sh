#!/usr/bin/env bash
set -e

# Build a BareMetal app written in Zig against the musl/lwIP/mbedTLS/
# lwext4 port in this directory -- the Zig counterpart to build-app.sh
# and build-rust-app.sh. See ZIG.md for the full story of why this
# works and what doesn't.
#
# Usage: ./build-zig-app.sh yourapp.zig
#
# yourapp.zig must export a C-ABI `main` itself:
#
#   export fn main(argc: c_int, argv: [*c][*c]u8, envp: [*c][*c]u8) callconv(.c) c_int
#
# This port supplies its own crt0.c/_start (the same one build-app.sh/
# build-rust-app.sh use), not Zig's own start.zig -- there is no Zig-
# idiomatic `pub fn main() !void` entry point here, the same way a C
# app on this port writes `int main(int argc, char **argv)` directly
# rather than relying on some other runtime's startup. See
# examples/zig/hello/hello.zig.

BUILD_DIR="build"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ $# -ne 1 ]; then
	echo "usage: $0 yourapp.zig" >&2
	exit 1
fi

APP_SRC="$1"
if [ ! -f "$APP_SRC" ]; then
	echo "error: $APP_SRC not found" >&2
	exit 1
fi
case "$APP_SRC" in
*.zig) ;;
*)
	echo "error: $APP_SRC is not a .zig file" >&2
	exit 1
	;;
esac
APP_NAME="$(basename "$APP_SRC" .zig).app"

ZIG_VERSION="0.15.2"
ZIG="$BUILD_DIR/zig-x86_64-linux-$ZIG_VERSION/zig"
if [ ! -x "$ZIG" ]; then
	echo "error: $ZIG not found -- run ./setup.sh first (see scripts/get-zig.sh)." >&2
	exit 1
fi

MUSL_DIR="$BUILD_DIR/musl-1.2.6"
MUSL_LIB="$MUSL_DIR/lib/libc.a"

LWIP_DIR="$BUILD_DIR/lwip-2.2.0"
LWIP_PORT="port/lwip_port"
MBEDTLS_DIR="$BUILD_DIR/mbedtls-3.6.6"
MBEDTLS_PORT="port/mbedtls_port"
LWEXT4_DIR="$BUILD_DIR/lwext4-58bcf89"
LWEXT4_INC="$LWEXT4_DIR/include"
LWEXT4_PORT="port/lwext4_port"
PORT="port"
ZIG_PORT="port/zig_port"

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
if [ ! -f "$BUILD_DIR/cacert_data.o" ]; then
	echo "error: $BUILD_DIR/cacert_data.o is missing -- run ./setup.sh first." >&2
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
if ! compgen -G "$BUILD_DIR/lwext4_*.o" >/dev/null; then
	echo "error: lwext4 objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
	exit 1
fi
if ! compgen -G "$BUILD_DIR/python_*.o" >/dev/null; then
	echo "error: Python objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
	exit 1
fi

mkdir -p "$BUILD_DIR"

# Same freestanding ABI build-app.sh's own CFLAGS comment explains
# (fixed high-canonical load address, no PIE, no ELF loader): static,
# non-PIC, large code model, no red zone (interrupts here don't switch
# stacks). -fno-stack-protector is this port's own choice, not forced by
# the ABI -- matches every other language here (build-app.sh's own CFLAGS),
# since std's stack-protector support-runtime symbols don't exist here.
#
# -O ReleaseSmall (never plain Debug mode) is required, not just a size
# preference: Zig's Debug-mode codegen for std.Io.Writer's internals (the
# machinery std.debug.print goes through) emits some anonymous constant/
# vtable references as absolute 32-bit (R_X86_64_32) relocations that
# can't reach this port's high-canonical (0xFFFF8000...) load address --
# "relocation truncated to fit" at link time. ReleaseSmall/ReleaseFast
# don't hit this (confirmed by a real link+boot of std.debug.print calls,
# both plain and formatted, under ReleaseSmall) -- an LLVM/Zig code-model
# limitation, not something this port's build can work around.
#
# No -fsingle-threaded: real threads (std.Thread/Mutex/Futex) work now --
# see ZIG.md for what made that true (a companion kernel-side fix in
# BareMetal-Firecracker, not anything in this build script).
ZIGFLAGS="-target x86_64-linux-musl -mcmodel=large -mno-red-zone -fno-stack-protector -O ReleaseSmall -lc"

echo "Building Zig app ($APP_SRC)..."

# zig build-obj already produces a single relocatable object with the
# whole app + whatever of std it pulled in, ready to feed straight
# into the final ld -T c.ld link below -- unlike Rust's cargo build (a
# full linked binary needing build-rust-app.sh's own -r partial-link
# stage first), so there's no separate stage here: this one compile
# command stands in directly for build-app.sh's $APP_OBJS.
#
# -target x86_64-linux-musl + -lc routes std's syscalls (file I/O,
# threading/futex, debug.print's internals, ...) through real calls to
# musl symbols this port's patched musl already intercepts
# (__bmos_syscall(), see port/musl_port/'s patch and RUST.md's "why this
# works" section for the same reasoning as Rust's libc crate) -- see
# ZIG.md for the full account, including the BareMetal-Firecracker
# kernel-side fix this now depends on for real std.debug.print/
# std.Thread support. The "bm" module (port/zig_port/bm.zig) is available
# to every app (`@import("bm")`) as an optional, lighter-weight
# alternative to std.debug.print -- not required for correctness anymore,
# just smaller and lock-free.
APP_OBJ="$BUILD_DIR/$(basename "$APP_SRC" .zig).o"
"$ZIG" build-obj \
	$ZIGFLAGS \
	--dep bm \
	-Mroot="$APP_SRC" \
	$ZIGFLAGS \
	-Mbm="$ZIG_PORT/bm.zig" \
	-femit-bin="$APP_OBJ"

# The same per-app shim compile + final `ld -T c.ld` link build-app.sh
# does (see that script's own comments for what each object/flag is
# for) -- $APP_OBJ stands in for build-app.sh's $APP_OBJS. No unwind
# stub is needed here the way Rust's port needs unwind_stub.o: Zig
# apps never emit a reference to any `_Unwind_*` symbol in the first
# place (panic = abort-equivalent by construction, not by a stub
# catching what would otherwise be a real unwind).
CFLAGS="-c -m64 -O2 -nostdlib -nostartfiles -nodefaultlibs -ffreestanding -fno-pic -fno-pie -mcmodel=large -falign-functions=16 -fomit-frame-pointer -mno-red-zone -fno-builtin -fno-stack-protector -ffunction-sections -fdata-sections -nostdinc -isystem $MUSL_DIR/sysroot/usr/local/musl/include"
LWIP_CFLAGS="$CFLAGS -I $LWIP_DIR/src/include -I $LWIP_PORT"
MBEDTLS_CFLAGS="$CFLAGS -I $MBEDTLS_DIR/include -I $MBEDTLS_PORT -DMBEDTLS_CONFIG_FILE=\"baremetal_mbedtls_config.h\""
LWEXT4_CFLAGS="$CFLAGS -I $LWEXT4_INC -I $LWEXT4_PORT -I $PORT -DCONFIG_USE_DEFAULT_CFG=0"

echo "Building port shims..."
gcc $CFLAGS -o "$BUILD_DIR/crt0.o" "$PORT/crt0.c"
gcc $CFLAGS -o "$BUILD_DIR/posix_shim.o" "$PORT/posix_shim.c"
gcc $CFLAGS -o "$BUILD_DIR/thread_shim.o" "$PORT/thread_shim.c"
gcc $LWEXT4_CFLAGS -o "$BUILD_DIR/ext4_shim.o" "$PORT/ext4_shim.c"
gcc $LWEXT4_CFLAGS -o "$BUILD_DIR/blockdev_baremetal.o" "$LWEXT4_PORT/blockdev_baremetal.c"
gcc $LWIP_CFLAGS -o "$BUILD_DIR/net_glue.o" "$PORT/net_glue.c"
gcc $LWIP_CFLAGS -o "$BUILD_DIR/net_shim.o" "$PORT/net_shim.c"
gcc $LWIP_CFLAGS -o "$BUILD_DIR/dns_shim.o" "$PORT/dns_shim.c"
gcc $MBEDTLS_CFLAGS -o "$BUILD_DIR/tls_shim.o" "$PORT/tls_shim.c"
gcc $MBEDTLS_CFLAGS -o "$BUILD_DIR/entropy_hardware_poll.o" "$MBEDTLS_PORT/entropy_hardware_poll.c"
gcc $CFLAGS -I "$BUILD_DIR/sqlite-3.46.1" -o "$BUILD_DIR/sqlite_vfs.o" "port/sqlite_port/sqlite_vfs.c"
gcc $CFLAGS -I "$BUILD_DIR/libsodium-1.0.22/src/libsodium/include" -I "$BUILD_DIR/libsodium-1.0.22/src/libsodium/include/sodium" -o "$BUILD_DIR/randombytes_baremetal.o" "port/libsodium_port/randombytes_baremetal.c"
gcc $CFLAGS -o "$BUILD_DIR/dlfcn_shim.o" "$PORT/dlfcn_shim.c"
gcc $CFLAGS -o "$BUILD_DIR/libBareMetal.o" "$PORT/libBareMetal.c"

LIBGCC="$(gcc -m64 -print-libgcc-file-name)"
LWIP_OBJS=$(ls "$BUILD_DIR"/lwip_*.o)
MBEDTLS_OBJS=$(ls "$BUILD_DIR"/mbedtls_*.o)
CURL_OBJS=$(ls "$BUILD_DIR"/curl_*.o)
SQLITE_OBJS="$BUILD_DIR/sqlite_sqlite3.o"
SODIUM_OBJS=$(ls "$BUILD_DIR"/sodium_*.o)
LWEXT4_OBJS=$(ls "$BUILD_DIR"/lwext4_*.o)
PYTHON_OBJS=$(ls "$BUILD_DIR"/python_*.o)

echo "Linking..."
ld --gc-sections --no-warn-rwx-segments --oformat elf64-x86-64 -T "$PORT/c.ld" -o "$BUILD_DIR/$APP_NAME.elf" "$BUILD_DIR/crt0.o" "$BUILD_DIR/posix_shim.o" "$BUILD_DIR/thread_shim.o" \
	"$BUILD_DIR/ext4_shim.o" "$BUILD_DIR/blockdev_baremetal.o" "$BUILD_DIR/net_glue.o" "$BUILD_DIR/net_shim.o" \
	"$BUILD_DIR/dns_shim.o" "$BUILD_DIR/tls_shim.o" "$BUILD_DIR/entropy_hardware_poll.o" "$BUILD_DIR/cacert_data.o" \
	"$BUILD_DIR/sqlite_vfs.o" "$BUILD_DIR/randombytes_baremetal.o" "$BUILD_DIR/dlfcn_shim.o" \
	"$BUILD_DIR/libBareMetal.o" "$APP_OBJ" $LWIP_OBJS $MBEDTLS_OBJS $CURL_OBJS $SQLITE_OBJS $SODIUM_OBJS $LWEXT4_OBJS $PYTHON_OBJS "$MUSL_LIB" "$LIBGCC"
objcopy -O binary "$BUILD_DIR/$APP_NAME.elf" "$APP_NAME"

echo "Built $APP_NAME"
