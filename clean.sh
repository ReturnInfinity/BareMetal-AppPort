#!/bin/bash
# Remove all build artifacts (.o, .a, .app, and the .app.elf
# intermediate build-app.sh links before objcopy flattens it to .app --
# see its --gc-sections comment) from this directory, and the
# extracted source folders (musl-*, lwip-*) from build/.
set -e

cd "$(dirname "$0")"

find . -type f \( -name '*.o' -o -name '*.a' -o -name '*.app' -o -name '*.elf' \) -delete

if [ -d build ]; then
	find build -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} +
fi
