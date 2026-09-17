#!/usr/bin/env bash
# Writes a .lua file onto an EXT2 disk image as /lualib/main.lua -- the
# file lua.c actually runs (see LUAMAIN_SCRIPT_PATH
# there). Same debugfs -w approach as
# port/python_port/install-main.sh (no host root/loop-mount, safe to
# run against a disk image a Firecracker VM doesn't currently have
# open).
#
# Idempotent -- main.lua is expected to be replaced often (that's the
# whole point), so an existing /lualib/main.lua is removed first
# rather than erroring out.
#
# Usage: ./install-main.sh /path/to/disk.img [/path/to/your_script.lua]
# With no second argument, installs this directory's own main_test.lua
# -- a smoke test covering core language, string/table/math libraries,
# coroutines, and real file I/O against the EXT2 disk image itself.
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
	echo "usage: $0 /path/to/disk.img [/path/to/your_script.lua]" >&2
	exit 1
fi

DISK="$1"
SRC="${2:-$SCRIPT_DIR/main_test.lua}"

if [ ! -f "$DISK" ]; then
	echo "error: $DISK not found" >&2
	exit 1
fi
if [ ! -f "$SRC" ]; then
	echo "error: $SRC not found" >&2
	exit 1
fi

CMDFILE="$(mktemp)"
LOG="/tmp/install-lua-main-debugfs.log"
trap 'rm -f "$CMDFILE"' EXIT

{
	# mkdir errors (already exists) are expected on every run after
	# the first -- debugfs has no -f-script-wide way to ignore a
	# single failing command and continue, so this relies on debugfs's
	# own behavior of reporting the error and moving on to the next
	# command in the file rather than aborting the whole run.
	echo "mkdir /lualib"
	echo "rm /lualib/main.lua"
	echo "write $SRC /lualib/main.lua"
} > "$CMDFILE"

echo "Installing $SRC as /lualib/main.lua on $DISK ..."
# debugfs's own exit code can't signal failure here (see
# port/python_port/install-main.sh's own comment for why) -- verify by
# stat'ing the file back out afterward instead.
debugfs -w -f "$CMDFILE" "$DISK" > "$LOG" 2>&1 || true
if ! debugfs -R 'stat /lualib/main.lua' "$DISK" 2>/dev/null | grep -q '^Inode:'; then
	echo "error: /lualib/main.lua isn't on $DISK after the write -- see $LOG" >&2
	cat "$LOG" >&2
	exit 1
fi
echo "Done (debugfs log: $LOG). Verify with: debugfs -R 'cat /lualib/main.lua' \"$DISK\""
