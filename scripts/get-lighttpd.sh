#!/bin/bash
set -e

# Pinned: port/lighttpd_port/ (config.h, plugin-static.h, lighttpd.c)
# and setup.sh's lighttpd source list are written against this exact
# release. Bump deliberately, not automatically. lighttpd is vendored
# unmodified; all port-side work lives in port/lighttpd_port/ (see
# LIGHTTPD.md).
VERSION="1.4.85"
SHA256="18de51b393bac4a6827879e1a7ff377c169e414bae92cd245091d80fc2601d13"
URL="https://download.lighttpd.net/lighttpd/releases-1.4.x/lighttpd-${VERSION}.tar.xz"
TARBALL="lighttpd-${VERSION}.tar.xz"
LIGHTTPD_DIR="lighttpd-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$LIGHTTPD_DIR" ]; then
	echo "$LIGHTTPD_DIR already exists - skipping download. Remove it first if you want to re-fetch."
	exit 0
fi

if [ -f "$TARBALL" ]; then
	echo "- $TARBALL already exists - skipping download."
else
	echo "- Downloading ${URL}"
	curl -s -L -o "${TARBALL}" "${URL}"
fi

# Checksum from the release's published lighttpd-${VERSION}.sha256sum.
if ! echo "${SHA256}  ${TARBALL}" | sha256sum -c --quiet -; then
	echo "error: ${TARBALL} checksum mismatch -- delete it and re-run" >&2
	exit 1
fi

echo "- Extracting ${TARBALL}"
tar -xJf "${TARBALL}"
