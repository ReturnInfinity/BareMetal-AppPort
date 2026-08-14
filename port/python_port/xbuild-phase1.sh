#!/usr/bin/env bash
# EXPERIMENTAL, Phase 1 (see ../../PYTHON_PORT.md). Not wired into
# setup.sh/build-app.sh -- this hand-drives a cross-compile of CPython
# 3.12.8's core interpreter + Modules/Setup.bootstrap.in's static
# module set against this port's musl sysroot, the same "bypass
# ./configure, invoke gcc directly with a hand-written config header"
# choice build-app.sh already makes for curl/mbedTLS/lwIP/lwext4 --
# CPython's own configure can't run its AC_TRY_RUN probes against a
# target that can't execute anything on this build host either.
#
# Object/file lists below are copied from build/host-python-build/
# Makefile's PARSER_OBJS/PYTHON_OBJS/OBJECT_OBJS/MODOBJS (a normal
# native build of this exact same source, done only to get generated,
# architecture-independent artifacts -- the pegen parser tables, the
# AST node definitions, the bytecode dispatch table, and the frozen
# importlib/os/site/etc bytecode in Python/deepfreeze/deepfreeze.c --
# see PYTHON_PORT.md's Phase 1 section). $(MACHDEP_OBJS)/$(LIBOBJS)/
# $(DTRACE_OBJS) were empty for that build and are omitted; pwdmodule
# is dropped from MODOBJS (needs getpwuid(), no uid/gid model on this
# port -- see OPENISSUES.md's Process model section). PHASE2_MODOBJS
# (Modules/socketmodule.c) is this port's own addition, not copied
# from the Makefile -- see its own comment below.
#
# Compiles every translation unit, reports which ones fail and why (the
# fast way to find every real gap between CPython's assumptions and
# this port's musl+posix_shim.c surface before spending time on any
# single one), then links and produces python.app.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPPORT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
cd "$APPPORT_DIR"

SRC="build/Python-3.12.8"
OUT="build/pyxobj"
LOG="build/pyxbuild.log"
MUSL_INC="build/musl-1.2.6/sysroot/usr/local/musl/include"
HOST_BUILD="build/host-python-build"

# gcc's own freestanding headers (stdatomic.h, stdarg.h, ...) -- not
# musl's job to provide, and -nostdinc below drops gcc's normal search
# path along with the host's. musl's own -isystem entry comes first so
# its stddef.h/stdint.h/etc (which it does ship, unlike stdatomic.h)
# still win; this only fills the gap. curl/mbedTLS/lwIP/SQLite/lwext4
# never needed this (build-app.sh has no equivalent) -- CPython is the
# first thing built against this port to use C11 <stdatomic.h>.
GCC_FREESTANDING_INC="$(gcc -print-file-name=include)"

mkdir -p "$OUT"
: > "$LOG"

CFLAGS_BASE="-c -m64 -nostdlib -nostartfiles -nodefaultlibs -ffreestanding -fno-pic -fno-pie -mcmodel=large -falign-functions=16 -fomit-frame-pointer -mno-red-zone -fno-builtin -fno-stack-protector -ffunction-sections -fdata-sections -nostdinc -isystem $MUSL_INC -isystem $GCC_FREESTANDING_INC -I $SCRIPT_DIR -I $SRC -I $SRC/Include -I $SRC/Include/internal -I $HOST_BUILD/Python"

CORE_CFLAGS="$CFLAGS_BASE -DPy_BUILD_CORE"
BUILTIN_CFLAGS="$CFLAGS_BASE -DPy_BUILD_CORE_BUILTIN"

PARSER_OBJS="Parser/token.c Parser/pegen.c Parser/pegen_errors.c Parser/action_helpers.c Parser/parser.c Parser/string_parser.c Parser/peg_api.c Parser/myreadline.c Parser/tokenizer.c"

PYTHON_OBJS="Python/_warnings.c Python/Python-ast.c Python/Python-tokenize.c Python/asdl.c Python/assemble.c Python/ast.c Python/ast_opt.c Python/ast_unparse.c Python/bltinmodule.c Python/ceval.c Python/codecs.c Python/compile.c Python/context.c Python/dynamic_annotations.c Python/errors.c Python/flowgraph.c Python/frame.c Python/frozenmain.c Python/future.c Python/getargs.c Python/getcompiler.c Python/getcopyright.c Python/getplatform.c Python/getversion.c Python/ceval_gil.c Python/hamt.c Python/hashtable.c Python/import.c Python/importdl.c Python/initconfig.c Python/instrumentation.c Python/intrinsics.c Python/legacy_tracing.c Python/marshal.c Python/modsupport.c Python/mysnprintf.c Python/mystrtoul.c Python/pathconfig.c Python/preconfig.c Python/pyarena.c Python/pyctype.c Python/pyfpe.c Python/pyhash.c Python/pylifecycle.c Python/pymath.c Python/pystate.c Python/pythonrun.c Python/pytime.c Python/bootstrap_hash.c Python/specialize.c Python/structmember.c Python/symtable.c Python/sysmodule.c Python/thread.c Python/traceback.c Python/tracemalloc.c Python/getopt.c Python/pystrcmp.c Python/pystrtod.c Python/pystrhex.c Python/dtoa.c Python/formatter_unicode.c Python/fileutils.c Python/suggestions.c Python/perf_trampoline.c Python/dynload_stub.c"

OBJECT_OBJS="Objects/abstract.c Objects/boolobject.c Objects/bytes_methods.c Objects/bytearrayobject.c Objects/bytesobject.c Objects/call.c Objects/capsule.c Objects/cellobject.c Objects/classobject.c Objects/codeobject.c Objects/complexobject.c Objects/descrobject.c Objects/enumobject.c Objects/exceptions.c Objects/genericaliasobject.c Objects/genobject.c Objects/fileobject.c Objects/floatobject.c Objects/frameobject.c Objects/funcobject.c Objects/interpreteridobject.c Objects/iterobject.c Objects/listobject.c Objects/longobject.c Objects/dictobject.c Objects/odictobject.c Objects/memoryobject.c Objects/methodobject.c Objects/moduleobject.c Objects/namespaceobject.c Objects/object.c Objects/obmalloc.c Objects/picklebufobject.c Objects/rangeobject.c Objects/setobject.c Objects/sliceobject.c Objects/structseq.c Objects/tupleobject.c Objects/typeobject.c Objects/typevarobject.c Objects/unicodeobject.c Objects/unicodectype.c Objects/unionobject.c Objects/weakrefobject.c"

FROZEN_OBJS="Python/deepfreeze/deepfreeze.c Python/frozen.c Modules/getpath.c Modules/getbuildinfo.c"

# Modules/getpath.c normally gets these from the Makefile (in turn from
# configure --prefix/--exec-prefix/PLATLIBDIR, VERSION from patchlevel.h's
# PY_VERSION, VPATH from the build's own srcdir-relative-ness). None of
# that applies here -- no installed prefix, no VPATH build -- and Phase 1
# doesn't even use calculate_path()'s filesystem search (see
# PYTHON_PORT.md: the entry point sets PyConfig.module_search_paths
# directly instead), so these values are placeholders only needed to
# satisfy getpath.c's #error/undeclared-identifier checks at compile
# time, not meaningful at runtime yet.
# Note: no single-quote wrapping -- compile_one's `gcc $cflags ...`
# word-splits this variable directly (no nested shell re-parses it), so
# what needs to survive into argv is the literal text below, quote
# characters included, not shell-quoting syntax around it.
GETPATH_DEFINES='-DPREFIX="/" -DEXEC_PREFIX="/" -DVERSION="3.12" -DVPATH="" -DPLATLIBDIR="lib"'

# pwdmodule.c dropped from the host's own MODOBJS (needs getpwuid() --
# no uid/gid model, OPENISSUES.md's Process model section). gcmodule.c
# added: not in Modules/Setup.bootstrap.in at all (it's one of the
# handful -- alongside marshal.c/import.c/Python-ast.c/Python-tokenize.c/
# _warnings.c/unicodeobject.c's PyInit__string -- that Modules/config.c.in's
# own "ADDMODULE MARKER" mechanism always builds in regardless of Setup,
# see build/host-python-build/Modules/config.c), but unlike those it
# isn't already part of PARSER_OBJS/PYTHON_OBJS/OBJECT_OBJS above, so it
# needs to be added by hand here.
MODOBJS="Modules/atexitmodule.c Modules/faulthandler.c Modules/posixmodule.c Modules/signalmodule.c Modules/_tracemalloc.c Modules/_codecsmodule.c Modules/_collectionsmodule.c Modules/errnomodule.c Modules/_io/_iomodule.c Modules/_io/iobase.c Modules/_io/fileio.c Modules/_io/bytesio.c Modules/_io/bufferedio.c Modules/_io/textio.c Modules/_io/stringio.c Modules/itertoolsmodule.c Modules/_sre/sre.c Modules/_threadmodule.c Modules/timemodule.c Modules/_typingmodule.c Modules/_weakref.c Modules/_abc.c Modules/_functoolsmodule.c Modules/_localemodule.c Modules/_operator.c Modules/_stat.c Modules/symtablemodule.c Modules/gcmodule.c"

# Phase 2 (see PYTHON_PORT.md): Modules/socketmodule.c is a
# Modules/Setup.stdlib.in module (not bootstrap), but turned out to
# need no net_shim.c-backed C shim at all -- unlike sqlite_vfs.c
# (SQLite bypasses libc and needs a whole hand-written OS layer),
# socketmodule.c just calls ordinary socket()/connect()/send()/recv(),
# which posix_shim.c already dispatches to net_shim.c for every other
# app here. HAVE_GETADDRINFO already left undefined (pyconfig_baremetal.h)
# means it self-includes its own bundled getaddrinfo.c/getnameinfo.c
# fallback, built on gethostbyname() -- dns_shim.c's real resolver.
PHASE2_MODOBJS="Modules/socketmodule.c"

n_ok=0
n_fail=0
failed_files=""

compile_one() {
	local src="$1" cflags="$2"
	local rel="${src#$SRC/}"
	rel="${rel#$HOST_BUILD/}"
	rel="${rel#$SCRIPT_DIR/}"
	local obj="$OUT/$(echo "$rel" | tr '/' '_' | sed 's/\.c$/.o/')"
	if gcc $cflags -o "$obj" "$src" >> "$LOG" 2>&1; then
		n_ok=$((n_ok+1))
	else
		n_fail=$((n_fail+1))
		failed_files="$failed_files $rel"
		echo "=== FAILED: $src ===" >> "$LOG"
	fi
}

echo "Compiling parser/core/objects (-DPy_BUILD_CORE)..."
for f in $PARSER_OBJS $PYTHON_OBJS $OBJECT_OBJS $FROZEN_OBJS; do
	if [ "$f" = "Modules/getpath.c" ]; then
		compile_one "$SRC/$f" "$CORE_CFLAGS $GETPATH_DEFINES"
	else
		compile_one "$SRC/$f" "$CORE_CFLAGS"
	fi
done

echo "Compiling bootstrap static modules (-DPy_BUILD_CORE_BUILTIN)..."
for f in $MODOBJS; do
	compile_one "$SRC/$f" "$BUILTIN_CFLAGS"
done

echo "Compiling Phase 2 modules (-DPy_BUILD_CORE_BUILTIN)..."
for f in $PHASE2_MODOBJS; do
	compile_one "$SRC/$f" "$BUILTIN_CFLAGS"
done

echo "Compiling this port's own entry point + built-in module table..."
compile_one "$SCRIPT_DIR/pymain_baremetal.c" "$CORE_CFLAGS"
compile_one "$SCRIPT_DIR/config_baremetal.c" "$CORE_CFLAGS"
compile_one "$SCRIPT_DIR/frozen_encodings_baremetal.c" "$CORE_CFLAGS"

echo ""
echo "== Compile result: $n_ok ok, $n_fail failed =="
if [ -n "$failed_files" ]; then
	echo "Failed files:"
	for f in $failed_files; do echo "  $f"; done
	echo ""
	echo "See $LOG for full compiler output."
	exit 1
fi

# ---------------------------------------------------------------------
# Link -- same two-stage ELF-intermediate-then-objcopy approach
# build-app.sh uses (c.ld's OUTPUT_FORMAT(binary) defeats --gc-sections
# otherwise, see that script's own comment) with the same crt0.o/
# posix_shim.o/thread_shim.o this port's other apps link against.
#
# ext4_shim.o/net_glue.o/net_shim.o/dns_shim.o *are* linked in, even
# though pymain_baremetal.c itself touches no filesystem or network --
# tried omitting them first, and posix_shim.c's own SYS_fstat/SYS_open/
# SYS_socket/etc dispatch cases (see port/posix_shim.c) reference
# ext4_shim_*/net_shim_* unconditionally, not through any weak symbol
# or #ifdef. That's not a Phase-1-specific problem: build-app.sh links
# every one of these into every app the same way regardless of what
# that app calls (see its own link line) -- CPython just makes the
# same thing visible by actually exercising fstat() on stdio's fds
# during Py_InitializeFromConfig(), where hello.c's plain printf()
# never happened to. Pulled from build/ as already-built by an earlier
# build-app.sh run (identical CFLAGS/musl sysroot either way -- see
# that script's own ext4_shim.o/net_glue.o/... compile lines).
# ---------------------------------------------------------------------
LIBGCC="$(gcc -m64 -print-libgcc-file-name)"
MUSL_LIB="build/musl-1.2.6/lib/libc.a"
APP_NAME="python.app"

if [ ! -f "$MUSL_LIB" ]; then
	echo "error: $MUSL_LIB is missing -- run ./setup.sh first." >&2
	exit 1
fi

echo "Compiling this port's crt0/posix_shim/thread_shim..."
gcc $CFLAGS_BASE -o "$OUT/crt0.o" "port/crt0.c"
gcc $CFLAGS_BASE -o "$OUT/posix_shim.o" "port/posix_shim.c"
gcc $CFLAGS_BASE -o "$OUT/thread_shim.o" "port/thread_shim.c"
gcc $CFLAGS_BASE -o "$OUT/libBareMetal.o" "port/libBareMetal.c"

echo "Linking..."
rm -f "$OUT/$APP_NAME.elf"
PY_OBJS=""
for f in $PARSER_OBJS $PYTHON_OBJS $OBJECT_OBJS $FROZEN_OBJS $MODOBJS $PHASE2_MODOBJS; do
	rel="${f#$SRC/}"
	obj="$OUT/$(echo "$rel" | tr '/' '_' | sed 's/\.c$/.o/')"
	PY_OBJS="$PY_OBJS $obj"
done

LWIP_OBJS="$(ls build/lwip_*.o 2>/dev/null)"
LWEXT4_OBJS="$(ls build/lwext4_*.o 2>/dev/null)"

ld --gc-sections --no-warn-rwx-segments --oformat elf64-x86-64 -T "port/c.ld" -o "$OUT/$APP_NAME.elf" \
	"$OUT/crt0.o" "$OUT/posix_shim.o" "$OUT/thread_shim.o" "$OUT/libBareMetal.o" \
	build/ext4_shim.o build/blockdev_baremetal.o build/net_glue.o build/net_shim.o build/dns_shim.o \
	"$OUT/pymain_baremetal.o" "$OUT/config_baremetal.o" "$OUT/frozen_encodings_baremetal.o" \
	$PY_OBJS $LWIP_OBJS $LWEXT4_OBJS "$MUSL_LIB" "$LIBGCC" 2>&1 | tee -a "$LOG"

if [ -f "$OUT/$APP_NAME.elf" ]; then
	objcopy -O binary "$OUT/$APP_NAME.elf" "$APP_NAME"
	echo "Built $APP_NAME"
else
	echo "Link failed -- see $LOG"
	exit 1
fi
