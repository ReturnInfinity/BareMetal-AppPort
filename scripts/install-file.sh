#!/usr/bin/env bash
# Writes an arbitrary file onto an EXT2 disk image at a given path, via
# debugfs -- same approach as port/mbedtls_port/install-cacert.sh and
# port/python_port/install-main.sh/install-stdlib.sh, generalized so it
# isn't tied to one specific file. No host root/loop-mount needed, and
# safe even while a VM has the disk image open.
#
# Typical use: installing a build-module.sh-built .so where a running
# app's dlopen() (port/dlfcn_shim.c) can read it back.
#
# Usage: ./install-file.sh /path/to/disk.img /path/to/local/file /dest/path/on/disk
set -eu

if [ $# -ne 3 ]; then
	echo "usage: $0 /path/to/disk.img /path/to/local/file /dest/path/on/disk" >&2
	exit 1
fi

DISK="$1"
SRC="$2"
DEST="$3"

if [ ! -f "$DISK" ]; then
	echo "error: $DISK not found" >&2
	exit 1
fi
if [ ! -f "$SRC" ]; then
	echo "error: $SRC not found" >&2
	exit 1
fi
case "$DEST" in
/*) ;;
*)
	echo "error: dest path '$DEST' must be absolute (e.g. /modules/mymodule.so)" >&2
	exit 1
	;;
esac

CMDFILE="$(mktemp)"
LOG="/tmp/install-file-debugfs.log"
trap 'rm -f "$CMDFILE"' EXIT

{
	# mkdir each parent directory in turn -- "already exists" errors
	# are expected on every run after the first. debugfs has no
	# -f-script-wide way to ignore a single failing command and
	# continue, so this relies on debugfs's own behavior of reporting
	# the error and moving on to the next command rather than
	# aborting the whole run (same as install-cacert.sh).
	dir="$(dirname "$DEST")"
	path=""
	IFS='/' read -ra parts <<< "$dir"
	for part in "${parts[@]}"; do
		[ -z "$part" ] && continue
		path="$path/$part"
		echo "mkdir $path"
	done
	echo "write $SRC $DEST"
} > "$CMDFILE"

echo "Installing $SRC as $DEST on $DISK ..."
# debugfs's own exit code can't signal failure here (every command
# inside can fail while the process itself still exits 0) -- verify by
# stat'ing the file back out afterward instead.
debugfs -w -f "$CMDFILE" "$DISK" > "$LOG" 2>&1 || true
if ! debugfs -R "stat $DEST" "$DISK" 2>/dev/null | grep -q '^Inode:'; then
	echo "error: $DEST isn't on $DISK after the write -- see $LOG" >&2
	cat "$LOG" >&2
	exit 1
fi
echo "Done (debugfs log: $LOG). Verify with: debugfs -R 'stat $DEST' \"$DISK\""
