#!/bin/bash
# install.sh -- put caddy.app's Caddyfile and site onto an EXT2 disk
# image, via debugfs -w (no root/loop-mount needed -- same approach as
# port/lighttpd_port/install.sh).
#
# Usage: ./install.sh /path/to/disk.img [Caddyfile] [docroot-dir]
#
# Defaults to this directory's Caddyfile and www/. The config lands
# at /caddy/Caddyfile (the path main.go runs by default), and the
# site's contents under /caddy/www, replacing whatever was there.
# Caddy's own data (certificates, its local CA) lives under /caddy/data
# and is left alone.
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -lt 1 ] || [ $# -gt 3 ]; then
	echo "usage: $0 /path/to/disk.img [Caddyfile] [docroot-dir]" >&2
	exit 1
fi

DISK="$1"
CONF="${2:-$SCRIPT_DIR/Caddyfile}"
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
LOG="/tmp/install-caddy-debugfs.log"
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
	echo "mkdir /caddy"
	echo "rm /caddy/Caddyfile"
	echo "write $CONF /caddy/Caddyfile"
	echo "mkdir /caddy/data"
	echo "mkdir /caddy/config"
	remove_tree /caddy/www
	echo "mkdir /caddy/www"
	(cd "$WWW" && find . -mindepth 1 -type d | sort) | while read -r d; do
		echo "mkdir /caddy/www/${d#./}"
	done
	(cd "$WWW" && find . -type f | sort) | while read -r f; do
		echo "write $WWW/${f#./} /caddy/www/${f#./}"
	done
} > "$CMDFILE"

echo "Installing $CONF and $WWW/ under /caddy on $DISK ..."
debugfs -w -f "$CMDFILE" "$DISK" > "$LOG" 2>&1 || true
if ! debugfs -R 'stat /caddy/Caddyfile' "$DISK" 2>/dev/null | grep -q '^Inode:'; then
	echo "error: /caddy/Caddyfile isn't on $DISK after the write -- see $LOG" >&2
	cat "$LOG" >&2
	exit 1
fi
