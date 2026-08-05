#!/bin/bash
set -e

# Pinned: port/curl_port/ (curl_config.h) and this port's build-app.sh/
# setup.sh curated file list are written against this exact release's
# API/file layout. Bump deliberately, not automatically. curl is
# vendored unmodified; all port-side work lives in port/curl_port/
# instead of patches to curl itself.
VERSION="8.21.0"
URL="https://curl.se/download/curl-${VERSION}.tar.xz"
TARBALL="curl-${VERSION}.tar.xz"
CURL_DIR="curl-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$CURL_DIR" ]; then
	echo "$CURL_DIR already exists - skipping download. Remove it first if you want to re-fetch."
	exit 0
fi

if [ -f "$TARBALL" ]; then
	echo "$TARBALL already exists - skipping download."
else
	echo "Downloading ${URL}"
	curl -s -L -o "${TARBALL}" "${URL}"
fi

echo "Extracting ${TARBALL}"
tar -xJf "${TARBALL}"

# echo "Done. Source extracted to: ${CURL_DIR}/"
