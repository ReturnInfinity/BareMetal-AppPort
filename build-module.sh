#!/usr/bin/env bash
set -e

# Build a dlopen()-able module (a real ET_DYN ELF64 shared object) for a
# BareMetal app -- see port/dlfcn_shim.c for the runtime loader this
# targets, and the top-level README for how dlopen() support works here.
#
# Usage: ./build-module.sh [-Ixxx|-Dxxx|-isystem xxx|...] yourmodule.c [otherfile.c ...] -o yourmodule.so
#
# Any argument starting with "-" other than "-o" is passed straight
# through to gcc, appended after this script's own CFLAGS -- e.g. for a
# Python C-extension module (see ../pyexttest.c), which needs Python's
# own Include tree plus this port's own pyconfig.h on the search path:
#   ./build-module.sh -I build/Python-3.12.8/Include -I port/python_port \
#       -isystem "$(gcc -print-file-name=include)" \
#       pyexttest.c -o build/pyexttest.so
# Most modules aren't Python extensions, so those paths aren't baked into
# this script's own CFLAGS below -- pass them explicitly when they're
# needed instead.
#
# Unlike build-app.sh, a module is NOT linked against musl (no libc.a,
# no crt0.o/posix_shim.o/etc) and is NOT flattened with objcopy -O binary
# at the end -- port/dlfcn_shim.c needs a real, loadable ELF64 shared
# object (PT_LOAD/PT_DYNAMIC/.dynsym/.rela.dyn intact) to parse and
# relocate at runtime. A module can only call back into the host app
# through dlfcn_shim.c's curated dl_exports[] table -- extend that table
# if your module needs something from libc/libBareMetal that isn't
# listed there yet.

BUILD_DIR="build"

MUSL_DIR="$BUILD_DIR/musl-1.2.6"
MUSL_INC="$MUSL_DIR/sysroot/usr/local/musl/include"
MUSL_LIB="$MUSL_DIR/lib/libc.a"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ ! -f "$MUSL_LIB" ]; then
	echo "error: $MUSL_LIB is missing -- run ./setup.sh first." >&2
	exit 1
fi

SRCS=()
OUT=""
EXTRA_FLAGS=()
while [ $# -gt 0 ]; do
	case "$1" in
	-o)
		OUT="$2"
		shift 2
		;;
	-isystem|-I|-D|-include)
		# Two-token flags (flag, then its argument, as a separate
		# shell word) -- gcc accepts -Ixxx attached too, but -I xxx
		# with a space is common enough (this script's own usage
		# comment above uses it) that it needs handling here, or
		# the path silently falls through to the "*)" source-file
		# case below instead of being attached to the flag.
		EXTRA_FLAGS+=("$1" "$2")
		shift 2
		;;
	-*)
		EXTRA_FLAGS+=("$1")
		shift
		;;
	*)
		SRCS+=("$1")
		shift
		;;
	esac
done

if [ ${#SRCS[@]} -eq 0 ] || [ -z "$OUT" ]; then
	echo "usage: $0 [-Ixxx|-Dxxx|-isystem xxx|...] yourmodule.c [otherfile.c ...] -o yourmodule.so" >&2
	exit 1
fi

mkdir -p "$BUILD_DIR"

# -fPIC/-shared (not -fno-pic/-fno-pie/-mcmodel=large like build-app.sh):
# a module's load address is chosen at runtime by dlfcn_shim.c's
# malloc(), not fixed at link time, so it has to be position-independent
# code. -fno-plt routes calls to undefined externals (musl/libBareMetal
# functions the host exports -- see dl_exports[] in dlfcn_shim.c)
# straight through the GOT (R_X86_64_GLOB_DAT) instead of through a PLT
# stub whose lazy-binding trampoline would jump into a resolver that
# doesn't exist here; dlfcn_shim.c resolves every relocation eagerly at
# dlopen() time regardless (there's no ld.so to defer to), so this just
# keeps the relocation set simple rather than changing what's possible.
# --hash-style=both guarantees a DT_HASH-format .hash section exists
# (not just .gnu.hash) -- dlfcn_shim.c reads DT_HASH's nchain as the
# .dynsym entry count, since it has no section headers to consult
# (nothing here relies on the hash table itself, just that one count).
CFLAGS="-c -m64 -fPIC -fno-plt -nostdlib -nostartfiles -nodefaultlibs -ffreestanding -fno-stack-protector -mno-red-zone -fno-builtin -falign-functions=16 -fomit-frame-pointer -nostdinc -isystem $MUSL_INC"

echo "Building module..."

OBJS=()
for src in "${SRCS[@]}"; do
	obj="$BUILD_DIR/$(basename "$src" .c).mod.o"
	gcc $CFLAGS "${EXTRA_FLAGS[@]}" -o "$obj" "$src"
	OBJS+=("$obj")
done

ld -shared -m elf_x86_64 --hash-style=both -o "$OUT" "${OBJS[@]}"

echo "Built $OUT"
