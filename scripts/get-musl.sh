#!/bin/bash
set -e

# Pinned (not "latest"): patches/musl-1.2.6-baremetal.patch and
# patches/musl-1.2.6-config.mak are written against this exact
# release's file layout. Bump deliberately, not automatically.
VERSION="1.2.6"
URL="https://musl.libc.org/releases/musl-${VERSION}.tar.gz"
TARBALL="musl-${VERSION}.tar.gz"
MUSL_DIR="musl-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$MUSL_DIR" ]; then
	echo "error: $MUSL_DIR already exists -- remove it first if you want to re-fetch." >&2
	exit 1
fi

echo "Downloading ${URL}..."
curl -L -o "${TARBALL}" "${URL}"

echo "Extracting ${TARBALL}..."
tar -xzf "${TARBALL}"

echo "Applying BareMetal port patch..."
patch -p1 -d "$MUSL_DIR" < "$SCRIPT_DIR/patches/musl-1.2.6-baremetal.patch"

echo "Installing pre-generated config.mak..."
cp "$SCRIPT_DIR/patches/musl-1.2.6-config.mak" "$MUSL_DIR/config.mak"

echo "Done. Source fetched and patched to: ${MUSL_DIR}/"
