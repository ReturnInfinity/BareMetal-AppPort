# C++ support

BareMetal-AppPort builds C++ apps against the same musl/lwIP/mbedTLS/
lwext4 port the C and Python sides already use, reusing the *host's
own* `g++`/libstdc++.a (whatever GCC this machine has installed) as
both the compiler and the C++ standard library -- no cross-compiler,
no rebuilding libstdc++ from source. See `build-app.sh`'s own header
for the C side and `RUST.md`/`PYTHON.md` for the Rust/Python
equivalents; this document is the C++ one.

## Why this works at all -- and why it's a harder fit than Rust

`build-app.sh` already established the core trick this port runs on:
use the host's compiler purely as a *freestanding code generator*
(`-ffreestanding -nostdlib -fno-pic -fno-pie -mcmodel=large
-mno-red-zone`, `-nostdinc` plus `-isystem` pointed at musl's own
headers), then link the resulting objects directly against this
port's own patched musl `libc.a` instead of glibc. Every C library
call an app or a vendored library makes resolves, by *name*, against
musl -- the host compiler never actually touches glibc at any point.

C++ needs one more piece C doesn't: libstdc++ itself, for `new`/
`delete`, `std::string`/`vector`/etc.'s out-of-line pieces, iostream,
locale, and the C++ exception-handling ABI. Unlike Rust's `std` (built
from source per-target via `-Z build-std`, so it's *compiled fresh*
against this port's real musl -- see `RUST.md`), the host's
`libstdc++.a` is a **precompiled binary archive**, built by the
distro against glibc, with real exception unwinding compiled in
throughout (there is no `-fno-exceptions` variant of it). That
mismatch -- a glibc-built static archive linked against musl instead,
with no unwinder anywhere in this port (`port/c.ld` `/DISCARD/`s
`.eh_frame*` outright, same as the C and Rust sides) -- is where
everything below comes from. Nothing here needed patching libstdc++'s
own source; every fix is either a link-time override (an object file
listed ahead of `libstdc++.a` on the link line, so archive resolution
never pulls in the real, incompatible implementation) or a small
compatibility shim providing a real glibc-only runtime symbol musl
has no equivalent of.

## Build flow: `build-cpp-app.sh`

Structurally the same shape as `build-app.sh` (same per-app shim
objects, same musl/lwIP/mbedTLS/curl/SQLite/libsodium/lwext4/Python
object reuse from `setup.sh`'s output, same two-stage
link-then-`objcopy` pattern -- see that script's own comments for why
each piece is there). Usage: `./build-cpp-app.sh yourapp.cpp
[otherfile.cpp ...]` (multi-file, like the C side; unlike Rust's
single-crate model) -> `yourapp.app`.

App code (and `port/cpp_port/cxxabi_stub.cpp` itself) builds with the
same freestanding CFLAGS as C, plus `-fno-exceptions -fno-rtti
-fno-threadsafe-statics` -- real exceptions can't work here (see
above), so `try`/`throw`/`catch` are compile errors in app code, the
same `panic=abort` posture Rust already takes. `-nostdinc++` plus
`-isystem` pointed at the host's own C++ headers (found via `g++
-print-file-name=...`/`-dumpversion` rather than hard-coded, so this
keeps working across whatever GCC is actually installed) adds C++ on
top of the same musl C headers -- **order matters**: the C++ include
dirs have to come *before* musl's in the `-isystem` list, not after,
because headers like `<cstdlib>` do `#include_next <stdlib.h>` to
reach the real C header underneath, and `#include_next` resumes
searching from the directory *after* wherever the current file
(`<cstdlib>`) was found -- musl's `stdlib.h` has to still be ahead in
that resumed search, which only happens if it's listed later on the
command line.

Link order matters too: `port/cpp_port/cxxabi_stub.o` (and the app's
own objects) are listed *before* `libstdc++.a`, so `ld` resolves
`std::__throw_*()`/`operator new`/`delete`/etc against this port's own
overrides first -- by the time `ld` scans `libstdc++.a` looking for
anything still undefined, those symbols are already satisfied, so the
real (throwing) implementations never get linked in at all. Static
archive members are only pulled in to resolve a symbol still undefined
when `ld` reaches them; a plain `.o` listed directly on the command
line (not inside an archive) is unconditionally linked in its
entirety, which is exactly the lever this whole approach depends on.

## What actually needed patching or shimming

Six real gaps, found the same empirical way this port's other
comments describe ("confirming linker behavior by testing it rather
than reasoning about it in the abstract") -- building a real app,
reading the actual compiler/linker error, fixing exactly that:

1. **`-D__STDC_HOSTED__=1`.** `-ffreestanding` sets
   `__STDC_HOSTED__=0`, which this GCC's libstdc++
   (`bits/c++config.h`: `#define _GLIBCXX_HOSTED __STDC_HOSTED__`) now
   checks directly to hard `#error` out of `<string>`/`<iostream>`/etc
   (`bits/requires_hosted.h`) -- new in this GCC version, not
   something older libstdc++ releases gated this way. Overriding just
   this one macro re-enables those headers without touching any of
   `-ffreestanding`'s other, still-wanted effects.

2. **`-D__GLIBC_PREREQ(maj,min)=0` and `-D__locale_t=locale_t`.** The
   multiarch C++ header directory (`bits/os_defines.h`,
   `bits/c++locale.h`) assumes glibc's own version-gating macro and
   its `__locale_t` typedef exist. musl provides the exact same GNU
   extended-locale API (`locale_t`, `newlocale`, `uselocale`,
   `freelocale`) under the single-underscore POSIX name, with no
   separate `__locale_t` alias -- aliasing the identifier is exact,
   not an approximation, and `__GLIBC_PREREQ` always-false is correct
   either way (every code path it gates is glibc-version-specific
   behavior musl has no reason to match).

3. **`port/cpp_port/glibc_ctype_compat.h` + `glibc_ctype_shim.c`.**
   libstdc++'s `ctype_base::mask` values are hard-coded as glibc's own
   internal `_ISupper`/`_ISlower`/etc classification bits, and
   libstdc++.a's own precompiled `ctype<char>` code calls glibc's
   `__ctype_b_loc()`/`__ctype_tolower_loc()`/`__ctype_toupper_loc()`
   directly to fetch the classification/case-conversion tables behind
   *every* `std::ctype<char>` call -- including what `std::cout`/`cin`
   touch on every character. musl has no equivalent at all (its own
   `isupper()`/etc are plain functions, not a shared exported table).
   The `_IS*` bit values are glibc's real, stable, publicly documented
   ABI (`_ISbit()`), reproduced exactly in the compat header; the shim
   builds real 384-entry tables from musl's own `isupper()`/`tolower()`
   /etc, correct for every byte value under the "C" locale (see
   `OPENISSUES.md`'s C++ section for the real limitation this leaves:
   there's no other locale to be correct *under*).

4. **`port/cpp_port/fortify_shim.c`.** Ubuntu's `libstdc++.a` was
   built with glibc's `_FORTIFY_SOURCE=2` hardening (the distro
   default), so its precompiled object code calls fortified
   `*_chk()` variants of `sprintf`/`memcpy`/`strcpy`/etc directly.
   Those are normally implemented *inside* glibc itself; musl only
   ever expands them to a plain call at the header level, never
   exports the symbols. Each shim here does the real bounds-checked
   thing using the extra "destination size" argument every call site
   passes, against musl -- not a no-op passthrough. Two more real
   glibc-only surfaces landed in the same file once `<iostream>`
   pulled in libstdc++'s wide-character locale machinery (see point 6
   below): `ftello64()`/`fseeko64()` (glibc's explicit-64-bit-off_t
   names -- musl's `off_t` is always 64-bit, so aliasing straight to
   `ftello()`/`fseeko()` is exact) and `__wmemset_chk()`/
   `__mbsrtowcs_chk()` (the wide-char flavor of the same
   `_FORTIFY_SOURCE` pattern).

5. **`port/cpp_port/libstdcxx_globals_shim.c`.** Two plain globals
   libstdc++.a's precompiled code references directly:
   `__dso_handle` (normally crtbegin.o's job; this port links no
   crtbegin/crtend at all, and any single consistent address is
   correct given there's no `dlclose()`/module-unload concept here to
   begin with) and `__libc_single_threaded` (a real glibc >= 2.32
   global `ios_base::Init`/`std::locale` check to skip atomic-refcount
   overhead single-threaded -- hard-coded `false` here since this port
   *does* have real threads, `thread_shim.c`'s `pthread_create`, and
   nothing updates this dynamically the way glibc's own
   `pthread_create` does; `false` is the only value that's safe in
   every case).

6. **A real bug in `port/c.ld`, not a C++-only shim.** This is the one
   that actually took the longest to find, and it isn't specific to
   C++ at all. See "The `.init_array` bug" below.

`port/cpp_port/cxxabi_stub.cpp` collects the rest -- see its own file
header for the complete reasoning, condensed here:

- **`operator new`/`operator new[]`/`operator delete`/`operator
  delete[]`** (throwing, `nothrow_t`, and sized-delete forms) route
  straight through musl's `malloc`/`free`, never through libsupc++'s
  own default (which calls `std::get_new_handler()`/throws
  `std::bad_alloc` on failure -- pulling in the exact throw path this
  whole file exists to avoid, from a plain `new` expression).
- **Every `std::__throw_*()` helper** (`bits/functexcept.h`'s
  `__throw_out_of_range`, `__throw_length_error`, `__throw_bad_alloc`,
  ...) is overridden to report the failure via `b_output()` then halt
  via `b_exit()` -- a hard abort, not a real exception; each is
  documented `[[noreturn]]` in libstdc++ itself, so this is a legal
  (if fatal) implementation. This is the layer that intercepts the
  overwhelming majority of real, reachable throw sites
  (`vector::at()`, `string` length checks, allocation failure) without
  needing to fake exception *propagation* at all.
- **`__cxa_pure_virtual`/`__cxa_deleted_virtual`** abort the same way.
- **A last-resort `_Unwind_*` safety net** (modeled directly on
  `port/rust_port/unwind_stub.c`), for anything that reaches a real
  unwind entry point despite the `__throw_*()` overrides above.
- **Deliberately NOT overridden**: `__cxa_throw`/`__cxa_begin_catch`/
  `__cxa_end_catch`/`__gxx_personality_v0`/`std::__throw_system_error`.
  These live inside libstdc++.a's own `eh_throw.o`/`eh_catch.o`/
  `eh_personality.o`/`system_error.o`, and `<ios>`'s own machinery
  (`ios_base::failure` derives from `system_error`) pulls at least one
  of those `.o`s in for real, for *other* symbols inside them,
  regardless of whether an app ever throws. Once `ld` pulls a whole
  `.o` in from an archive for symbol A, every other symbol that same
  `.o` defines becomes a real, strong definition too -- overriding
  these as well produced a hard "multiple definition" link error, not
  a harmless shadow. Leaving them alone lets the real ones link in
  instead; confirmed dead code in every actually-reachable path, since
  those are already intercepted earlier by the `__throw_*()` layer.

## The `.init_array` bug (`port/c.ld`)

`std::cout << "x"` compiled and linked cleanly but segfaulted on the
very first use, reading through a null pointer at a small negative
offset (`this - 24`, matching `basic_ostream::sentry`'s constructor
reading a vbase-offset entry from `std::cout`'s vtable pointer --
which was zero). Root cause, found by disassembling the crash site and
walking backward from there: **`std::cout`'s real constructor never
ran.**

`std::cout`/`cin`/`cerr` (and `wcout`/`wcin`/`wcerr`, constructed
alongside them unconditionally) are placement-constructed by
`ios_base::Init::Init()`, itself called from a per-translation-unit
`.init_array` entry libstdc++.a's own `globals_io.o` carries
(`_GLOBAL__sub_I.00090_globals_io.cc`). `port/c.ld`'s original
`.init_array` rule --

```
.init_array : {
	__init_array_start = .;
	KEEP(*(.init_array .ctors))
	__init_array_end = .;
}
```

-- only matches input sections named exactly `.init_array`/`.ctors`.
With `-ffunction-sections` (every `build-*-app.sh`'s CFLAGS), gcc
doesn't always emit a plain `.init_array` for a constructor -- it
names it `.init_array.NNNNN` instead (`NNNNN` encoding init priority),
and `globals_io.o`'s own copy is exactly one of these
(`.init_array.00090`, confirmed via `objdump -h`). That's the *exact*
same class of bug `c.ld`'s own comment already documents for
`.tdata.NAME`/`.tbss.NAME` -- just never hit before, because nothing
prior to the C++ port relied on a *library-provided* (not app-local)
global constructor. The unmatched `.init_array.00090` became an
orphan section, placed by `ld` outside `[__init_array_start,
__init_array_end)`, so `musl`'s own `libc_start_init()` -- which
walks exactly that range (`src/env/__libc_start_main.c`) -- never
called it. `std::cout`'s storage stayed zeroed `.bss`, vtable pointer
included.

Fixed by widening the glob to match the suffixed form too, the same
way `.tdata`/`.tbss`/`.bss` already do, with `SORT_BY_INIT_PRIORITY`
on the suffixed part (matching the standard GNU ld default linker
script's own `.init_array` rule -- unlike `.tdata.*`/`.tbss.*`, the
`NNNNN` suffix *is* an explicit ordering, so an unsorted glob would
link fine but could silently run constructors in the wrong relative
order once more than one exists):

```
.init_array : {
	__init_array_start = .;
	KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
	KEEP(*(.init_array .ctors))
	__init_array_end = .;
}
```

(`.fini_array` gets the matching fix, for symmetry -- nothing
currently populates it, real or suffixed, but the next thing that
does shouldn't hit this again.) This is a real, general fix to a file
every language here links against, not a C++-only patch -- it just
took a library-provided global constructor to expose it.

## Verified so far

All confirmed by an actual `./1-build.sh`-built unikernel booted under
Firecracker (`./baremetal.sh start` / `output --full`), not just a
successful compile:

- **Classes, virtual dispatch, `new`/`delete`.** A class with a pure
  virtual method, a derived override, heap allocation and
  destruction through a base pointer -- boots and exits cleanly.
- **`std::cout << ... << std::endl`** (`examples/cpp/hello/hello.cpp`)
  -- prints correctly, including through the full `std::string`
  concatenation path (`std::string s = "Hello"; s += ", ...";`).
- **`std::vector`**: `push_back`, range-`for` iteration, `.at()` --
  all correct (`sum of squares 0..9 = 285`, `vector[3] = 9`).
- **`std::vector::at()` out of range aborts safely**: calling
  `v.at(100)` on a 10-element vector prints `cxxabi_stub:
  std::out_of_range` and halts, rather than crashing or attempting a
  real unwind -- confirms the `__throw_*()` override layer actually
  intercepts a real, reachable throw site end to end.
- **Output buffering behaves like real libstdc++.a**: `std::cout <<
  "x"` with no explicit flush shows up at process-exit buffer flush
  (after `main()` returns, during global destruction), not
  immediately -- confirmed by comparing against the same call
  followed by `std::endl`, which flushes immediately. Not a bug; this
  is exactly what unbuffered-vs-`std::endl` behavior looks like on a
  real Linux box too.

## Known gaps

See `OPENISSUES.md`'s C++ section for the maintained list (real
exceptions, locale always "C"/POSIX, `std::thread`/`std::filesystem`/
`std::regex`/wide-stream output unverified).

## Toolchain

No pinned version, unlike Rust's nightly (`scripts/get-rust.sh`) --
this reuses whatever `g++`/libstdc++ the host already has installed
(verified against Ubuntu's GCC 15.2.0). `build-cpp-app.sh` resolves
every version-specific path (`libstdc++.a`'s location, the C++ header
directories, the GCC major version for the multiarch header path)
via `g++ -print-file-name=...`/`-dumpversion` rather than hard-coding
them, but the specific shims in `port/cpp_port/` were only verified
against this one GCC version -- a different major version's libstdc++
may need more, or fewer, of them (the exact same caveat `RUST.md`
raises about bumping its own pinned nightly).
