#!/usr/bin/env bash
# Writes a .sql script onto an EXT2 disk image as /sqlite/main.sql --
# the file sqlite.c has SQLite's shell .read (see SQLITE_SCRIPT_PATH
# there). Same debugfs -w approach as port/lua_port/install-main.sh (no
# host root/loop-mount, safe to run against a disk image a VM doesn't
# currently have open).
#
# The database the script runs against is /sqlite/main.db. It's left
# alone by default, so it keeps whatever earlier runs put in it. Pass a
# local database file as the third argument to replace it with a copy of
# that file instead, or "-" to delete it so the next run starts empty.
#
# Usage: ./install-main.sh /path/to/disk.img script.sql [database.db|-]
set -eu

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
	echo "usage: $0 /path/to/disk.img script.sql [database.db|-]" >&2
	exit 1
fi

DISK="$1"
SRC="$2"
DB="${3:-}"

for f in "$DISK" "$SRC"; do
	if [ ! -f "$f" ]; then
		echo "error: $f not found" >&2
		exit 1
	fi
done
if [ -n "$DB" ] && [ "$DB" != "-" ] && [ ! -f "$DB" ]; then
	echo "error: $DB not found" >&2
	exit 1
fi

CMDFILE="$(mktemp)"
LOG="/tmp/install-sqlite-main-debugfs.log"
trap 'rm -f "$CMDFILE"' EXIT

{
	# "already exists"/"not found" errors on first or later runs are
	# expected -- debugfs reports them and carries on.
	echo "mkdir /sqlite"
	echo "rm /sqlite/main.sql"
	echo "write $SRC /sqlite/main.sql"
	if [ -n "$DB" ]; then
		echo "rm /sqlite/main.db"
		# Rollback journal left by a run that didn't finish cleanly;
		# it belongs to the database being replaced.
		echo "rm /sqlite/main.db-journal"
		if [ "$DB" != "-" ]; then
			echo "write $DB /sqlite/main.db"
		fi
	fi
} > "$CMDFILE"

echo "Installing $SRC as /sqlite/main.sql on $DISK ..."
debugfs -w -f "$CMDFILE" "$DISK" > "$LOG" 2>&1 || true
if ! debugfs -R 'stat /sqlite/main.sql' "$DISK" 2>/dev/null | grep -q '^Inode:'; then
	echo "error: /sqlite/main.sql isn't on $DISK after the write -- see $LOG" >&2
	cat "$LOG" >&2
	exit 1
fi
if [ -n "$DB" ] && [ "$DB" != "-" ] && ! debugfs -R 'stat /sqlite/main.db' "$DISK" 2>/dev/null | grep -q '^Inode:'; then
	echo "error: /sqlite/main.db isn't on $DISK after the write -- see $LOG" >&2
	cat "$LOG" >&2
	exit 1
fi
