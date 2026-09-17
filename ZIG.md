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
c.ld` link, simpler than either.

**This requires `BareMetal-Firecracker`'s own `zig` branch, not its
`main`.** Ordinary Zig code -- `std.debug.print`, `std.Thread`, anything
else -- routinely emits the raw `syscall` x86 instruction directly,
bypassing libc entirely (see "Why this works" below for the full
mechanism). That instruction needs real kernel-side support
(`EFER.SCE`/`STAR`/`LSTAR`/`SFMASK`, a real entry stub) this port's
kernel didn't have until now; `BareMetal-Firecracker`'s `zig` branch
adds it (`src/BareMetal/interrupt.asm`'s `int_syscall_fast`, `src/
BareMetal/init/64.asm`'s MSR setup, `src/init/init.asm`'s GDT additions).
Built against a `main`-branch kernel without that fix, the exact same
Zig code that works here reproduces the original crash this work started
from: `Exception 0x06 (UD)`, `RAX=__NR_gettid` (or whatever syscall
number happened to be in flight). If you hit that, you're on the wrong
kernel branch, not a bug in your Zig code.

## Why this works

Zig's own `lib/std/os/linux.zig` implements Linux syscalls itself,
independent of whether libc is linked -- for *some* of `std`, this always
takes that path (an inlined `syscall` instruction, no libc call in
sight); for the rest, `-lc` genuinely routes it through libc. Ordinary
file I/O and most of `std.c`/`std.posix` (`open`/`read`/`write`/`lseek`/
`stat`/heap allocation via libc `malloc`/...) compile down to real
*calls* into libc symbols when `-target x86_64-linux-musl -lc` is passed
-- confirmed via `nm`/`objdump` on the resulting object (`writev`/
`pwritev`/`lseek`/`__errno_location` show up as undefined symbols) and a
real boot. Since this port's musl is a real, ABI-compatible musl
re-plumbed only at the syscall trap (`__bmos_syscall()`, see
`port/musl_port/`'s patch -- the same fact `RUST.md`'s "Why this works
at all" section explains for Rust's `libc` crate), that's exactly the
interception point this port already had, and it catches everything
routed through it.

`std.debug.print`'s stderr-locking path, `std.Thread`/`Mutex`/`Futex`/
`getCurrentId()`, and apparently other spots throughout `std` take the
*other* path -- a raw `syscall` instruction, inlined at the call site,
matching real Linux's raw ABI (`RAX`=syscall number, `RDI`/`RSI`/`RDX`/
`R10`/`R8`/`R9`=args 1-6, only `RAX`/`RCX`/`R11` change across the call)
but with **no libc symbol anywhere for `-lc`/this port's patched musl to
intercept**. That's what `BareMetal-Firecracker`'s `zig` branch adds
kernel-side support for -- see the next section.

## SYSCALL/SYSRET (`BareMetal-Firecracker`'s `zig` branch)

Adds a second, parallel way from ring 3 into the kernel, alongside the
existing `int 0x80` gate (`int_syscall`) every `b_*` call already uses:

- `src/init/init.asm`'s `gdt64` gains three new selectors
  (`SYSRET_CS32_SEL`/`SYSRET_SS_SEL`/`SYSRET_CS64_SEL`) at a fresh,
  consecutive `base`/`base+8`/`base+16` block -- `IA32_STAR`'s SYSCALL/
  SYSRET arithmetic requires that exact layout, which the existing
  `USR64_CODE_SEL`/`USR64_DATA_SEL` pair (8 bytes apart, not 16) can't
  satisfy without renumbering everything already built on them.
- `src/BareMetal/init/64.asm`'s `init_64` programs `IA32_EFER.SCE`,
  `IA32_STAR` (SYSCALL target = the existing `SYS64_CODE_SEL`/
  `SYS64_DATA_SEL` pair; SYSRET base = the new block above), `IA32_LSTAR`
  (target = the new entry stub, below), and `IA32_FMASK` (clears IF/TF/DF
  on entry, matching the "interrupts off while manually switching stacks"
  posture `int 0x80`'s interrupt-gate semantics already give it for
  free).
- `src/BareMetal/interrupt.asm`'s `int_syscall_fast` is the entry stub
  itself (see its own, extensive header comment for the full blow-by-blow).
  In short: swap to the fixed kernel stack (SYSCALL doesn't consult the
  TSS RSP0 the way an interrupt/exception gate does, so this is done by
  hand), save every register the real Linux ABI promises survives a
  syscall, marshal into `__bmos_syscall(n, a1..a6)`'s plain SysV C call
  ABI and call it directly (ring 0 can call the app's own ring-3-mapped
  `__bmos_syscall()` -- reused instead of reimplementing a second syscall
  dispatcher in assembly), restore every saved register, and return via
  `iretq` -- not `sysret`. The app publishes its own `__bmos_syscall`
  address into a fixed low-memory slot (`app_bmos_syscall_ptr`,
  `sysvar.asm`) once, at `_start` time (see `crt0.c`'s own comment there),
  since every app is compiled/linked independently and the kernel has no
  other way to find it.

**Two real bugs found and fixed while getting this working, both worth
knowing if this code is ever touched again:**

1. **Stack alignment.** SysV requires `RSP % 16 == 0` immediately before
   any `call`. The first version of `int_syscall_fast` pushed an odd
   number of 8-byte values before calling `__bmos_syscall`, leaving RSP
   8-mod-16 at the call site -- `-O2` GCC code (`__bmos_syscall` and
   anything it calls) can silently miscompile against that if it ever
   spills to an aligned SSE store. Symptom: intermittent, timing-
   dependent `#GP` crashes, always somewhere *else* (a later, unrelated
   interrupt handler), never at the actual bug site -- a reminder that a
   misaligned stack corrupts silently, not loudly.
2. **Register preservation.** The real Linux syscall ABI promises
   `RDI`/`RSI`/`RDX`/`R10`/`R8`/`R9` survive a syscall unchanged (only
   `RAX`/`RCX`/`R11` may). The first working version used those
   registers as scratch space for its own arg-shuffling into
   `__bmos_syscall`'s SysV call and never restored them. This "worked"
   for a single isolated syscall (nothing reloads its own operands from
   them right after) but silently corrupted whatever a caller had chosen
   to keep live there across a *second* one -- confirmed with a real,
   repeatable test: two back-to-back raw `write` syscalls, only the
   first one's output ever appeared. Fixed by saving all six to the
   stack up front and restoring them, byte-for-byte, before returning.

Also: `sysret` (the "natural" SYSCALL-paired return instruction) was
tried first and produced intermittent `#GP` crashes that were never
conclusively root-caused -- every `STAR`/GDT byte was re-verified against
the assembled listing and checked out, and an extra `cli` in front of the
whole handler (redundant with `IA32_FMASK`, testing whether IF was really
staying clear) made no difference, arguing against a masking/race
explanation. `iretq` -- the exact same ring0→ring3 return mechanism every
other path in this kernel already used successfully -- sidesteps whatever
that was.

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
-fno-stack-protector -O ReleaseSmall -lc` compiles the app (plus the
optional `bm` module below, plus whatever of `std` it pulled in) straight
to one relocatable object -- unlike Rust's cargo build (a full linked
binary needing `build-rust-app.sh`'s own `-r` partial-link stage first),
there's no equivalent step needed here. That object then feeds into the
same per-app shim compile + final `ld -T port/c.ld` link `build-app.sh`
does. `-mcmodel=large`/`-mno-red-zone` match `build-app.sh`'s own
CFLAGS -- this image is loaded at a fixed high-canonical address with no
PIE support and no stack-switching on interrupt entry, the same
freestanding ABI every language here builds against.
`-fno-stack-protector` is this port's own choice on top of that ABI, not
forced by it, matching every other language here.

**`-O ReleaseSmall` (never plain Debug mode) is required.** Zig's
Debug-mode codegen for `std.Io.Writer`'s internals (the machinery
`std.debug.print` goes through in Zig 0.15) emits some anonymous
constant/vtable references as absolute 32-bit (`R_X86_64_32`)
relocations that can't reach this port's high-canonical
(`0xFFFF8000...`) load address -- `ld` fails with "relocation truncated
to fit". `ReleaseSmall`/`ReleaseFast` don't hit this (confirmed by a real
link+boot of `std.debug.print` calls, both plain and formatted, under
`ReleaseSmall`) -- an LLVM/Zig code-model limitation, not something this
port's build can work around.

No unwind stub is needed the way Rust's port needs
`unwind_stub.c`/C++'s needs `cxxabi_stub.cpp`: a Zig app built this way
never references any `_Unwind_*`/`__cxa_*` symbol in the first place
(Zig has no unwinding runtime at all -- a panic always aborts the
process, an ahead-of-time posture rather than a stub catching what
would otherwise be a real unwind).

## `port/zig_port/bm.zig` (optional)

`bm.print`/`bm.eprint` format into a fixed on-stack buffer
(`std.fmt.bufPrint`, no heap allocation, no locking) and write it with a
single, plain `std.c.write()` call. Originally written as a *mandatory*
workaround for `std.debug.print`'s crash (see `bm.zig`'s own header for
that history) -- now that the SYSCALL/SYSRET fix makes `std.debug.print`
itself safe to use, this file is kept purely as an optional,
lighter-weight alternative (no locking, no dependency on the
Firecracker kernel fix at all, since it routes through libc the same way
every other language here already does), imported as the `bm` module
(`@import("bm")`) if you want it.

## Verified so far

- `examples/zig/hello/hello.zig` (real, unmodified `std.debug.print`)
  builds via `./1-build.sh hello.zig` and boots under `./2-run.sh`,
  printing to the console -- confirms the `export fn main(...)` entry
  point, the SYSCALL/SYSRET path, and clean process exit all work.
- Plain and formatted (`{d}` integer interpolation) `std.debug.print`
  calls both link and boot correctly under `-O ReleaseSmall`.
- A real `std.Thread.spawn`/`join` test (4 threads, each incrementing a
  shared `std.atomic.Value(i32)`, joined and checked) boots and reports
  the correct final count -- `clone`/`futex`-backed real threading works
  end to end through `thread_shim.c`'s existing cooperative pthreads,
  the same machinery every other language's real threads already use.
- Both of the above were run repeatedly (4-6 boots each) to rule out the
  intermittency the two kernel-side bugs above caused before they were
  found -- consistently clean.
- Debug-mode (`-O` omitted) builds of anything touching `std.Io.Writer`
  hit the relocation-truncation issue described above; not something
  this port's build can paper over. Always pass `-O ReleaseSmall` (or
  `ReleaseFast`, untried but should behave the same way).
- `examples/zig/webserver/webserver.zig` (real `std.net.Address.listen`/
  `Server.accept`, matching `webserver.c`/`webserver-rs`/`webserver.py`):
  built via `build-zig-app.sh`, booted with real Firecracker networking
  (`BareMetal-Firecracker/scripts/mkbr0.sh`'s bridge/tap), and fetched
  with a real `curl` from the host -- `HTTP/1.1 200`, correct HTML body,
  hit counter incrementing across requests, request line logged to the
  console for each connection. Confirms `bind`/`listen`/`accept`/`read`
  all work through the musl -> `posix_shim` -> `net_shim` -> lwIP path
  the same way they already did for every other language.

## Known gaps

See `OPENISSUES.md`'s Zig section for the condensed version.

- **Debug-mode builds don't link** (see "Build flow" above) -- use
  `-O ReleaseSmall`/`ReleaseFast`.
- **`std.net.Stream.write()`/`writeAll()` (the current, non-deprecated
  API) don't work -- use `std.posix.write()` instead.** Found while
  building `examples/zig/webserver/webserver.zig`: Zig 0.15's rewritten
  `Io.Writer` machinery backs `Stream.write()` with a real `sendmsg()`
  syscall, and this port's `posix_shim.c` has no `SYS_sendmsg` (or
  `SYS_recvmsg`) case at all -- it falls through to the `-ENOSYS`
  default, which Zig's error-mapping doesn't expect from a real Linux
  `write()` and surfaces as `error.Unexpected`. Symptom: a connection
  that reads the client's request fine and then just closes with no
  response, no crash. `std.posix.write()` (the plain `SYS_write` syscall
  `Stream.write()` itself used before the 0.15 rework) works correctly,
  through the same `sys_write()`/`net_shim_send()` path `stream.read()`
  already uses on the way in -- see `webserver.zig`'s own comment on its
  local `writeAll()` helper. Fixing this for real would mean adding
  `SYS_sendmsg`/`SYS_recvmsg` to `posix_shim.c`/`net_shim.c` (flattening
  the iovecs into the existing `net_shim_send`/`recv`, similar to how
  `sys_writev`/`sys_readv` already do) -- not attempted here, out of
  scope for an app-level example.
- **Not exhaustively audited beyond `std.debug.print`/`std.Thread`/
  basic TCP server sockets.** The rest of `std` (more of `std.fs`,
  `std.process`, UDP, ...) is presumed to work the same way file I/O
  already did (real libc calls, or now real syscalls via
  `int_syscall_fast`) but hasn't been individually exercised.
- Locale is always "C"/POSIX, same posture as every other language
  here.
- Requires `BareMetal-Firecracker`'s `zig` branch, not `main` -- see the
  top of this document.
