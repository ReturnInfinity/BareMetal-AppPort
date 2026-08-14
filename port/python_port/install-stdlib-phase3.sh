#!/usr/bin/env bash
# EXPERIMENTAL, Phase 3 (see ../../PYTHON_PORT.md). Not wired into
# setup.sh/build-app.sh -- writes a curated set of real, unmodified
# Lib/*.py files from build/Python-3.12.8/ directly into an EXT2 disk
# image, under /pylib, using debugfs -w (part of e2fsprogs). Not a
# loop mount (disk.sh's own approach): that needs root (`sudo mount`)
# and can't run against a disk image a Firecracker VM might currently
# have open; debugfs writes the raw ext2 structures directly, needing
# only read/write access to the image file itself, the same
# ext4_shim.c/lwext4 stack every other app's disk.img content already
# sits on.
#
# Usage: ./install-stdlib-phase3.sh /path/to/disk.img
#
# What gets installed is deliberately small, not "all of Lib/" --
# exactly the closure `import json` and `import encodings.ascii` pull
# in on a real CPython (traced by diffing sys.modules before/after
# against build/host-python-build/python -S -c "import json", the
# authoritative way to get this right instead of guessing at
# dependencies by reading source). This is meant to be extended the
# same way -- trace a real import, add exactly what's new -- not grown
# by copying more of Lib/ speculatively.
set -eu

if [ $# -ne 1 ]; then
	echo "usage: $0 /path/to/disk.img" >&2
	exit 1
fi

DISK="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPPORT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
LIB="$APPPORT_DIR/build/Python-3.12.8/Lib"

if [ ! -f "$DISK" ]; then
	echo "error: $DISK not found" >&2
	exit 1
fi
if [ ! -d "$LIB" ]; then
	echo "error: $LIB not found -- run scripts/get-python.sh first" >&2
	exit 1
fi

# name:destination-subdir pairs, one per line -- destination-subdir is
# "." for /pylib itself.
FILES="
_collections_abc.py:.
copyreg.py:.
enum.py:.
functools.py:.
keyword.py:.
operator.py:.
reprlib.py:.
types.py:.
collections/__init__.py:collections
json/__init__.py:json
json/decoder.py:json
json/encoder.py:json
json/scanner.py:json
re/__init__.py:re
re/_casefix.py:re
re/_compiler.py:re
re/_constants.py:re
re/_parser.py:re
encodings/ascii.py:encodings
"

CMDFILE="$(mktemp)"
trap 'rm -f "$CMDFILE"' EXIT

{
	echo "mkdir /pylib"
	echo "mkdir /pylib/collections"
	echo "mkdir /pylib/json"
	echo "mkdir /pylib/re"
	echo "mkdir /pylib/encodings"
	while IFS= read -r line; do
		[ -z "$line" ] && continue
		src="${line%%:*}"
		destdir="${line##*:}"
		base="$(basename "$src")"
		if [ "$destdir" = "." ]; then
			dest="/pylib/$base"
		else
			dest="/pylib/$destdir/$base"
		fi
		echo "write $LIB/$src $dest"
	done <<< "$FILES"
} > "$CMDFILE"

echo "Writing $(grep -c '^write' "$CMDFILE") files into $DISK under /pylib ..."
debugfs -w -f "$CMDFILE" "$DISK"
echo "Done. Verify with: debugfs -R 'ls -l /pylib' \"$DISK\""
