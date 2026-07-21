#!/bin/bash
set -e

# Pinned: port/lwip_port/ (lwipopts.h, arch/cc.h, arch/sys_arch.h) and
# port/net_glue.c/net_shim.c are written against this exact release's
# file layout and config option set. Bump deliberately, not automatically.
VERSION="2.2.0"
URL="https://download.savannah.nongnu.org/releases/lwip/lwip-${VERSION}.zip"
ZIPFILE="lwip-${VERSION}.zip"
LWIP_DIR="lwip-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$LWIP_DIR" ]; then
	echo "error: $LWIP_DIR already exists -- remove it first if you want to re-fetch." >&2
	exit 1
fi

echo "Downloading ${URL}..."
curl -L -o "${ZIPFILE}" "${URL}"

echo "Extracting ${ZIPFILE}..."
unzip -q -o "${ZIPFILE}"

echo "Done. Source extracted to: ${LWIP_DIR}/"
