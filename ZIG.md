# Zig support

BareMetal-AppPort builds Zig apps against the same musl/lwIP/mbedTLS/
lwext4 port the C, Rust, C++, and Python sides already use. See
`build-app.sh`'s own header for the C side and `RUST.md`/`CPP.md` for
the closest equivalents; this document is the Zig one.

Unlike Rust (which recompiles `std` from source per-target) or C++
(which reuses the host's own precompiled `libstdc++.a`), Zig's own
compiler cross-compiles natively to `x86_64-linux-musl` with no extra
toolchain step -- `zig build-obj` already produces a single relocatable
object ready to feed straight into `build-app.sh`'s own final `ld -T
c.ld` link, simpler than either. The catch, covered in full below, is
that a meaningful slice of Zig's standard library bypasses libc
entirely for a few specific primitives, in a way this port has no way
to intercept -- **this port's Zig support is deliberately restricted to
avoid that slice**, not a fix for it. Read "Known gaps" before writing
anything beyond `println`-style output.

## Why this (mostly) works

Zig's own `lib/std/os/linux.zig` implements Linux syscalls itself,
independent of whether libc is linked -- but only some of it always
takes that path. Ordinary file I/O and most of `std.c`/`std.posix`
(`open`/`read`/`write`/`lseek`/`stat`/heap allocation via libc
`malloc`/...) compile down to real *calls* into libc symbols when
`-target x86_64-linux-musl -lc` is passed -- confirmed via `nm`/
`objdump` on the resulting object (`writev`/`pwritev`/`lseek`/
`__errno_location` show up as undefined symbols, no inline syscalls at
all) and a real boot. Since this port's musl is a real, ABI-compatible
musl re-plumbed only at the syscall trap (`__bmos_syscall()`, see
`port/musl_port/`'s patch -- the same fact `RUST.md`'s "Why this works
at all" section explains for Rust's `libc` crate), that's exactly the
interception point this port already has, and it catches everything
routed through it.

## The entry point: no Zig-idiomatic `main`

This port supplies its own `crt0.c`/`_start` -- the same one every
other language here uses -- not Zig's own `lib/std/start.zig`. A Zig
app on this port must therefore export a plain C-ABI `main` itself,
the same signature a C app on this port already writes:

```zig
export fn main(argc: c_int, argv: [*c][*c]u8, envp: [*c][*c]u8) callconv(.c) c_int {
    ...
    return 0;
}
```

There is no wrapper generating this for you (unlike Python's/Lua's
`python.c`/`lua.c`, which exist because upstream's own `main` wants a
real argv/interactive stdin this port doesn't have) -- exporting `main`
directly is already the natural, zero-shim shape here, the same way a
C app's `int main(int argc, char **argv)` is. See
`examples/zig/hello/hello.zig`.

## Build flow: `build-zig-app.sh`

One stage, simpler than either Rust's or C++'s:

```
./build-zig-app.sh yourapp.zig
```

`zig build-obj -target x86_64-linux-musl -mcmodel=large -mno-red-zone
-fno-stack-protector -fsingle-threaded -O ReleaseSmall -lc` compiles
the app (plus the `bm` module below, plus whatever of `std` it pulled
in) straight to one relocatable object -- unlike Rust's cargo build (a
full linked binary needing `build-rust-app.sh`'s own `-r` partial-link
stage first), there's no equivalent step needed here. That object then
feeds into the same per-app shim compile + final `ld -T port/c.ld`
link `build-app.sh` does. `-mcmodel=large`/`-mno-red-zone` match
`build-app.sh`'s own CFLAGS -- this image is loaded at a fixed
high-canonical address with no PIE support and no stack-switching on
interrupt entry, the same freestanding ABI every language here builds
against. `-fno-stack-protector`/`-fsingle-threaded` are this port's own
choice on top of that ABI, not forced by it -- see "Known gaps" below
for why.

No unwind stub is needed the way Rust's port needs
`unwind_stub.c`/C++'s needs `cxxabi_stub.cpp`: a Zig app built this way
never references any `_Unwind_*`/`__cxa_*` symbol in the first place
(Zig has no unwinding runtime at all -- a panic always aborts the
process, an ahead-of-time posture rather than a stub catching what
would otherwise be a real unwind).

## `port/zig_port/bm.zig`: why not `std.debug.print`

The one port-support file this needs, imported as the `bm` module
(`--dep bm -Mbm=port/zig_port/bm.zig` in `build-zig-app.sh`).
`bm.print`/`bm.eprint` format into a fixed on-stack buffer
(`std.fmt.bufPrint`, no heap allocation, no locking) and write it with
a single, plain `std.c.write()` call -- nothing else.

This exists because `std.debug.print` is not safe to call here.
Confirmed with a real boot: linking and booting a `std.debug.print`
call produced `Exception 0x06 (UD)` with `RAX=0x000000BA` (186 =
`__NR_gettid`) -- the CPU raised Invalid Opcode on a bare `syscall`
instruction the compiler emitted *inline*, with no libc symbol behind
it at all for this port's patched musl to intercept.
`std.debug.print`'s stderr-locking/tty-detection path reaches this
(`std.Thread.Mutex`/`Futex`/`getCurrentId()`, plus a `statx` call),
regardless of `-lc` and regardless of `-fsingle-threaded` (that flag
removes the `futex` calls specifically, since there's no second thread
to contend with, but not the `gettid`/`statx` ones). BareMetal never
enables `EFER.SCE`/programs the `STAR`/`LSTAR`/`SFMASK` MSRs the real
`SYSCALL` instruction needs -- this port's own ring-3 syscall path uses
`int 0x80` instead (see the ring-3 support work), so the bare opcode
has nothing to trap into.

## Verified so far

- `examples/zig/hello/hello.zig` builds via `./1-build.sh
  hello.zig` and boots under `./2-run.sh`, printing to the console via
  `bm.print` -- confirms the `export fn main(...)` entry point, libc
  file-descriptor I/O, and clean process exit all work.
- A `std.fmt.bufPrint`-formatted, multi-argument `bm.print` call
  (string + integer interpolation) links and boots correctly with zero
  inlined `syscall` instructions in the resulting object (checked via
  `objdump`).
- The `std.debug.print` failure mode above was reproduced on purpose,
  end to end (real link, real boot, real crash), not just reasoned
  about from source.

## Known gaps

See `OPENISSUES.md`'s Zig section for the condensed version. The
underlying cause for all of them is the same: **anything in `std` that
reaches a raw, inlined `syscall` instruction crashes with `Exception
0x06 (UD)`, unconditionally, regardless of `-lc`.** This is not a
per-symbol shim gap the way C++'s `std::__throw_*()` overrides are --
there's no linkable symbol at that call site for this port to patch.
Known affected paths, all avoided in `port/zig_port/bm.zig` and meant
to be avoided in app code too, until/unless BareMetal's kernel grows a
real `SYSCALL`/`SYSRET` handler (a bounded, reusable kernel-side
addition -- see the ring-3 support work's `int 0x80` path for the
existing precedent -- but out of scope for this app-level port):

- `std.debug.print` and anything else that touches `std.debug`'s
  internal stderr lock.
- `std.Thread`, `std.Thread.Mutex`, `std.Thread.Futex`,
  `std.Thread.getCurrentId()` -- real OS threads are not available to
  Zig apps on this port at all (this port's real cooperative pthreads,
  `thread_shim.c`, are only reachable from C/Rust/C++/Python/Lua via
  the `libc`/musl pthread API, not from `std.Thread`, which never goes
  through libc for these calls in the first place).
- Anything else in `std` documented as going through
  `std.os.linux`/`std.os.windows`-style raw syscalls "whether or not
  libc is linked" (that phrase is `lib/std/os/linux.zig`'s own file
  header) -- not audited exhaustively beyond what `hello.zig`/`bm.zig`
  exercise; treat any new `std` API as unverified until checked the
  same way (`objdump -d` the resulting `.o` for bare `syscall`
  instructions before trusting it).
- Locale is always "C"/POSIX, same posture as every other language
  here.
