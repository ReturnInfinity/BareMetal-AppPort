#!/bin/bash
set -e

# Pinned: this port's setup.sh (LEXBOR_CFLAGS) builds a hand-picked set
# of module directories against this exact release's layout (source/
# lexbor/<module>/*.c, source/lexbor/ports/posix/lexbor/<module>/*.c).
# Bump deliberately, not automatically. lexbor is vendored unmodified --
# a plain GitHub source tarball, not a git checkout; all port-side work
# lives in setup.sh/build-app.sh instead of patches to lexbor itself.
VERSION="3.0.0"
URL="https://github.com/lexbor/lexbor/archive/refs/tags/v${VERSION}.tar.gz"
TARBALL="lexbor-${VERSION}.tar.gz"
LEXBOR_DIR="lexbor-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$LEXBOR_DIR" ]; then
	echo "$LEXBOR_DIR already exists - skipping download. Remove it first if you want to re-fetch."
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

# echo "Done. Source extracted to: ${LEXBOR_DIR}/"
