#!/usr/bin/env bash
# Writes a curated set of real, unmodified Lib/*.py files from
# build/Python-3.14.7/ directly into an EXT2 disk image, under /pylib,
# using debugfs -w (part of e2fsprogs) -- deploying the disk-image
# content python.app needs is a per-deployment step, not a build step,
# so this is a standalone script rather than something setup.sh/
# build-app.sh run automatically. Not a loop mount (disk.sh's own
# approach): that needs root (`sudo mount`) and can't run against a
# disk image a Firecracker VM might currently have open; debugfs writes
# the raw ext2 structures directly, needing only read/write access to
# the image file itself, the same ext4_shim.c/lwext4 stack every other
# app's disk.img content already sits on.
#
# Usage: ./install-stdlib.sh /path/to/disk.img
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
LIB="$APPPORT_DIR/build/Python-3.14.7/Lib"

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
#
# The block below (_weakrefset.py through weakref.py) is the traced
# closure of `import http.server, socketserver, threading` on a real
# CPython (build/host-python-build/python -S -c "import ...", diffing
# sys.modules before/after, same method as the json/encodings.ascii
# block above) -- added once selectmodule.c/mathmodule.c/_struct.c/
# binascii.c/_randommodule.c were wired into config_baremetal.c/
# setup.sh's PYTHON_BUILTIN_SRCS (they, not this block, were the real
# missing pieces: os/os.path/posixpath/stat/genericpath are already
# frozen -- see Python/frozen.c -- and ssl/zlib/_datetime are each
# behind a real try/except ImportError in the stdlib source itself, so
# skipping _ssl/zlib and adding _pydatetime.py as datetime.py's
# fallback avoided needing those three C extensions at all).
FILES="
_collections_abc.py:.
copyreg.py:.
enum.py:.
functools.py:.
keyword.py:.
operator.py:.
platform.py:.
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
encodings/idna.py:encodings
stringprep.py:.
_weakrefset.py:.
base64.py:.
bisect.py:.
calendar.py:.
copy.py:.
datetime.py:.
_pydatetime.py:.
fnmatch.py:.
ipaddress.py:.
locale.py:.
mimetypes.py:.
quopri.py:.
random.py:.
selectors.py:.
shutil.py:.
socket.py:.
socketserver.py:.
string/__init__.py:string
struct.py:.
threading.py:.
warnings.py:.
_py_warnings.py:.
weakref.py:.
email/__init__.py:email
email/_encoded_words.py:email
email/_parseaddr.py:email
email/_policybase.py:email
email/base64mime.py:email
email/charset.py:email
email/encoders.py:email
email/errors.py:email
email/feedparser.py:email
email/header.py:email
email/iterators.py:email
email/message.py:email
email/parser.py:email
email/quoprimime.py:email
email/utils.py:email
html/__init__.py:html
html/entities.py:html
http/__init__.py:http
http/client.py:http
http/server.py:http
urllib/__init__.py:urllib
urllib/parse.py:urllib
"

CMDFILE="$(mktemp)"
LOG="/tmp/install-stdlib-debugfs.log"
trap 'rm -f "$CMDFILE"' EXIT

{
	echo "mkdir /pylib"
	echo "mkdir /pylib/collections"
	echo "mkdir /pylib/json"
	echo "mkdir /pylib/re"
	echo "mkdir /pylib/encodings"
	echo "mkdir /pylib/email"
	echo "mkdir /pylib/html"
	echo "mkdir /pylib/http"
	echo "mkdir /pylib/urllib"
	echo "mkdir /pylib/string"
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
# mkdir/write "already exists" errors are expected on every run after
# the first -- the curated file list rarely changes, so re-deploying
# just skips re-creating what's already there. debugfs has no
# -f-script-wide way to ignore a single failing command and continue,
# so this relies on debugfs's own behavior of reporting the error and
# moving on to the next command in the file rather than aborting the
# whole run. That transcript goes to a log file instead of the screen
# (same approach as install-main.sh) since this script runs on every
# deploy and the "already exists" noise would otherwise scroll past
# every single time.
debugfs -w -f "$CMDFILE" "$DISK" > "$LOG" 2>&1 || true
if grep -q 'No such file or directory while opening' "$LOG"; then
	echo "error: a source file in FILES above is missing from $LIB (stdlib layout changed upstream?) -- see $LOG" >&2
	cat "$LOG" >&2
	exit 1
fi
if ! debugfs -R 'stat /pylib/json/__init__.py' "$DISK" 2>/dev/null | grep -q '^Inode:'; then
	echo "error: /pylib doesn't look populated on $DISK after the write -- see $LOG" >&2
	cat "$LOG" >&2
	exit 1
fi
echo "Done (debugfs log: $LOG). Verify with: debugfs -R 'ls -l /pylib' \"$DISK\""
