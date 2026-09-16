#!/usr/bin/env bash
set -e

# Build a BareMetal app written in C++ against the musl/lwIP/mbedTLS/
# lwext4 port in this directory -- the C++ counterpart to build-app.sh.
# See CPP.md for the full story of why this works and what doesn't.
#
# Usage: ./build-cpp-app.sh yourapp.cpp [otherfile.cpp ...]
# The output is named after the first source file given, with a .app
# extension (e.g. myapp.cpp -> myapp.app).

BUILD_DIR="build"

BAREMETAL_DEBUG="${BAREMETAL_DEBUG:-FALSE}"

LIBGCC="$(gcc -m64 -print-libgcc-file-name)"

# The host's own libstdc++.a (Ubuntu's, built against glibc) -- see
# CPP.md for why linking a glibc-built static archive against this
# port's own musl works at all (short version: libstdc++'s own object
# code only ever calls libc functions by name -- malloc, memcpy,
# pthread_mutex_*, fwrite, ... -- never anything glibc-internal-layout-
# specific, so swapping in a real, ABI-compatible musl at final-link
# time is no different from any other libc function libstdc++ expects
# to find). Resolved via `g++ -print-file-name` rather than
# hard-coded, so this keeps working across whatever GCC version is
# actually installed.
LIBSTDCXX="$(g++ -print-file-name=libstdc++.a)"
if [ ! -f "$LIBSTDCXX" ]; then
	echo "error: could not find libstdc++.a via g++ -- is g++ installed?" >&2
	exit 1
fi

# The host g++'s own C++ standard headers (<vector>, <string>, ...),
# found the same way build-app.sh's own PYTHON_GCC_FREESTANDING_INC
# finds gcc's freestanding <stdatomic.h> etc: ask the compiler itself
# rather than hard-coding a path that shifts between distро versions.
# Order matters -- libstdc++ headers themselves (e.g. <cstdlib>) do
# `#include_next <stdlib.h>` to reach the real C header once they've
# added their C++ wrapper on top, and with -nostdinc (see CFLAGS
# below) the *only* stdlib.h anywhere on this search path is musl's
# own (-isystem $MUSL_INC, appended last) -- so #include_next finds
# musl's, not glibc's, and every C-level declaration <cstdlib>/
# <cstdio>/<cmath>/etc. re-export ends up being musl's, matching what
# the rest of this port already links against.
CXX_INC_ROOT="$(g++ -print-file-name=include 2>/dev/null || true)"
GXX_VER="$(g++ -dumpversion | cut -d. -f1)"
CXX_INC1="/usr/include/c++/$GXX_VER"
CXX_INC2="/usr/include/x86_64-linux-gnu/c++/$GXX_VER"
CXX_INC3="$CXX_INC1/backward"
if [ ! -d "$CXX_INC1" ]; then
	echo "error: $CXX_INC1 not found -- adjust build-cpp-app.sh for this g++ install layout." >&2
	exit 1
fi

MUSL_DIR="$BUILD_DIR/musl-1.2.6"
MUSL_INC="$MUSL_DIR/sysroot/usr/local/musl/include"
MUSL_LIB="$MUSL_DIR/lib/libc.a"

LWIP_DIR="$BUILD_DIR/lwip-2.2.0"
LWIP_INC="$LWIP_DIR/src/include"
LWIP_PORT="port/lwip_port"

MBEDTLS_DIR="$BUILD_DIR/mbedtls-3.6.6"
MBEDTLS_INC="$MBEDTLS_DIR/include"
MBEDTLS_PORT="port/mbedtls_port"

CURL_DIR="$BUILD_DIR/curl-8.21.0"
CURL_INC="$CURL_DIR/include"
CURL_PORT="port/curl_port"

SQLITE_DIR="$BUILD_DIR/sqlite-3.46.1"
SQLITE_INC="$SQLITE_DIR"
SQLITE_PORT="port/sqlite_port"

SODIUM_DIR="$BUILD_DIR/libsodium-1.0.22/src/libsodium"
SODIUM_INC="$SODIUM_DIR/include"
SODIUM_PORT="port/libsodium_port"

LWEXT4_DIR="$BUILD_DIR/lwext4-58bcf89"
LWEXT4_INC="$LWEXT4_DIR/include"
LWEXT4_PORT="port/lwext4_port"

CPP_PORT="port/cpp_port"

PORT="port"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ $# -eq 0 ]; then
	echo "usage: $0 yourapp.cpp [otherfile.cpp ...]" >&2
	exit 1
fi

if [ ! -f "$MUSL_LIB" ]; then
	echo "error: $MUSL_LIB is missing -- run ./setup.sh first." >&2
	exit 1
fi
if ! compgen -G "$BUILD_DIR/lwip_*.o" >/dev/null; then
	echo "error: lwIP objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
	exit 1
fi
if ! compgen -G "$BUILD_DIR/mbedtls_*.o" >/dev/null; then
	echo "error: mbedTLS objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
	exit 1
fi
if [ ! -f "$BUILD_DIR/cacert_data.o" ]; then
	echo "error: $BUILD_DIR/cacert_data.o is missing -- run ./setup.sh first." >&2
	exit 1
fi
if ! compgen -G "$BUILD_DIR/curl_*.o" >/dev/null; then
	echo "error: curl objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
	exit 1
fi
if [ ! -f "$BUILD_DIR/sqlite_sqlite3.o" ]; then
	echo "error: $BUILD_DIR/sqlite_sqlite3.o is missing -- run ./setup.sh first." >&2
	exit 1
fi
if ! compgen -G "$BUILD_DIR/sodium_*.o" >/dev/null; then
	echo "error: libsodium objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
	exit 1
fi
if ! compgen -G "$BUILD_DIR/lwext4_*.o" >/dev/null; then
	echo "error: lwext4 objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
	exit 1
fi
if ! compgen -G "$BUILD_DIR/python_*.o" >/dev/null; then
	echo "error: Python objects are missing from $BUILD_DIR -- run ./setup.sh first." >&2
	exit 1
fi

mkdir -p "$BUILD_DIR"

APP_SRCS=("$@")
APP_NAME="$(basename "${APP_SRCS[0]}" .cpp).app"

# Same freestanding ABI as build-app.sh's own CFLAGS (see that
# script's comment for what each flag is for) -- -fno-exceptions
# -fno-rtti added on top (see cxxabi_stub.cpp's header for the full
# reasoning: no unwinder exists anywhere in this port, c.ld /DISCARD/s
# .eh_frame* outright, so real C++ exceptions/dynamic_cast-across-TUs
# can't work here; matches Rust's panic=abort posture already used
# elsewhere in this port).
# Split from the musl -isystem itself (added separately below, in a
# different position for C vs C++ -- see CXXFLAGS's own comment on
# #include_next ordering for why).
CFLAGS_BASE="-c -m64 -O2 -nostdlib -nostartfiles -nodefaultlibs -ffreestanding -fno-pic -fno-pie -mcmodel=large -falign-functions=16 -fomit-frame-pointer -mno-red-zone -fno-builtin -fno-stack-protector -ffunction-sections -fdata-sections -nostdinc"
CFLAGS="$CFLAGS_BASE -isystem $MUSL_INC"
# -D__GLIBC_PREREQ(maj,min)=0: the multiarch C++ header directory
# (bits/os_defines.h) unconditionally calls __GLIBC_PREREQ() to gate a
# handful of glibc-version-specific tweaks (deprecated-gets shadowing,
# isinf/isnan-obsolete-decl suppression, float128 math, native-thread-
# id fast paths) -- all of it purely about *matching glibc's own
# declarations more precisely*, never something musl needs or provides
# an equivalent of. With -nostdinc, musl's own headers never define
# __GLIBC_PREREQ at all (there's no <features.h> equivalent pulled in
# here), so the bare macro name is left as an undefined identifier and
# `__GLIBC_PREREQ(2,15)` fails to parse as a expression at all (`0
# (2,15)` has no operator between them). Defining it as always-false
# is the correct answer either way: every one of those glibc-specific
# code paths should stay off against musl.
# -D__locale_t=locale_t: libstdc++'s bits/c++locale.h hard-codes
# `typedef __locale_t __c_locale;` -- glibc's own name for its opaque
# locale-object pointer type (bits/types/__locale_t.h). musl provides
# the exact same GNU extended-locale API (locale_t, newlocale,
# uselocale, freelocale, duplocale -- see musl's own locale.h) just
# under the single-underscore POSIX name, with no separate `__locale_t`
# alias. Since the two are the same opaque pointer type by contract,
# aliasing the identifier at the preprocessor level is exact, not an
# approximation.
# -U__STDC_HOSTED__ -D__STDC_HOSTED__=1: -ffreestanding (in $CFLAGS)
# sets __STDC_HOSTED__=0, which GCC 15's libstdc++ now checks directly
# (bits/c++config.h: `#define _GLIBCXX_HOSTED __STDC_HOSTED__`) to
# hard #error out of <string>/<iostream>/etc via bits/requires_hosted.h
# -- new in this GCC version, not something older libstdc++ releases
# gated this way. This port's C++ apps *are* "freestanding" in the
# sense build-app.sh's own CFLAGS comment means (no dynamic linker, no
# process model, flat binary at a fixed address) but very much
# "hosted" in the C++ standard's sense that matters here: a real,
# ABI-compatible libc (musl) and now (see glibc_ctype_shim.c/
# cxxabi_stub.cpp) enough of libstdc++'s own runtime expectations
# satisfied to make <string>/<iostream>/containers work. Overriding
# just this one macro re-enables those headers without touching any
# of -ffreestanding's other, still-wanted effects (see CFLAGS's own
# comment in build-app.sh).
CXXFLAGS_HOSTED="-U__STDC_HOSTED__ -D__STDC_HOSTED__=1"

# -include glibc_ctype_compat.h: see that file's own header -- forces
# the _ISupper/etc bit-mask constants bits/ctype_base.h expects into
# every C++ TU before that header (or anything that drags it in, like
# <iostream>) ever gets parsed.
# $CXX_INC1/2/3 (g++'s own C++ headers) come *before* $MUSL_INC here,
# not after like $CFLAGS's plain-C ordering above: libstdc++ headers
# like <cstdlib>/<cstdio>/<cmath> add their C++ wrapper on top of the
# real C header via `#include_next <stdlib.h>` etc, which resumes
# searching from the -isystem directory *after* the one the currently-
# open file (<cstdlib>, found in $CXX_INC1) was itself found in.
# $MUSL_INC has to be later in the list than $CXX_INC1 for that resumed
# search to ever reach it -- with $CFLAGS's own ordering (musl first),
# #include_next had nowhere left to search afterward and failed with
# "stdlib.h: No such file or directory" despite musl's own stdlib.h
# existing (confirmed by testing both orderings directly).
CXXFLAGS="$CFLAGS_BASE $CXXFLAGS_HOSTED -fno-exceptions -fno-rtti -fno-threadsafe-statics -nostdinc++ -isystem $CXX_INC1 -isystem $CXX_INC2 -isystem $CXX_INC3 -isystem $MUSL_INC -D__GLIBC_PREREQ(maj,min)=0 -D__locale_t=locale_t -I $PORT -include $CPP_PORT/glibc_ctype_compat.h"

LWIP_CFLAGS="$CFLAGS -I $LWIP_INC -I $LWIP_PORT"
MBEDTLS_CFLAGS="$CFLAGS -I $MBEDTLS_INC -I $MBEDTLS_PORT -DMBEDTLS_CONFIG_FILE=\"baremetal_mbedtls_config.h\""
LWEXT4_CFLAGS="$CFLAGS -I $LWEXT4_INC -I $LWEXT4_PORT -I $PORT -DCONFIG_USE_DEFAULT_CFG=0"

PYTHON_GCC_FREESTANDING_INC="$(gcc -print-file-name=include)"

# App-facing flags: same library -I's build-app.sh's own APP_CFLAGS
# exposes to a .c app, plus CXX_INC_ROOT (gcc's own freestanding
# <stdatomic.h> etc, same reasoning as build-app.sh's
# PYTHON_GCC_FREESTANDING_INC) and CPP_PORT for any C++-side port glue.
APP_CFLAGS="$CXXFLAGS -DCURL_STATICLIB -I $CURL_INC -I $SQLITE_INC -DSODIUM_STATIC -I $SODIUM_INC -I $MBEDTLS_INC -I $MBEDTLS_PORT -DMBEDTLS_CONFIG_FILE=\"baremetal_mbedtls_config.h\" -I $LWIP_INC -I $LWIP_PORT -isystem $PYTHON_GCC_FREESTANDING_INC -I $CPP_PORT"

echo "Building..."

gcc $CFLAGS -o "$BUILD_DIR/crt0.o" "$PORT/crt0.c"
gcc $CFLAGS -o "$BUILD_DIR/posix_shim.o" "$PORT/posix_shim.c"
gcc $CFLAGS -o "$BUILD_DIR/thread_shim.o" "$PORT/thread_shim.c"
gcc $LWEXT4_CFLAGS -o "$BUILD_DIR/ext4_shim.o" "$PORT/ext4_shim.c"
gcc $LWEXT4_CFLAGS -o "$BUILD_DIR/blockdev_baremetal.o" "$LWEXT4_PORT/blockdev_baremetal.c"
NET_GLUE_CFLAGS="$LWIP_CFLAGS"
if [ "$BAREMETAL_DEBUG" = "TRUE" ]; then
	NET_GLUE_CFLAGS="$NET_GLUE_CFLAGS -DBAREMETAL_DEBUG=1"
fi
gcc $NET_GLUE_CFLAGS -o "$BUILD_DIR/net_glue.o" "$PORT/net_glue.c"
gcc $LWIP_CFLAGS -o "$BUILD_DIR/net_shim.o" "$PORT/net_shim.c"
gcc $LWIP_CFLAGS -o "$BUILD_DIR/dns_shim.o" "$PORT/dns_shim.c"
gcc $MBEDTLS_CFLAGS -o "$BUILD_DIR/tls_shim.o" "$PORT/tls_shim.c"
gcc $MBEDTLS_CFLAGS -o "$BUILD_DIR/entropy_hardware_poll.o" "$MBEDTLS_PORT/entropy_hardware_poll.c"
gcc $CFLAGS -I "$SQLITE_INC" -o "$BUILD_DIR/sqlite_vfs.o" "$SQLITE_PORT/sqlite_vfs.c"
gcc $CFLAGS -I "$SODIUM_INC" -I "$SODIUM_INC/sodium" -o "$BUILD_DIR/randombytes_baremetal.o" "$SODIUM_PORT/randombytes_baremetal.c"
gcc $CFLAGS -o "$BUILD_DIR/dlfcn_shim.o" "$PORT/dlfcn_shim.c"
gcc $CFLAGS -o "$BUILD_DIR/libBareMetal.o" "$PORT/libBareMetal.c"

echo "Building C++ ABI stub..."
g++ $CXXFLAGS -I "$CPP_PORT" -o "$BUILD_DIR/cxxabi_stub.o" "$CPP_PORT/cxxabi_stub.cpp"
gcc $CFLAGS -I "$CPP_PORT" -o "$BUILD_DIR/glibc_ctype_shim.o" "$CPP_PORT/glibc_ctype_shim.c"
gcc $CFLAGS -I "$CPP_PORT" -o "$BUILD_DIR/fortify_shim.o" "$CPP_PORT/fortify_shim.c"
gcc $CFLAGS -I "$CPP_PORT" -o "$BUILD_DIR/libstdcxx_globals_shim.o" "$CPP_PORT/libstdcxx_globals_shim.c"

APP_OBJS=""
for src in "${APP_SRCS[@]}"; do
	obj="$BUILD_DIR/$(basename "$src" .cpp).o"
	g++ $APP_CFLAGS -o "$obj" "$src"
	APP_OBJS="$APP_OBJS $obj"
done

LWIP_OBJS=""
for obj in "$BUILD_DIR"/lwip_*.o; do
	LWIP_OBJS="$LWIP_OBJS $obj"
done

MBEDTLS_OBJS=""
for obj in "$BUILD_DIR"/mbedtls_*.o; do
	MBEDTLS_OBJS="$MBEDTLS_OBJS $obj"
done

CURL_OBJS=""
for obj in "$BUILD_DIR"/curl_*.o; do
	CURL_OBJS="$CURL_OBJS $obj"
done

SQLITE_OBJS="$BUILD_DIR/sqlite_sqlite3.o"

SODIUM_OBJS=""
for obj in "$BUILD_DIR"/sodium_*.o; do
	SODIUM_OBJS="$SODIUM_OBJS $obj"
done

LWEXT4_OBJS=""
for obj in "$BUILD_DIR"/lwext4_*.o; do
	LWEXT4_OBJS="$LWEXT4_OBJS $obj"
done

PYTHON_OBJS=""
for obj in "$BUILD_DIR"/python_*.o; do
	PYTHON_OBJS="$PYTHON_OBJS $obj"
done

echo "Linking..."

# Same two-stage link-then-objcopy shape as build-app.sh (see its own
# comment for why: c.ld's OUTPUT_FORMAT(binary) silently defeats
# --gc-sections, so link to an ELF intermediate first, then flatten).
#
# Link order matters here in one place build-app.sh doesn't have to
# worry about: cxxabi_stub.o and $APP_OBJS are listed *before*
# $LIBSTDCXX, so ld resolves std::__throw_*()/operator new/delete/etc
# against our own definitions first -- by the time it scans
# libstdc++.a looking for anything still undefined, those symbols are
# already satisfied, so the real (throwing) implementations in
# libstdc++.a never get pulled in at all (see cxxabi_stub.cpp's file
# header). $LIBSTDCXX itself comes before $MUSL_LIB/$LIBGCC, same
# relative order build-app.sh already uses for "our code, then the
# libc/compiler-support it depends on".
# --no-relax: libstdc++.a's wchar_t locale/time_get instantiations
# (cxx11-wlocale-inst.o) hit a GOTPCREL relocation ld's own linker
# relaxation can't safely convert under -mcmodel=large -- ld's own
# error message names this exact flag as the fix. C apps never
# instantiate this template graph at all (it's part of libstdc++'s
# locale machinery, not anything build-app.sh's C side pulls in), so
# this is scoped to the C++ build only rather than touched in c.ld or
# build-app.sh.
ld --gc-sections --no-warn-rwx-segments --no-relax --oformat elf64-x86-64 -T "$PORT/c.ld" -o "$BUILD_DIR/$APP_NAME.elf" "$BUILD_DIR/crt0.o" "$BUILD_DIR/posix_shim.o" "$BUILD_DIR/thread_shim.o" \
	"$BUILD_DIR/ext4_shim.o" "$BUILD_DIR/blockdev_baremetal.o" "$BUILD_DIR/net_glue.o" "$BUILD_DIR/net_shim.o" \
	"$BUILD_DIR/dns_shim.o" "$BUILD_DIR/tls_shim.o" "$BUILD_DIR/entropy_hardware_poll.o" "$BUILD_DIR/cacert_data.o" \
	"$BUILD_DIR/sqlite_vfs.o" "$BUILD_DIR/randombytes_baremetal.o" "$BUILD_DIR/dlfcn_shim.o" \
	"$BUILD_DIR/libBareMetal.o" "$BUILD_DIR/cxxabi_stub.o" "$BUILD_DIR/glibc_ctype_shim.o" "$BUILD_DIR/fortify_shim.o" "$BUILD_DIR/libstdcxx_globals_shim.o" $APP_OBJS $LWIP_OBJS $MBEDTLS_OBJS $CURL_OBJS $SQLITE_OBJS $SODIUM_OBJS $LWEXT4_OBJS $PYTHON_OBJS \
	"$LIBSTDCXX" "$MUSL_LIB" "$LIBGCC"
objcopy -O binary "$BUILD_DIR/$APP_NAME.elf" "$APP_NAME"

echo "Built $APP_NAME"
