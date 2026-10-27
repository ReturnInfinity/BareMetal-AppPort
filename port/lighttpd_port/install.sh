#!/bin/bash
# install.sh -- put lighttpd.app's config and document root onto an
# EXT2 disk image, via debugfs -w (no root/loop-mount needed -- same
# approach as port/lua_port/install-main.sh).
#
# Usage: ./install.sh /path/to/disk.img [lighttpd.conf] [docroot-dir]
#
# Defaults to this directory's lighttpd.conf and www/. The config lands
# at /lighttpd/lighttpd.conf (the fixed path lighttpd.c passes to -f),
# and the docroot's contents under /lighttpd/www, replacing whatever
# was there.
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -lt 1 ] || [ $# -gt 3 ]; then
	echo "usage: $0 /path/to/disk.img [lighttpd.conf] [docroot-dir]" >&2
	exit 1
fi

DISK="$1"
CONF="${2:-$SCRIPT_DIR/lighttpd.conf}"
WWW="${3:-$SCRIPT_DIR/www}"

for f in "$DISK" "$CONF"; do
	if [ ! -f "$f" ]; then
		echo "error: $f not found" >&2
		exit 1
	fi
done
if [ ! -d "$WWW" ]; then
	echo "error: $WWW is not a directory" >&2
	exit 1
fi

CMDFILE="$(mktemp)"
LOG="/tmp/install-lighttpd-debugfs.log"
trap 'rm -f "$CMDFILE"' EXIT

# Removes an existing on-disk tree depth-first (debugfs's rmdir only
# takes empty directories), from a listing of what's there now.
remove_tree() {
	local dir="$1"
	debugfs -R "ls -p $dir" "$DISK" 2>/dev/null | while IFS=/ read -r _ _ mode _ _ name _; do
		[ -z "$name" ] || [ "$name" = "." ] || [ "$name" = ".." ] && continue
		case "$mode" in
		040*) remove_tree "$dir/$name" ;; # debugfs prints octal modes zero-padded: 040755
		*) echo "rm $dir/$name" ;;
		esac
	done
	echo "rmdir $dir"
}

{
	# "already exists"/"not found" errors on first or later runs are
	# expected -- debugfs reports them and carries on.
	echo "mkdir /lighttpd"
	echo "rm /lighttpd/lighttpd.conf"
	echo "write $CONF /lighttpd/lighttpd.conf"
	echo "mkdir /lighttpd/tmp" # server.upload-dirs in the default config
	remove_tree /lighttpd/www
	echo "mkdir /lighttpd/www"
	(cd "$WWW" && find . -mindepth 1 -type d | sort) | while read -r d; do
		echo "mkdir /lighttpd/www/${d#./}"
	done
	(cd "$WWW" && find . -type f | sort) | while read -r f; do
		echo "write $WWW/${f#./} /lighttpd/www/${f#./}"
	done
} > "$CMDFILE"

echo "Installing $CONF and $WWW/ under /lighttpd on $DISK ..."
debugfs -w -f "$CMDFILE" "$DISK" > "$LOG" 2>&1 || true
if ! debugfs -R 'stat /lighttpd/lighttpd.conf' "$DISK" 2>/dev/null | grep -q '^Inode:'; then
	echo "error: /lighttpd/lighttpd.conf isn't on $DISK after the write -- see $LOG" >&2
	cat "$LOG" >&2
	exit 1
fi
