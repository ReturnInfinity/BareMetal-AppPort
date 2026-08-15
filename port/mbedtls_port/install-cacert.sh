#!/usr/bin/env bash
# Writes build/cacert.pem (Mozilla's CA root bundle -- see
# scripts/get-cacert.sh) onto an EXT2 disk image as /etc/ssl/cacert.pem,
# the trust store port/tls_shim.c's mbedtls_x509_crt_parse_file()
# (TLS_CA_BUNDLE_PATH) and curltest.c's CURLOPT_CAINFO (CA_BUNDLE_PATH)
# both point at. Same debugfs -w approach as port/python_port/
# install-stdlib.sh -- no host root/loop-mount needed, and safe even
# while a VM has the disk image open.
#
# Unlike install-stdlib.sh/install-main.sh (run per-deploy, by hand),
# the outer BareMetal-App repo's setup.sh runs this once, right after
# it formats a fresh disk.img -- the CA bundle is a system file every
# app's HTTPS traffic depends on, not something that varies per
# program the way main.py does.
#
# Usage: ./install-cacert.sh /path/to/disk.img
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPPORT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
CACERT="$APPPORT_DIR/build/cacert.pem"

if [ $# -ne 1 ]; then
	echo "usage: $0 /path/to/disk.img" >&2
	exit 1
fi

DISK="$1"

if [ ! -f "$DISK" ]; then
	echo "error: $DISK not found" >&2
	exit 1
fi
if [ ! -f "$CACERT" ]; then
	echo "error: $CACERT not found -- run scripts/get-cacert.sh first" >&2
	exit 1
fi

CMDFILE="$(mktemp)"
LOG="/tmp/install-cacert-debugfs.log"
trap 'rm -f "$CMDFILE"' EXIT

{
	# mkdir errors (already exists) are expected on every run after
	# the first -- debugfs has no -f-script-wide way to ignore a
	# single failing command and continue, so this relies on debugfs's
	# own behavior of reporting the error and moving on to the next
	# command in the file rather than aborting the whole run.
	echo "mkdir /etc"
	echo "mkdir /etc/ssl"
	echo "write $CACERT /etc/ssl/cacert.pem"
} > "$CMDFILE"

echo "Installing $CACERT as /etc/ssl/cacert.pem on $DISK ..."
# debugfs's own exit code can't signal failure here (same caveat as
# install-main.sh -- see its own comment): every command inside can
# fail while the process itself still exits 0. Real verification
# instead: stat the file back out afterward and check it actually
# landed.
debugfs -w -f "$CMDFILE" "$DISK" > "$LOG" 2>&1 || true
if ! debugfs -R 'stat /etc/ssl/cacert.pem' "$DISK" 2>/dev/null | grep -q '^Inode:'; then
	echo "error: /etc/ssl/cacert.pem isn't on $DISK after the write -- see $LOG" >&2
	cat "$LOG" >&2
	exit 1
fi
echo "Done (debugfs log: $LOG). Verify with: debugfs -R 'stat /etc/ssl/cacert.pem' \"$DISK\""
