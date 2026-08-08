#!/bin/bash
set -e

# Pinned: port/sqlite_port/ (sqlite_baremetal_config.h, sqlite_vfs.c) is
# written against this exact release's public API/amalgamation layout.
# Bump deliberately, not automatically. SQLite is vendored unmodified
# (the amalgamation -- sqlite3.c/sqlite3.h -- not a git checkout); all
# port-side work lives in port/sqlite_port/ instead of patches to
# sqlite3.c itself.
VERSION="3.46.1"
AMALGAMATION="3460100"
YEAR="2024"
URL="https://sqlite.org/${YEAR}/sqlite-amalgamation-${AMALGAMATION}.zip"
ZIPFILE="sqlite-amalgamation-${AMALGAMATION}.zip"
SQLITE_DIR="sqlite-${VERSION}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$DIST_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

if [ -d "$SQLITE_DIR" ]; then
	echo "$SQLITE_DIR already exists - skipping download. Remove it first if you want to re-fetch."
	exit 0
fi

if [ -f "$ZIPFILE" ]; then
	echo "- $ZIPFILE already exists - skipping download."
else
	echo "- Downloading ${URL}"
	curl -s -L -o "${ZIPFILE}" "${URL}"
fi

echo "- Extracting ${ZIPFILE}"
unzip -q -o "${ZIPFILE}"
mv "sqlite-amalgamation-${AMALGAMATION}" "$SQLITE_DIR"

# echo "Done. Source extracted to: ${SQLITE_DIR}/"
