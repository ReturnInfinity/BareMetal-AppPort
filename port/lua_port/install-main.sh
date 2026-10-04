#!/usr/bin/env bash
# Writes a .lua file onto an EXT2 disk image as /lua/main.lua -- the
# file lua.c actually runs (see LUAMAIN_SCRIPT_PATH there). Same
# debugfs -w approach as port/python_port/install-main.sh (no host
# root/loop-mount, safe to run against a disk image a Firecracker VM
# doesn't currently have open). Lua needs nothing else on disk -- its
# whole standard library is compiled into lua.app -- so there's no
# install-stdlib.sh counterpart.
#
# Idempotent: main.lua is expected to be replaced often, so an
# existing /lua/main.lua is removed first rather than erroring out.
#
# Usage: ./install-main.sh /path/to/disk.img [/path/to/your_script.lua]
# With no second argument, installs this directory's own main_test.lua
# -- a smoke test covering the core language and every standard library
# (see that file's own header).
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
	# The mkdir/rm errors on first/later runs ("already exists"/"not
	# found") are expected -- debugfs reports them and moves on to the
	# next command (see port/python_port/install-main.sh).
	echo "mkdir /lua"
	echo "rm /lua/main.lua"
	echo "write $SRC /lua/main.lua"
} > "$CMDFILE"

echo "Installing $SRC as /lua/main.lua on $DISK ..."
# debugfs exits 0 even when every command fails (see
# port/python_port/install-main.sh), so verify by stat-ing the file
# back out instead.
debugfs -w -f "$CMDFILE" "$DISK" > "$LOG" 2>&1 || true
if ! debugfs -R 'stat /lua/main.lua' "$DISK" 2>/dev/null | grep -q '^Inode:'; then
	echo "error: /lua/main.lua isn't on $DISK after the write -- see $LOG" >&2
	cat "$LOG" >&2
	exit 1
fi
echo "Done (debugfs log: $LOG). Verify with: debugfs -R 'cat /lua/main.lua' \"$DISK\""
