#!/bin/bash
set -e

# EXPERIMENTAL -- see ../PYTHON_PORT.md. Not wired into setup.sh yet:
# unlike get-curl.sh/get-sqlite.sh/etc., there is no working port/
# python_port/ build this vendors into an app yet, just the plan in
# PYTHON_PORT.md and an early pyconfig_baremetal.h draft. This script
# only fetches and unpacks the source so that plan can be developed
# against real CPython files instead of from memory.
#
# Pinned, same reasoning as every other get-*.sh here: PYTHON_PORT.md
# and port/python_port/ (once it exists for real) will be written
# against this exact release's Modules/Setup.bootstrap.in layout,
# pyconfig.h.in macro list, and Python/thread_pthread.h contents. Bump
# deliberately, not automatically. CPython is vendored unmodified --
# no patch, unlike musl -- all port-side work is meant to live in
# port/python_port/ instead, the same choice already made for lwIP/
# Mbed TLS/curl/SQLite/lwext4/libsodium.
VERSION="3.12.8"
URL="https://www.python.org/ftp/python/${VERSION}/Python-${VERSION}.tar.xz"
TARBALL="Python-${VERSION}.tar.xz"
PYTHON_DIR="Python-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$PYTHON_DIR" ]; then
	echo "$PYTHON_DIR already exists - skipping download. Remove it first if you want to re-fetch."
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

# echo "Done. Source extracted to: ${PYTHON_DIR}/"
