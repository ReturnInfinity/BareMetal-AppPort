#!/bin/bash
set -e

# Pinned: setup.sh's own build of CPython (build/python_*.o) and
# port/python_port/ (pyconfig.h, python.c, config_baremetal.c, ...) are
# written against this exact release's Modules/Setup.bootstrap.in
# layout, pyconfig.h.in macro list, and Python/thread_pthread.h
# contents -- see PYTHON.md. Bump deliberately, not automatically.
# CPython is vendored unmodified -- no patch, unlike musl -- all
# port-side work lives in port/python_port/ instead, the same choice
# already made for lwIP/Mbed TLS/curl/SQLite/lwext4/libsodium.
#
# Called from setup.sh alongside the other get-*.sh scripts; also
# runnable standalone the same way they are.
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
