# lexbor support

BareMetal-AppPort links lexbor's HTML5 parser, DOM tree, and CSS-
selector engine into the same musl/lwIP/mbedTLS/lwext4 port the C/C++/
Rust/Python/Zig/QuickJS sides already use, following the same recipe as
`SQLITE_CFLAGS`/`SODIUM_CFLAGS`/`QUICKJS_CFLAGS` in `setup.sh`: bypass
the library's own build system entirely (lexbor normally builds via
CMake), hand-pick which module directories to build, and compile every
`.c` file under them flat with this port's existing freestanding
`CFLAGS`.

This is step two of the "headless browser on BareMetal" effort (see
QUICKJS.md for step one). There is still no `fetch`, no `document`/
`window` JS globals, and no CSS layout/rendering: an app gets a real
lexbor DOM it built by parsing an HTML string, and can run CSS
selectors against it, same as any other lexbor embedder. The DOM<->
QuickJS binding layer is next.

## What's vendored

lexbor 3.0.0 (`scripts/get-lexbor.sh`), unmodified, as a plain GitHub
source tarball. Only these module directories under `source/lexbor/`
are built (every `.c` file in each, recursively -- lexbor's own CMake
globs the same way via its `GET_MODULE_RESURSES` macro, there's no
curated per-file list to keep in sync, same posture as mbedTLS/curl/
lwext4 in `setup.sh` already):

- `core` -- base utilities: memory arenas (`mraw`/`dobject`), hash
  tables, its own `dtoa`/`strtod`, string helpers
- `tag` / `ns` -- element tag name / namespace lookup tables the DOM
  needs
- `dom` -- the DOM tree itself (nodes, elements, every DOM interface
  type: text, comment, document-fragment, ...)
- `html` -- the real HTML5 tokenizer + tree-construction parser
  (insertion modes, every HTML element interface, `<template>`
  handling, foreign content, the works)
- `css` -- CSS tokenizer/parser (property/value/at-rule parsing) and
  its own `css/selectors` submodule (selector syntax parsing)
- `selectors` -- matches parsed CSS selectors against a DOM tree
  (`lxb_selectors_find`)

Plus two files from lexbor's own POSIX platform-abstraction layer,
`source/lexbor/ports/posix/lexbor/core/`:

- `memory.c` -- a thin indirection over `malloc`/`realloc`/`calloc`/
  `free` (lets an embedder swap allocators later; unused here, this
  port just takes the default)
- `perf.c` -- rdtsc-based timing helpers, compiled to an always-
  returns-0 stub here since `LEXBOR_WITH_PERF` is never defined (this
  port's `CFLAGS` don't set it, and nothing calls the timing API)

Deliberately **not** built:

- `fs.c` (the third file in that same `ports/posix` directory) --
  `opendir`/`readdir`/`stat`-based directory listing and whole-file
  reads. Confirmed by grep, not assumed: nothing in `core`/`tag`/`ns`/
  `dom`/`html`/`css`/`selectors` calls any `lexbor_fs_*` function
  outside `fs.h`'s own declaration -- it's an app-level convenience
  helper the parser pipeline never reaches. This port's `posix_shim.c`
  doesn't implement `opendir`/`readdir` anyway (see OPENISSUES.md), so
  this is a non-issue rather than a deferred gap.
- `encoding` (real-world `<meta charset>`/BOM detection -- this scope
  only ever feeds lexbor already-decoded UTF-8 strings; nothing in
  `html`'s own sources references `lexbor/encoding` either, confirmed
  by grep)
- `url` (relative-URL resolution) and its own dependencies `unicode`/
  `punycode` -- no fetch layer exists yet to need URL resolution at all
- `style` / `engine` -- CSS cascade/computed-style and a convenience
  wrapper API respectively. Layout/rendering is out of scope for a
  DOM+JS headless browser (see QUICKJS.md's framing: no visual box
  model, no canvas, ever) -- `style` would only matter for that.

None of these are hard blockers -- each is a "didn't need it for this
scope" call, verified by grepping the actual dependency graph rather
than assumed, and each can be added back independently later (e.g.
`encoding` the day a real fetched page shows up with a non-UTF-8
charset).

No `port/lexbor_port/` directory exists -- like QuickJS, lexbor's core
needed no build-generated config header at all here; the two knobs that
matter are a single `-D` define (`LEXBOR_STATIC`) and one include path.

## Why this works

- **`-DLEXBOR_STATIC`** -- makes `source/lexbor/core/def.h`'s `LXB_API`
  macro expand to nothing instead of
  `__attribute__((visibility("default")))`. Harmless either way on
  this non-Windows, one-flat-static-binary target (there's no `.so`
  for a visibility attribute to affect), but matches `SODIUM_STATIC`'s
  posture: tell every vendored library here "no shared library, no
  export decorations needed."
- **`-I $LEXBOR_SRC`** (the `source/` directory, not `source/lexbor/`)
  -- matches lexbor's own `CMakeLists.txt`
  `include_directories(${LEXBOR_DIR_HEADER})` call: every file quote-
  includes its own headers as `"lexbor/core/mraw.h"` etc, relative to
  `source/`.
- **No thread/mutex source files exist in this release at all.**
  `LEXBOR_WITHOUT_THREADS` defaults `ON` upstream too ("Not used now,
  for the future", per lexbor's own `CMakeLists.txt` comment) --
  confirmed by `find`, not just trusting the CMake option: there is no
  `core/thread.c` or equivalent anywhere in the 3.0.0 tree to disable
  in the first place.
- **No build-time codegen step.** lexbor's tag/entity/CSS-property
  tables (`source/lexbor/{tag,ns,html,css}/*_res.h`, `html/tag_res.h`,
  etc) are pre-generated and checked into the release tarball;
  `utils/lexbor/*.py` are maintainer-only regeneration scripts
  (`LEXBOR_BUILD_UTILS`, default off) this port's build never invokes
  -- same story as quickjs-ng's `libunicode-table.h`.
- **Everything else compiled clean on the first real attempt** against
  this port's existing freestanding `CFLAGS` (`-ffreestanding
  -nostdlib -mcmodel=large -mno-red-zone -fno-builtin
  -fno-stack-protector`, no libc beyond musl's headers/`libc.a`) --
  189 object files, zero errors, no shims needed beyond the two knobs
  above. Unlike QuickJS's writeup, there was no landmine worth a
  dedicated callout here to rule out; the built-in-only, no-thread,
  no-network shape of these particular modules just happens to line up
  with what this port already provides.

## Boot-testing note: MEMSIZE

Same story as `PYTHON.md`/`QUICKJS.md`: `examples/lexbor/hello/hello.c`
needed `BareMetal-Firecracker`'s `baremetal.sh` `MEMSIZE` bumped well
past its 4MiB default to boot at all (the compiled parser/DOM/CSS
tables push the flat binary well past what 4MiB leaves room for). Bump
it per-app as needed; not something this port's build controls.

## Example

`examples/lexbor/hello/hello.c` -- adapted from lexbor's own
`examples/lexbor/selectors/easy_way.c`: parses a small in-memory HTML
string into a real lexbor DOM, parses a CSS selector (`p.greeting`),
runs `lxb_selectors_find` against the DOM, and serializes each match
via `lxb_html_serialize_cb`, printed through this port's normal
`printf`/`posix_shim.c` stdout path (same mechanism `QUICKJS.md`'s
example already used). Verified booting for real, repeatedly, through
the actual `build-app.sh` pipeline:

```
Lexbor match 1: <p class="greeting">
Lexbor says: 1 match(es)
```

## Fetch + parse example

`examples/lexbor/fetch/fetch.c` -- the first real fetch-then-parse
pipeline: libcurl (same pattern as the repo-root `curltest.c`: same
CA-bundle handling, same fixed-size `write_cb` buffer, same
`https://example.com/` target, kept identical on purpose so this
example's expected output stays predictable) does a real HTTPS GET
into an in-memory buffer, then that buffer is handed straight to
`lxb_html_document_parse` -- no intermediate copy, no assumption the
fetched bytes are a null-terminated C string. Two small real DOM
queries follow: `lxb_html_document_title()` for the `<title>` text, and
a `lxb_selectors_find()` over the CSS selector `a` (same API
`examples/lexbor/hello/hello.c` already used) to count anchor tags.

No new `setup.sh`/`build-app.sh` wiring was needed -- curl and lexbor
are both already linked into every app unconditionally, confirming
that assumption held. Networking itself needed no extra setup either:
this port's `net_glue.c` falls back to DHCP when the Firecracker `ip=`
kernel param is absent, and `baremetal.sh start` already auto-attaches
`tap0` as the guest NIC when it exists on the host -- nothing app- or
port-side to configure beyond the usual `MEMSIZE` bump.

Verified booting for real, twice, through the actual `build-app.sh` /
`BareMetal-Firecracker` pipeline:

```
status: 200
body: 559 byte(s) kept (RESPONSE_BUF_SIZE cap)

title: Example Domain
<a> tag count: 1
```

## Not yet done

- No DOM<->QuickJS binding layer yet -- lexbor and QuickJS are each
  linked into every app already, but nothing connects them (no
  `document`/`window` JS globals, no running `<script>` tags found
  during parsing). That's the next phase.
- `encoding`/`url` modules not built (see "What's vendored" above) --
  follow-ups once a real fetched page needs charset detection or
  relative-URL resolution, not blockers. `example.com`'s page is plain
  ASCII/UTF-8 with no relative URLs, so the fetch example above never
  exercises either gap.
- `fs.c` intentionally never built -- if some future module actually
  needs `lexbor_fs_*` (unlikely for the DOM+JS scope this project is
  aiming for), it would first need `opendir`/`readdir`/`stat` added to
  `posix_shim.c` (see OPENISSUES.md), a real gap, not just an unbuilt
  file.
