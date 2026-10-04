#!/bin/bash
set -e

# Pinned: port/lua_port/ (lua.c, install-main.sh) is written against
# this exact release's public API (lua.h/lualib.h/lauxlib.h). Bump
# deliberately, not automatically. Lua is vendored unmodified; all
# port-side work lives in port/lua_port/ instead of patches to Lua
# itself (see LUA.md).
VERSION="5.5.1"
SHA256="1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce"
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

# lua.org publishes the checksum alongside each tarball (www.lua.org/ftp/);
# checked here since it's the one fetch in setup.sh served over a
# plain mirror rather than a GitHub release/archive URL.
if ! echo "${SHA256}  ${TARBALL}" | sha256sum -c --quiet -; then
	echo "error: ${TARBALL} checksum mismatch -- delete it and re-run" >&2
	exit 1
fi

echo "- Extracting ${TARBALL}"
tar -xzf "${TARBALL}"

# echo "Done. Source extracted to: ${LUA_DIR}/"
