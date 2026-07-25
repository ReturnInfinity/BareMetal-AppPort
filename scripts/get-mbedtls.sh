#!/bin/bash
set -e

# Pinned: port/mbedtls_port/ (mbedtls_config.h, entropy_hardware_poll.c) and
# port/tls_shim.c are written against this exact release's API. Bump
# deliberately, not automatically. mbedTLS is vendored unmodified; all
# port-side work lives in port/mbedtls_port/ and port/tls_shim.c instead of
# patches to mbedTLS itself.
VERSION="3.6.6"
URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${VERSION}/mbedtls-${VERSION}.tar.bz2"
TARBALL="mbedtls-${VERSION}.tar.bz2"
MBEDTLS_DIR="mbedtls-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$MBEDTLS_DIR" ]; then
	echo "error: $MBEDTLS_DIR already exists -- remove it first if you want to re-fetch." >&2
	exit 1
fi

echo "Downloading ${URL}..."
curl -L -o "${TARBALL}" "${URL}"

echo "Extracting ${TARBALL}..."
tar -xjf "${TARBALL}"

echo "Done. Source extracted to: ${MBEDTLS_DIR}/"
