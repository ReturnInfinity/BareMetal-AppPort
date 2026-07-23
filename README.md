# BareMetal AppPort

A build system for compiling your own C applications to run as BareMetal apps: a musl libc port (syscalls dispatched into `libBareMetal` calls instead of trapped), a BMFS file I/O layer, and lwIP-based TCP/IP networking. See `OPENISSUES.md` for what's supported and what isn't.

## Requirements

`gcc`, `ld`, `make`, `curl`, `tar`, `unzip` (a standard Linux toolchain works).

## Setup

Run once, from this directory:

```
./setup.sh
```

This downloads musl 1.2.6 and applies the BareMetal port patch, then downloads lwIP 2.2.0 (used as-is, unmodified), creating `build/musl-1.2.6/` and `build/lwip-2.2.0/`. Both are pinned versions -- the patch and the `port/lwip_port/` glue are written against these exact releases. (`setup.sh` just runs `scripts/get-musl.sh` and `scripts/get-lwip.sh` in turn, if you want to re-run one on its own.)

## Building an app

```
./build-app.sh myapp.c     # builds your own app -> myapp.app
```

Downloaded sources and intermediate `.o` files live under `build/`; the final `.app` is placed here in the top-level directory. It's a flat binary linked at `0xFFFF800000000000` (see `port/c.ld`), ready to load as a BareMetal app (e.g. copy it onto a BMFS disk image and load it from the BareMetal monitor or run it as a unikernel).

`./clean.sh` removes library code and build artifacts (`.o`/`.a`/`.app`) from this directory and `build/` without touching the fetched `musl-1.2.6/`/`lwip-2.2.0/` zip/tarball.

## What's in here

- `setup.sh` -- fetches musl and lwIP (see Setup above).
- `build-app.sh` -- builds an app (see Building an app above).
- `clean.sh` -- removes build artifacts.
- `helloc.c` -- minimal demo app (musl `printf`, argc/argv/envp).
- `port/` -- the port glue every app links against:
  - `crt0.c`, `c.ld` -- startup and linker script for the flat-binary,
    ring-0, fixed-address BareMetal environment (no ELF loader, no
    syscall trap).
  - `posix_shim.c`/`.h` -- the syscall dispatcher musl's patched
    `syscall_arch.h` calls into, plus the heap (`brk`/`mmap`) backing
    it.
  - `bmfs.c`/`.h` -- POSIX file I/O (`open`/`read`/`write`/`stat`/...)
    on top of BMFS, the on-disk format BareMetal uses.
  - `net_glue.c`/`.h`, `net_shim.c`/`.h`, `lwip_port/` -- a blocking
    BSD-socket-shaped layer over lwIP's raw callback API, plus the
    Ethernet netif driver and port config.
  - `libBareMetal.c`/`.h`/`.asm` -- the low-level calls into the
    BareMetal kernel (`b_output`, `b_net_tx`, ...) everything above is
    built on.
- `scripts/` -- the fetch scripts `setup.sh` calls:
  - `get-musl.sh` -- downloads musl 1.2.6 and applies
    `patches/musl-1.2.6-baremetal.patch`, the 3-file patch (syscall
    transport, TLS bootstrap, cancellation-point syscalls), then
    installs `patches/musl-1.2.6-config.mak` as musl's `config.mak`
    (equivalent to running musl's `./configure` with the flags this
    port needs, without you having to run `configure` yourself).
  - `get-lwip.sh` -- downloads lwIP 2.2.0. lwIP is vendored
    unmodified; all lwIP-side port work lives in `port/lwip_port/`
    and `port/net_glue.c`/`net_shim.c` instead of patches to lwIP
    itself.

## Limitations

This is not a general-purpose POSIX environment: no `fork`/`exec`, no threads (yet), no signals (yet?), flat BMFS namespace (no subdirectories), TCP/UDP only (no raw sockets exposed), 30s timeout on blocking socket calls. See `OPENISSUES.md` for the full list and the reasoning behind each cut.
