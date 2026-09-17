#!/bin/bash
set -e

# Pinned: build-zig-app.sh's own -mcmodel=large/-mno-red-zone/-lc flags
# and ZIG.md's account of what does/doesn't route through libc were
# verified against this exact release. Bump deliberately, the same way
# get-musl.sh pins VERSION="1.2.6". Vendored as the official prebuilt
# tarball straight into build/ (like lwIP/mbedTLS/curl/SQLite/lwext4/
# CPython/Lua) rather than installed system-wide -- unlike Rust, Zig's
# compiler is a single self-contained tarball with no toolchain
# manager needed.
VERSION="0.15.2"
URL="https://ziglang.org/download/${VERSION}/zig-x86_64-linux-${VERSION}.tar.xz"
TARBALL="zig-x86_64-linux-${VERSION}.tar.xz"
ZIG_DIR="zig-x86_64-linux-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$ZIG_DIR" ]; then
	echo "$ZIG_DIR already exists - skipping download. Remove it first if you want to re-fetch."
	exit 0
fi

if [ -f "$TARBALL" ]; then
	echo "- $TARBALL already exists - skipping download."
else
	echo "- Downloading ${URL}"
	curl -s -L -o "${TARBALL}" "${URL}"
fi

echo "- Extracting ${TARBALL}"
tar -xJf "${TARBALL}"
