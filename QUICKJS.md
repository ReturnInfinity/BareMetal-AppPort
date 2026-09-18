# QuickJS support

BareMetal-AppPort links quickjs-ng's core JS engine into the same
musl/lwIP/mbedTLS/lwext4 port the C/C++/Rust/Python/Zig sides already
use, following the same recipe as `SQLITE_CFLAGS`/`SODIUM_CFLAGS` in
`setup.sh`: bypass the library's own build system entirely (quickjs-ng
normally builds via CMake), hand-pick the four source files its own
`qjs_sources` CMake variable lists, and compile them flat with this
port's existing freestanding `CFLAGS`. See `build-app.sh`'s own header
for the C side and `ZIG.md`/`CPP.md`/`PYTHON.md` for the closest
equivalents; this is the QuickJS one.

This is step one of a longer-running "headless browser on BareMetal"
effort -- just the JS engine, embedded and evaluating scripts. There is
no DOM, no HTML/CSS parser (lexbor is next), no `fetch`, no
`document`/`window` globals: an app gets a bare `JSRuntime`/`JSContext`
and whatever globals it registers itself via `JS_NewCFunction`, same as
any other embedder.

## What's vendored

quickjs-ng 0.16.2 (`scripts/get-quickjs.sh`, the actively maintained
fork of bellard/quickjs, not the original), unmodified, as a plain
GitHub source tarball. Only the four files quickjs-ng's own
`CMakeLists.txt` puts in its `qjs_sources` library target are built:

- `quickjs.c` -- parser, bytecode compiler, interpreter, GC, builtins
- `libregexp.c` / `libunicode.c` -- regex engine and Unicode tables
  (`libunicode-table.h` ships pre-generated in the release tarball; no
  codegen step runs as part of this port's build)
- `dtoa.c` -- quickjs-ng's own float-to-string/string-to-float engine

Deliberately **not** built: `qjs.c`/`qjsc.c` (the CLI/bytecode-compiler
tools), `quickjs-libc.c` (the optional POSIX file/os/std module --
`js_std_*`/`js_os_*`, console/print helpers, the module loader). The
DOM/fetch bindings a real headless browser needs will be hand-written
against this port's own `posix_shim.c`/`net_shim.c`/`tls_shim.c`
later, not against quickjs-libc's own (Linux-`FILE*`-based) idea of
what a host environment provides.

No `port/quickjs_port/` config-header stub was needed -- unlike
SQLite/libsodium, quickjs-ng's core needs no build-generated config.h
at all; every knob that matters here is a plain `-D` define (see
"Why this works" below).

## Why this works

Two compiler defines in `setup.sh`'s `QUICKJS_CFLAGS`, both landing on
existing fallback paths quickjs-ng already carries for other platforms/
compilers, rather than anything specific to this port:

- **`-D_GNU_SOURCE`** -- matches quickjs-ng's own CMake build
  unconditionally passing this; nothing here specifically requires it,
  but it costs nothing and keeps this port's headers looking the same
  as upstream's own build sees them.
- **`-DGCC_BUILTIN_ATOMICS`** -- forces `quickjs-c-atomics.h`'s
  `__atomic_*()`-builtin code path (only the JS `Atomics.*` opcodes use
  this, nothing load-bearing at startup) instead of its `#include
  <stdatomic.h>` branch. musl 1.2.6 (this port's vendored version)
  doesn't ship a `stdatomic.h` at all -- later musl releases do -- and
  `quickjs-c-atomics.h` already carries this exact macro as its own
  fallback for pre-4.9 GCC; defining it manually sidesteps needing a
  header this musl doesn't have, using a path upstream already
  maintains for a different reason.

Everything else "just works" because of how `gcc` and musl happen to
line up here, verified against quickjs-ng 0.16.2's actual source before
assuming it, not after debugging a crash:

- **`js__malloc_usable_size()`** (`cutils.h`) is gated on
  `defined(__linux__) || ...`. `gcc` predefines `__linux__`
  unconditionally based on its target triple -- `-ffreestanding`/
  `-nostdlib`/etc. don't change that, they're compiler flags, not
  target-triple macros -- so this resolves to a real call to
  `malloc_usable_size()`. musl 1.2.6 does implement and export that
  symbol (a glibc-compat addition, confirmed via `nm` on the built
  `libc.a`), so this needs no stubbing, unlike a true bare-metal target
  that would fall through to the `js_malloc_usable_size_unknown`-style
  "0" path.
- **No NaN-boxing pointer truncation risk.** `quickjs.h`'s
  `JS_NAN_BOXING` only auto-enables `#if INTPTR_MAX < INT64_MAX` (32-bit
  builds). This port is `-m64`, so `INTPTR_MAX == INT64_MAX` and
  quickjs-ng uses its normal tagged-`JSValue`-struct representation
  (a real 64-bit pointer plus a separate tag word) instead -- this
  port's high-canonical load address (`0xFFFF8000...`, see `c.ld`)
  never gets packed into spare double-mantissa bits that would assume a
  low, zero-extended userspace address on unpacking. Worth stating
  explicitly since it's exactly the kind of thing that would otherwise
  surface as a baffling, non-deterministic GC crash instead of a build
  failure.
- **No pthreads, no signals, no `mmap`/`mprotect`, no `dlopen`.** None
  of `quickjs.c`/`libregexp.c`/`libunicode.c`/`dtoa.c` reference any of
  these (checked directly against the vendored source, not assumed from
  general JS-engine folklore) -- the engine itself is a pure bytecode
  interpreter with no JIT and no background threads; only
  `quickjs-libc.c` (not built here) would pull those in.

## Boot-testing note: MEMSIZE

`examples/quickjs/hello/hello.c` needed `BareMetal-Firecracker`'s
`baremetal.sh` `MEMSIZE` bumped well past its 4MiB default to boot at
all (the compiled engine plus Unicode tables alone push the flat
binary past what 4MiB leaves room for once boot/loader overhead is
subtracted) -- same story as `PYTHON.md`'s own MEMSIZE note. Bump it
per-app as needed; not something this port's build controls.

## Example

`examples/quickjs/hello/hello.c` -- `JS_NewRuntime`/`JS_NewContext`,
`JS_Eval` a small script (string concatenation, arithmetic, a plain
function call), print the result via this port's normal
`printf`/`posix_shim.c` stdout path. Verified booting for real,
repeatedly, through the actual `build-app.sh` pipeline:

```
QuickJS says: Hello, BareMetal! 2+2=4
```

## Not yet done

- Exception handling shown (`JS_IsException`/`JS_GetException`) but not
  exercised end-to-end with a script that actually throws.
- No `JS_NewRuntime2`/custom allocator wired to this port's own heap
  arena -- uses quickjs-ng's own default libc-`malloc`-backed allocator
  as-is.
- The whole DOM/HTML/CSS/fetch layer -- see this repo's broader
  headless-browser plan; lexbor is next.
