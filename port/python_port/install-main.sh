#!/usr/bin/env bash
# EXPERIMENTAL (see ../../PYTHON_PORT.md). Writes a .py file onto an
# EXT2 disk image as /pylib/main.py -- the file pymain_baremetal.c
# actually runs (see PYMAIN_SCRIPT_PATH there). Same debugfs -w
# approach as install-stdlib-phase3.sh (no host root/loop-mount, safe
# to run against a disk image a Firecracker VM doesn't currently have
# open), just for the one file that changes on every deploy instead of
# the stdlib slice that changes rarely.
#
# Unlike install-stdlib-phase3.sh, this one *is* idempotent -- main.py
# is expected to be replaced often (that's the whole point), so an
# existing /pylib/main.py is removed first rather than erroring out.
#
# Usage: ./install-main.sh /path/to/disk.img [/path/to/your_script.py]
# With no second argument, installs this directory's own
# main_test.py -- a smoke test covering what Phases 1-3 proved works
# on this port (core language, os/sys/time, _socket, and the real
# /pylib-backed json/re/collections/encodings.ascii imports), plus one
# EXPERIMENTAL, timeout-guarded _thread test (see that file's own
# comment for why it's bounded rather than a plain blocking acquire()).
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
	echo "usage: $0 /path/to/disk.img [/path/to/your_script.py]" >&2
	exit 1
fi

DISK="$1"
SRC="${2:-$SCRIPT_DIR/main_test.py}"

if [ ! -f "$DISK" ]; then
	echo "error: $DISK not found" >&2
	exit 1
fi
if [ ! -f "$SRC" ]; then
	echo "error: $SRC not found" >&2
	exit 1
fi

CMDFILE="$(mktemp)"
trap 'rm -f "$CMDFILE"' EXIT

{
	# mkdir errors (already exists) are expected on every run after
	# the first -- debugfs has no -f-script-wide way to ignore a
	# single failing command and continue, so this relies on debugfs's
	# own behavior of reporting the error and moving on to the next
	# command in the file rather than aborting the whole run.
	echo "mkdir /pylib"
	echo "rm /pylib/main.py"
	echo "write $SRC /pylib/main.py"
} > "$CMDFILE"

echo "Installing $SRC as /pylib/main.py on $DISK ..."
debugfs -w -f "$CMDFILE" "$DISK"
echo "Done. Verify with: debugfs -R 'cat /pylib/main.py' \"$DISK\""
