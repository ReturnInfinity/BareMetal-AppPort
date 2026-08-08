#!/bin/bash
set -e

# Pinned (not "latest"): port/musl_port/musl-1.2.6-baremetal.patch and
# port/musl_port/musl-1.2.6-config.mak are written against this exact
# release's file layout. Bump deliberately, not automatically.
VERSION="1.2.6"
URL="https://musl.libc.org/releases/musl-${VERSION}.tar.gz"
TARBALL="musl-${VERSION}.tar.gz"
MUSL_DIR="musl-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
PATCH_DIR="$DIST_DIR/port/musl_port"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$MUSL_DIR" ]; then
	echo "$MUSL_DIR already exists - skipping download. Remove it first if you want to re-fetch."
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

echo "- Applying BareMetal port patch"
patch --quiet -p1 -d "$MUSL_DIR" < "$PATCH_DIR/musl-1.2.6-baremetal.patch"

echo "- Installing pre-generated config.mak"
cp "$PATCH_DIR/musl-1.2.6-config.mak" "$MUSL_DIR/config.mak"

# echo "Done. Source fetched and patched to: ${MUSL_DIR}/"
