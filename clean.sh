#!/bin/bash
# Remove all build artifacts (.o, .a, .app) from this directory,
# and the extracted source folders (musl-*, lwip-*) from build/.
set -e

cd "$(dirname "$0")"

find . -type f \( -name '*.o' -o -name '*.a' -o -name '*.app' \) -print -delete

if [ -d build ]; then
	find build -mindepth 1 -maxdepth 1 -type d -print -exec rm -rf {} +
fi
