#!/bin/bash
set -e

# Pinned: this port's CFLAGS (see setup.sh's QUICKJS_CFLAGS) are written
# against this exact release's quickjs-c-atomics.h/cutils.h behavior.
# Bump deliberately, not automatically. quickjs-ng (the actively
# maintained fork, not the original bellard/quickjs) is vendored
# unmodified -- a plain GitHub source tarball, not a git checkout; all
# port-side work lives in QUICKJS_CFLAGS's defines instead of patches to
# quickjs.c itself.
VERSION="0.16.2"
URL="https://github.com/quickjs-ng/quickjs/archive/refs/tags/v${VERSION}.tar.gz"
TARBALL="quickjs-ng-${VERSION}.tar.gz"
QUICKJS_DIR="quickjs-ng-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$QUICKJS_DIR" ]; then
	echo "$QUICKJS_DIR already exists - skipping download. Remove it first if you want to re-fetch."
	exit 0
fi

if [ -f "$TARBALL" ]; then
	echo "- $TARBALL already exists - skipping download."
else
	echo "- Downloading ${URL}"
	curl -sL -o "${TARBALL}" "${URL}"
fi

echo "- Extracting ${TARBALL}"
tar xzf "${TARBALL}"
mv "quickjs-${VERSION}" "$QUICKJS_DIR"

# echo "Done. Source extracted to: ${QUICKJS_DIR}/"
