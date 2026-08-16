#!/usr/bin/env bash
set -e

# Builds every fixture test-dlopen.c (../test-dlopen.c) needs and
# installs them onto disk.img under /modules/dlopen_tests/ -- the
# "good" modules in dlopen_modules/*.c (one .so each, via
# build-module.sh) plus a set of deliberately-broken ELF files
# derived from mod_data.so by scripts/make-bad-dlopen-modules.py,
# since there's no compiler flag that produces "wrong ELF class" or
# "PT_DYNAMIC missing" on demand.
#
# Also installs modtest.so and pyexttest.so if they're already built
# (see modtest.c/pyexttest.c's own header comments for how to build
# them) -- test-dlopen.c's tier 2 exercises those as real consumers of
# the same loader, not just this script's synthetic fixtures. Neither
# is required: test-dlopen.c skips that tier's checks if the file
# isn't present on disk.
#
# Usage: ./build-dlopen-tests.sh [/path/to/disk.img]
# Defaults to ../disk.img (this repo's top-level image, same one
# 1-build.sh/1-build_elf.sh deploy apps onto).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

DISK="${1:-../disk.img}"
if [ ! -f "$DISK" ]; then
	echo "error: $DISK not found -- run ./setup.sh first, or pass a path explicitly." >&2
	exit 1
fi

OUT_DIR="build/dlopen_tests"
mkdir -p "$OUT_DIR"

echo "Building good fixtures..."
for src in dlopen_modules/*.c; do
	name="$(basename "$src" .c)"
	./build-module.sh "$src" -o "$OUT_DIR/$name.so"
done

echo "Deriving broken-ELF fixtures from mod_data.so..."
python3 scripts/make-bad-dlopen-modules.py "$OUT_DIR/mod_data.so" "$OUT_DIR"

echo "Installing fixtures onto $DISK ..."
for so in "$OUT_DIR"/*.so; do
	name="$(basename "$so")"
	scripts/install-file.sh "$DISK" "$so" "/modules/dlopen_tests/$name"
done

for extra in modtest.so pyexttest.so; do
	if [ -f "build/$extra" ]; then
		scripts/install-file.sh "$DISK" "build/$extra" "/modules/dlopen_tests/$extra"
	else
		echo "note: build/$extra not built -- skipping (see its .c file's header comment to build it); test-dlopen.c will SKIP that check."
	fi
done

echo "Done."
