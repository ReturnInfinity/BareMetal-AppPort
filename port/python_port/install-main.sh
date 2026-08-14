#!/usr/bin/env bash
# Writes a .py file onto an EXT2 disk image as /pylib/main.py -- the
# file python.c actually runs (see PYMAIN_SCRIPT_PATH there). Same
# debugfs -w approach as install-stdlib.sh (no host root/loop-mount,
# safe to run against a disk image a Firecracker VM doesn't currently
# have open), just for the one file that changes on every deploy
# instead of the stdlib slice that changes rarely.
#
# Unlike install-stdlib.sh, this one *is* idempotent -- main.py
# is expected to be replaced often (that's the whole point), so an
# existing /pylib/main.py is removed first rather than erroring out.
#
# Usage: ./install-main.sh /path/to/disk.img [/path/to/your_script.py]
# With no second argument, installs this directory's own main_test.py
# -- a smoke test covering core language, os/sys/time, _socket, and the
# real /pylib-backed json/re/collections/encodings.ascii imports, plus
# one timeout-guarded _thread test (see that file's own comment for why
# it's bounded rather than a plain blocking acquire()).
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
LOG="/tmp/install-main-debugfs.log"
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
# debugfs's own transcript (command echoes, "Allocated inode", the
# expected "already exists" noise on every run after the first -- see
# above) goes to a log file instead of the screen; this script is
# meant to be run on every deploy (install-main.sh's own header), so
# that output would otherwise scroll past on every single run.
#
# debugfs's own exit code can't signal failure here -- confirmed by
# running it against a plain non-ext2 file: every command inside fails
# ("Filesystem not open"), but the process itself still exits 0
# regardless. Real verification instead: stat the file back out
# afterward and check it actually landed.
debugfs -w -f "$CMDFILE" "$DISK" > "$LOG" 2>&1 || true
if ! debugfs -R 'stat /pylib/main.py' "$DISK" 2>/dev/null | grep -q '^Inode:'; then
	echo "error: /pylib/main.py isn't on $DISK after the write -- see $LOG" >&2
	cat "$LOG" >&2
	exit 1
fi
echo "Done (debugfs log: $LOG). Verify with: debugfs -R 'cat /pylib/main.py' \"$DISK\""
