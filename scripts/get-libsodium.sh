#!/bin/bash
set -e

# Pinned: port/libsodium_port/ (randombytes_baremetal.c, version.h) and this
# port's setup.sh/build-app.sh curated file list are written against this
# exact release's API/file layout. Bump deliberately, not automatically.
# libsodium is vendored unmodified *except* for src/libsodium/include/sodium/
# version.h, which a normal build generates from version.h.in via
# ./configure -- since this port never runs libsodium's own build system
# (see setup.sh's "Building libsodium" step, which hand-picks source files
# the same way it already does for mbedTLS), that one generated header is
# written out by hand below instead, from the same VERSION/MAJOR/MINOR this
# release's configure.ac itself defines.
VERSION="1.0.22"
LIBRARY_VERSION_MAJOR="26"
LIBRARY_VERSION_MINOR="4"
SODIUM_DIR="libsodium-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
REPO_ROOT="$(dirname "$DIST_DIR")"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$SODIUM_DIR" ]; then
	echo "$SODIUM_DIR already exists - skipping fetch. Remove it first if you want to re-fetch."
	exit 0
fi

# Prefer an already-extracted checkout sitting at the repo root (this is
# how it arrived here: unzipped from the official 1.0.22-RELEASE source
# archive) over re-downloading it.
if [ -d "$REPO_ROOT/libsodium" ]; then
	echo "- Using $REPO_ROOT/libsodium"
	cp -r "$REPO_ROOT/libsodium" "$SODIUM_DIR"
elif [ -f "$REPO_ROOT/libsodium-${VERSION}-RELEASE.zip" ]; then
	echo "- Extracting $REPO_ROOT/libsodium-${VERSION}-RELEASE.zip"
	unzip -q -o "$REPO_ROOT/libsodium-${VERSION}-RELEASE.zip"
	mv "libsodium-${VERSION}-RELEASE" "$SODIUM_DIR"
else
	URL="https://github.com/jedisct1/libsodium/archive/refs/tags/${VERSION}-RELEASE.zip"
	ZIPFILE="libsodium-${VERSION}-RELEASE.zip"
	echo "- Downloading ${URL}"
	curl -s -L -o "${ZIPFILE}" "${URL}"
	echo "- Extracting ${ZIPFILE}"
	unzip -q -o "${ZIPFILE}"
	mv "libsodium-${VERSION}-RELEASE" "$SODIUM_DIR"
fi

# Hand-generate version.h from version.h.in (see this script's header
# comment) -- SODIUM_LIBRARY_MINIMAL_DEF is left blank, matching a
# non-minimal ./configure run (the default, and what this port's curated
# build gives: the full API, not --enable-minimal's trimmed-down subset).
VERSION_H_IN="$SODIUM_DIR/src/libsodium/include/sodium/version.h.in"
VERSION_H="$SODIUM_DIR/src/libsodium/include/sodium/version.h"
sed \
	-e "s/@VERSION@/${VERSION}/" \
	-e "s/@SODIUM_LIBRARY_VERSION_MAJOR@/${LIBRARY_VERSION_MAJOR}/" \
	-e "s/@SODIUM_LIBRARY_VERSION_MINOR@/${LIBRARY_VERSION_MINOR}/" \
	-e "s/@SODIUM_LIBRARY_MINIMAL_DEF@//" \
	"$VERSION_H_IN" > "$VERSION_H"

# echo "Done. Source extracted to: ${SODIUM_DIR}/"
