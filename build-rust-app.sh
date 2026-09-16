#!/usr/bin/env bash
set -e

# Build a BareMetal app, written in Rust with full `std`, against the
# musl/lwIP/mbedTLS/lwext4 port in this directory -- the Rust
# counterpart to build-app.sh. See RUST.md for the full story of why
# this works and what doesn't.
#
# Usage: ./build-rust-app.sh yourcrate/src/main.rs
# yourcrate/ must be a normal cargo project (yourcrate/Cargo.toml next
# to yourcrate/src/). The output is named after the crate directory,
# with a .app extension (e.g. hello-rs/ -> hello-rs.app).

BUILD_DIR="build"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# scripts/get-rust.sh's own `source "$HOME/.cargo/env"` only updates
# PATH for that script's own process -- if this is the first time
# rustup was ever installed, a shell that hasn't been restarted since
# (e.g. setup.sh followed by ./1-build.sh in the same terminal) still
# resolves `cargo` to any pre-existing apt/distro cargo in /usr/bin,
# which doesn't understand the `+nightly-...` toolchain-override
# syntax below and fails with "no such command: `+nightly-...`".
# Sourcing it here too makes this script work regardless of whether
# the invoking shell already picked up rustup's PATH change.
if [ -f "$HOME/.cargo/env" ]; then
	# shellcheck disable=SC1091
	source "$HOME/.cargo/env"
fi

if [ $# -ne 1 ]; then
	echo "usage: $0 yourcrate/src/main.rs" >&2
	exit 1
fi

MAIN_RS="$1"
if [ ! -f "$MAIN_RS" ]; then
	echo "error: $MAIN_RS not found" >&2
	exit 1
fi

CRATE_DIR="$(cd "$(dirname "$MAIN_RS")/.." && pwd)"
if [ ! -f "$CRATE_DIR/Cargo.toml" ]; then
	echo "error: $CRATE_DIR/Cargo.toml not found -- $MAIN_RS must be <crate>/src/main.rs" >&2
	exit 1
fi
APP_NAME="$(basename "$CRATE_DIR").app"

RUST_PORT="port/rust_port"
TARGET_SPEC="$SCRIPT_DIR/$RUST_PORT/x86_64-baremetal-firecracker.json"

RUST_NIGHTLY="nightly-2026-09-14"
if [ -f "$RUST_PORT/PINNED_TOOLCHAIN" ]; then
	RUST_NIGHTLY="$(cat "$RUST_PORT/PINNED_TOOLCHAIN")"
fi

if ! command -v cargo > /dev/null 2>&1; then
	echo "error: cargo not found -- run ./setup.sh first (see scripts/get-rust.sh)." >&2
	exit 1
fi

# Same "-c -m64 ... -mno-red-zone ... -nostdinc -isystem $MUSL_INC"
# freestanding CFLAGS build-app.sh uses for crt0.c/the port's own
# shims -- see that script's own CFLAGS comment for what each flag is
# for. Only what unwind_stub.c actually needs is repeated here.
MUSL_DIR="$BUILD_DIR/musl-1.2.6"
MUSL_INC="$MUSL_DIR/sysroot/usr/local/musl/include"
MUSL_LIB="$MUSL_DIR/lib/libc.a"
CFLAGS="-c -m64 -O2 -nostdlib -nostartfiles -nodefaultlibs -ffreestanding -fno-pic -fno-pie -mcmodel=large -falign-functions=16 -fomit-frame-pointer -mno-red-zone -fno-builtin -fno-stack-protector -ffunction-sections -fdata-sections -nostdinc -isystem $MUSL_INC"

LWIP_DIR="$BUILD_DIR/lwip-2.2.0"
LWIP_PORT="port/lwip_port"
MBEDTLS_DIR="$BUILD_DIR/mbedtls-3.6.6"
MBEDTLS_PORT="port/mbedtls_port"
LWEXT4_DIR="$BUILD_DIR/lwext4-58bcf89"
LWEXT4_INC="$LWEXT4_DIR/include"
LWEXT4_PORT="port/lwext4_port"
PORT="port"

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

echo "Building Rust crate ($CRATE_DIR)..."

# Stage 1: `cargo build -Z build-std` compiles the app + std/core/
# alloc/panic_abort from source against the custom target above, then
# the target's own `pre-link-args: ["-r", "-u", "main"]` (see that
# file) makes rustc's own linker invocation collapse the whole crate
# graph into ONE relocatable object instead of a final executable --
# same shape as build-app.sh compiling one .c file to one .o, just
# with cargo doing the compiling.
#
# -L $STUBLIB_DIR (empty libc.a/libgcc_s.a, generated below) stands in
# for the real musl libc.a and libgcc_s at THIS stage only: rustc's
# std target unconditionally wants to link both by name (see
# RUST.md's "why the stub libs" section for how this was found), but
# actually resolving them here -- rather than once, for real, in Stage
# 2's final link below -- produces duplicate-symbol errors against the
# port's other objects, which also pull from musl's libc.a. So this
# stage leaves every musl/libgcc symbol Rust's std needs (open, read,
# malloc, pthread_create, __udivti3, ...) unresolved and Stage 2
# resolves them all at once, the same single point of truth
# build-app.sh already uses for the C side.
STUBLIB_DIR="$BUILD_DIR/rust_stublibs"
if [ ! -f "$STUBLIB_DIR/libc.a" ]; then
	mkdir -p "$STUBLIB_DIR"
	ar rcs "$STUBLIB_DIR/libc.a"
	ar rcs "$STUBLIB_DIR/libgcc_s.a"
fi

(
	cd "$CRATE_DIR"
	RUSTFLAGS="-C target-feature=+crt-static -L $SCRIPT_DIR/$STUBLIB_DIR" \
		cargo "+$RUST_NIGHTLY" build \
			-Z build-std=std,panic_abort \
			-Z json-target-spec \
			--target "$TARGET_SPEC" \
			--release
)
RUST_OBJ="$CRATE_DIR/target/x86_64-baremetal-firecracker/release/$(basename "$CRATE_DIR")"
if [ ! -f "$RUST_OBJ" ]; then
	echo "error: expected cargo output at $RUST_OBJ -- check the crate's [package] name matches its directory name." >&2
	exit 1
fi

# See port/rust_port/unwind_stub.c's own header for why this is the
# one extra object the Rust side needs beyond what build-app.sh
# already compiles per-app below.
gcc $CFLAGS -o "$BUILD_DIR/unwind_stub.o" "$RUST_PORT/unwind_stub.c"

# Stage 2: the exact same per-app shim compile + final `ld -T c.ld`
# link build-app.sh does (see that script's own comments for what each
# object/flag is for) -- $RUST_OBJ + unwind_stub.o stand in for
# build-app.sh's $APP_OBJS.
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
	"$BUILD_DIR/libBareMetal.o" "$RUST_OBJ" "$BUILD_DIR/unwind_stub.o" $LWIP_OBJS $MBEDTLS_OBJS $CURL_OBJS $SQLITE_OBJS $SODIUM_OBJS $LWEXT4_OBJS $PYTHON_OBJS "$MUSL_LIB" "$LIBGCC"
objcopy -O binary "$BUILD_DIR/$APP_NAME.elf" "$APP_NAME"

echo "Built $APP_NAME"
