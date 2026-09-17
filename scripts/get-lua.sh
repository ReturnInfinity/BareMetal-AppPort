#!/bin/bash
set -e

# Pinned: port/lua_port/ (lua.c) and setup.sh's own
# LUA_SRCS list are written against this exact release's src/ layout.
# Bump deliberately, not automatically. Lua is vendored unmodified (the
# official source tarball) -- all port-side work lives in
# port/lua_port/ instead, the same choice already made for lwIP/
# Mbed TLS/curl/SQLite/lwext4/libsodium/CPython.
#
# Called from setup.sh alongside the other get-*.sh scripts; also
# runnable standalone the same way they are.
VERSION="5.4.7"
URL="https://www.lua.org/ftp/lua-${VERSION}.tar.gz"
TARBALL="lua-${VERSION}.tar.gz"
LUA_DIR="lua-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$LUA_DIR" ]; then
	echo "$LUA_DIR already exists - skipping download. Remove it first if you want to re-fetch."
	exit 0
fi

if [ -f "$TARBALL" ]; then
	echo "- $TARBALL already exists - skipping download."
else
	echo "- Downloading ${URL}"
	curl -s -L -o "${TARBALL}" "${URL}"
fi

echo "- Extracting ${TARBALL}"
tar -xzf "${TARBALL}"

# echo "Done. Source extracted to: ${LUA_DIR}/"
