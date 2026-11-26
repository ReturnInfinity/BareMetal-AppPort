#!/usr/bin/env bash
set -e

# Build a BareMetal app from a Go main package. See GO.md.
#
# Usage: ./build-go-app.sh path/to/main.go     (single file, no go.mod needed)
#        ./build-go-app.sh path/to/package-dir (a main package inside a module)
#
# The output is named after the .go file (or the directory), e.g.
# hello.go -> hello.app.
#
# The Go code is compiled by the host's own Go toolchain with
# -buildmode=c-archive. That produces position-independent code (so it
# runs at c.ld's 0xFFFF800000000000) that starts the Go runtime from an
# .init_array constructor and creates its threads with pthread_create()
# (so they run on thread_shim.c). port/go_port/go_main.c is the app's C
# main(), which calls into main.main via port/go_port/bm_main.go. The
# archive is then linked by build-app.sh like any other app.

BUILD_DIR="build"
GO_PORT="port/go_port"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -ne 1 ]; then
	echo "usage: $0 main.go | package-dir" >&2
	exit 1
fi

if ! command -v go >/dev/null 2>&1; then
	echo "error: 'go' not found -- install Go (https://go.dev/dl/) first." >&2
	exit 1
fi

SRC="$(realpath "$1")"
if [ -d "$SRC" ]; then
	PKG_DIR="$SRC"
	APP_BASE="$(basename "$SRC")"
	GO_TARGETS=(.)
elif [ -f "$SRC" ] && [[ "$SRC" == *.go ]]; then
	PKG_DIR="$(dirname "$SRC")"
	APP_BASE="$(basename "$SRC" .go)"
	GO_TARGETS=("$(basename "$SRC")" bm_main.go)
else
	echo "error: $1 is neither a .go file nor a directory" >&2
	exit 1
fi
APP_NAME="$APP_BASE.app"

cd "$SCRIPT_DIR"
mkdir -p "$BUILD_DIR/go"
GO_BUILD="$(realpath "$BUILD_DIR/go")"
MUSL_INC="$(realpath "$BUILD_DIR/musl-1.2.6/sysroot/usr/local/musl/include")"
if [ ! -d "$MUSL_INC" ]; then
	echo "error: $MUSL_INC is missing -- run ./setup.sh first." >&2
	exit 1
fi

# Go's 64-bit runtime sizes its heap metadata for a 48-bit address space
# and reserves it all up front: about 1.1 GiB of PROT_NONE mappings
# (page-allocator summaries, scavenger index, arena map). Linux backs
# those lazily, but posix_shim.c's mmap() has no paging to work with:
# every mapping is real memory in the app's window. 40 bits (1 TiB,
# what Go already uses on ios/arm64) shrinks all of that to a few MiB.
# The heap sits right above arenaBaseOffset (0xffff800000000000 on
# amd64) here, so it is still inside the range. Patched with an
# -overlay instead of a forked GOROOT, so any host Go version works as
# long as the line below still matches.
GOROOT_RT="$(go env GOROOT)/src/runtime"
sed -E 's/^(\theapAddrBits = ).*$/\140/' "$GOROOT_RT/malloc.go" > "$GO_BUILD/malloc.go"
if ! grep -q $'^\theapAddrBits = 40$' "$GO_BUILD/malloc.go"; then
	echo "error: couldn't patch heapAddrBits in $GOROOT_RT/malloc.go -- this Go version may need build-go-app.sh updating." >&2
	exit 1
fi

cat > "$GO_BUILD/overlay.json" <<EOF
{
	"Replace": {
		"$GOROOT_RT/malloc.go": "$GO_BUILD/malloc.go",
		"$GOROOT_RT/bm_runtime.go": "$SCRIPT_DIR/$GO_PORT/bm_runtime.go",
		"$PKG_DIR/bm_main.go": "$SCRIPT_DIR/$GO_PORT/bm_main.go"
	}
}
EOF

# cgo compiles runtime/cgo (and any C in the package) against this
# port's musl headers, not the host's glibc ones.
GO_CC="gcc -nostdinc -isystem $MUSL_INC -isystem $(gcc -print-file-name=include) -fno-stack-protector"

ARCHIVE="$GO_BUILD/$APP_BASE.a"
echo "Building Go package ($(go version | awk '{print $3}'))..."
(cd "$PKG_DIR" && CGO_ENABLED=1 GOOS=linux GOARCH=amd64 CC="$GO_CC" \
	go build -trimpath -buildmode=c-archive -overlay "$GO_BUILD/overlay.json" \
	-o "$ARCHIVE" "${GO_TARGETS[@]}")
rm -f "${ARCHIVE%.a}.h"

APP_NAME="$APP_NAME" APP_LIBS="$ARCHIVE" ./build-app.sh "$GO_PORT/go_main.c"
