# Rust support

BareMetal-AppPort builds Rust apps with full `std` (not a `no_std`
subset) against the same musl/lwIP/mbedTLS/lwext4 port the C and Python
sides already use. See `build-app.sh`'s own header for the C
side and `PYTHON.md` for the Python side; this document is the Rust
equivalent.

## Why this works at all

This port doesn't write its own libc -- it vendors stock musl 1.2.6 and
patches exactly one thing (`port/musl_port/musl-1.2.6-baremetal.patch`):
`arch/x86_64/syscall_arch.h`'s raw `syscall` instruction is replaced
with a call to `__bmos_syscall()` (`port/posix_shim.c`), keyed by the
same Linux `__NR_*` numbers musl already used. `clone.s` and
`__set_thread_area.s` get similarly targeted patches. Everything above
that syscall boundary -- file buffering, malloc, and musl's own
`pthread_mutex_t`/`pthread_cond_t`/TSD -- is unmodified upstream musl.

Rust's `std` for a `*-linux-musl` target is, underneath, just calls
into libc (via the `libc` crate) plus a handful of
`std::sys::pal::unix` assumptions. Since this port's musl is a real,
ABI-compatible musl (just re-plumbed at the syscall trap), reusing
Rust's existing `unix`/Linux platform backend -- rather than writing a
from-scratch `std::sys::pal` the way e.g. the Hermit unikernel target
does upstream -- turned out to be the right amount of work, and no
patch to Rust's own std source was needed at all (see "What actually
needed patching" below -- it's smaller than that).

## The target: `port/rust_port/x86_64-baremetal-firecracker.json`

Modeled on `x86_64-unknown-linux-musl`
(`rustc -Z unstable-options --print target-spec-json --target
x86_64-unknown-linux-musl`), with `os`/`env` left as `"linux"`/`"musl"`
so std picks its unix backend and the `libc` crate emits Linux-musl
bindings, and everything else matching `build-app.sh`'s own freestanding
ABI: `relocation-model: static`, `code-model: large`, `disable-redzone:
true`, `panic-strategy: abort`, no dynamic linking, no PIE -- the exact
same reasoning as `build-app.sh`'s `-fno-pic -fno-pie -mcmodel=large
-mno-red-zone`.

TLS's *addressing* works unmodified: `port/c.ld` already carves
per-symbol `.tdata`/`.tbss` output sections and thread-pointer setup
uses `wrfsbase` (Local-Exec model), which is exactly what a statically
linked, non-PIC Rust binary uses too. Getting a *thread's own copy* of
that template correctly initialized took one real fix -- see "The
PT_TLS fix" below.

## Build flow: `build-rust-app.sh`

Two stages, mirroring `build-app.sh`'s own two-stage (compile, then one
`ld -T port/c.ld` link) shape:

1. **`cargo +<pinned nightly> build -Z build-std=std,panic_abort -Z
   json-target-spec --target port/rust_port/x86_64-baremetal-firecracker.json
   --release`** compiles the app plus `std`/`core`/`alloc`/`panic_abort`
   from source (this target has no prebuilt std -- there's no
   crates.io-style download for it). The target's own `pre-link-args`
   (`-r`, a relocatable partial link) make rustc's own linker
   invocation collapse the *entire* crate graph into one relocatable
   `.o` instead of a final executable -- the Rust equivalent of
   `build-app.sh` compiling one `.c` file to one `.o`. `-u main` is in
   there too: without it, `ld`'s own `--gc-sections` (added
   unconditionally by rustc for this linker flavor, not something the
   target spec can suppress) discards `main` at this stage, since
   nothing inside the partial link itself calls it -- only `crt0.o`'s
   `__libc_start_main` does, in stage 2.

   This stage links against **stub** `libc.a`/`libgcc_s.a` (empty
   archives, `build/rust_stublibs/`), not the port's real musl. Rustc
   unconditionally wants `-lc`/`-lgcc_s` on this linker flavor; pointing
   it at the real musl here (rather than stage 2) pulls actual object
   contents into the merged `.o` and causes "multiple definition"
   errors once the real musl is linked for real in stage 2. Leaving
   every musl/libgcc symbol std needs (`open`, `read`, `malloc`,
   `pthread_create`, `__udivti3`, ...) unresolved here and resolving
   them all at once in stage 2 keeps one single point of truth for
   musl linking, same as the C side already has.

2. The same per-app shim compile + final `ld --gc-sections -T
   port/c.ld ...` link `build-app.sh` does, with the Rust object (plus
   `unwind_stub.o`, see below) standing in for `build-app.sh`'s
   `$APP_OBJS`.

## What actually needed patching

Building a `println!("hello")` binary against this target and stage-1
linking it left exactly **13 undefined symbols**: `__bmos_syscall` and
`b_system` (already provided by `posix_shim.o`/`libBareMetal.o`, no
work needed), plus 11 `_Unwind_*` symbols
(`_Unwind_Backtrace`, `_Unwind_GetIP`, ...) from std's backtrace
machinery, which normally comes from libgcc_s/libunwind -- neither of
which exists in this port (`c.ld` already discards `.eh_frame`
entirely; there are no unwind tables anywhere here, matching every app
being built with `panic = "abort"`).

**`port/rust_port/unwind_stub.c`** provides those 11 symbols as
link-time-only stubs that report "nothing found" rather than actually
walking anything. `std::backtrace`/panic-location printing degrades to
"unavailable" at runtime; it does not crash. This is the *only* patch
this port needed -- no changes to Rust's own std source were required,
despite the plan that led here expecting a small patch to
`std::rt`'s guard-page install and `std::process::Command`.

## The PT_TLS fix (`crt0.c`, `c.ld`)

`std::thread::spawn` used to panic during thread teardown --
`cannot access a Thread Local Storage value during or after
destruction: AccessError` (`std/src/thread/local.rs`), crashing the VM
outright (`Exception 0x13(GP)`) as often as not. Root cause, found by
patching a debug trace into std's own `sys/thread_local` source
(`-Z build-std` compiles it from source anyway, so this is just a
throwaway local edit to the toolchain's `rust-src` copy, not a
committed patch) and reading musl's `src/env/__init_tls.c`:

musl's `static_init_tls()` (called from `__libc_start_main`, for
*every* process, threaded or not) walks `aux[AT_PHDR]`/`aux[AT_PHNUM]`
looking for a `PT_TLS` program header to learn the executable's TLS
template -- its image address, its initialized (`.tdata`) size, its
total (`.tdata`+`.tbss`) size, and its alignment. `__copy_tls()` then
uses exactly that to `memcpy` the template into every thread's TLS
block, main thread included. `crt0.c`'s fabricated auxv never carried
`AT_PHDR` (there's no real program header table left once c.ld's
`OUTPUT_FORMAT(binary)` strips it), so that lookup silently found
nothing: `libc.tls_head` was never set, and `__copy_tls()`'s copy loop
never ran, for *any* thread. Any `#[thread_local]`/`__thread` variable
whose compiled-in initial value happens to be all-zero bytes
(`Option::None`, `0u64`, ...) looked correct anyway, purely because it
landed on already-zeroed storage (main thread: musl's own
`builtin_tls[]`, in `.bss`; a spawned thread: a fresh `mmap`, always
zero-filled -- see `posix_shim.c`'s `sys_mmap()`). One with a
*non-zero* initial value -- std's own internal `DTORS: RefCell<Vec<..>>`
thread-local (list.rs), whose empty-`Vec` sentinel pointer is a small
non-zero constant -- read garbage instead, corrupting that thread's own
destructor list. It only ever surfaced once a spawned thread actually
exited and that destructor list got walked; a single-threaded run never
touched the buggy path at all.

The fix fabricates one `PT_TLS` program header entry (`crt0.c`'s
`struct fake_phdr`/`tls_phdr`) pointing at `c.ld`'s own
`__tdata_start`/`__tdata_end`/`__tbss_end` symbols -- exact and known
at link time, since this image is neither PIE nor relocated -- and adds
`AT_PHDR`/`AT_PHENT`/`AT_PHNUM` to the fabricated auxv so
`static_init_tls()` finds it. No musl changes needed; it already does
the right thing once it has a `PT_TLS` entry to read. One `c.ld`
wrinkle along the way, worth knowing if this is ever touched again:
`ld`'s location counter does not advance past a `SHT_TLS` output
section's own size the way it does for every other section (`.tbss` is
`SHT_NOBITS` + `SHT_TLS`) -- a bare `symbol = .;` placed right after
`.tbss`'s closing brace lands at `.tbss`'s *start*, not its end, unless
you explicitly do `. = . + SIZEOF(.tbss);` first (`c.ld`'s own comment
there has the details).

## Verified so far

- `hello-rs/` (a `println!` app) builds via `./1-build.sh
  hello-rs/src/main.rs` and boots under `./2-run.sh`, printing to the
  console -- confirms `std::rt::init`/`lang_start`, stdio, and clean
  process exit all work.
- `std::fs::write`/`read_to_string`/`remove_file` against `disk.img`'s
  ext2 filesystem (via `ext4_shim.c`, the same path C's `fs_test.c`
  uses) work correctly end to end.
- `std::thread::spawn`/`join` + `Arc<Mutex<_>>` across 4 threads
  (1000 increments each, final count checked) works correctly end to
  end, including clean thread teardown -- see "The PT_TLS fix" above.

## Known gaps (do not attempt to "fix" these without re-reading
`OPENISSUES.md`'s "Process model" section first)

- **`std::process::Command`**: this port has no `fork`/`vfork`/`execve`
  (`OPENISSUES.md`'s "Process model" section) -- same limitation
  already true for C and Python's `os.fork`/`subprocess`. Expect a
  runtime error, not a link failure.
- **No real stack-overflow guard pages**: signals here are
  software-raised/checkpoint-delivered only (no hardware-fault
  `SIGSEGV`), so std's guard-page/`SIGSEGV`-handler stack-overflow
  detection can't provide real protection. Not yet confirmed whether
  the guard-page *install* itself (a `mmap`/`sigaltstack` call) fails
  outright or silently no-ops against `posix_shim.c`'s bump-allocator
  `mmap()` -- needs checking before relying on deep recursion failing
  cleanly.
- **`std::net`** (`TcpStream`/`TcpListener`) is architecturally
  expected to work the same way `std::fs` does (same "musl already
  speaks this ABI" reasoning, and C's own `net_test.c`/`tcp_test.c`
  already exercise `net_shim.c` from the same syscall surface) but has
  not been smoke-tested end to end yet -- do that before calling it
  supported.

## Toolchain

Pinned via `scripts/get-rust.sh` (parallel to `scripts/get-musl.sh`'s
`VERSION="1.2.6"`) -- `-Z build-std`/`-Z json-target-spec` are unstable
cargo/rustc surface that shifts between nightlies, so this port is
verified against one specific pinned nightly, recorded in
`port/rust_port/PINNED_TOOLCHAIN` after `get-rust.sh` runs. Bump the
pin deliberately, not automatically, and re-verify the "What actually
needed patching" section above still holds (a newer std may need more,
or fewer, stubs).
