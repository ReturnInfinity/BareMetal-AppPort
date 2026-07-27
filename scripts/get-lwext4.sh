#!/bin/bash
set -e

# Pinned: port/lwext4_port/ (generated/ext4_config.h, blockdev_baremetal.c)
# and port/ext4_shim.c are written against this exact commit's API/layout.
# Bump deliberately, not automatically.
#
# lwext4's last tagged release is v1.0.0 (2016), well over a hundred
# commits and six years of fixes behind its master branch -- so this
# pins a specific master commit (2022-09-22) by tarball instead of a
# release tag.
COMMIT="58bcf89a121b72d4fb66334f1693d3b30e4cb9c5"
VERSION="58bcf89"
URL="https://github.com/gkostka/lwext4/archive/${COMMIT}.tar.gz"
TARBALL="lwext4-${VERSION}.tar.gz"
LWEXT4_DIR="lwext4-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$LWEXT4_DIR" ]; then
	echo "$LWEXT4_DIR already exists -- skipping download. Remove it first if you want to re-fetch."
	exit 0
fi

if [ -f "$TARBALL" ]; then
	echo "$TARBALL already exists -- skipping download."
else
	echo "Downloading ${URL}..."
	curl -L -o "${TARBALL}" "${URL}"
fi

echo "Extracting ${TARBALL}..."
tar -xzf "${TARBALL}"
mv "lwext4-${COMMIT}" "$LWEXT4_DIR"

echo "Done. Source extracted to: ${LWEXT4_DIR}/"
