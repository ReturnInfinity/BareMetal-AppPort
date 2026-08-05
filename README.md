# BareMetal AppPort

A build system for compiling your own C applications to run as BareMetal apps: a [musl](https://musl.libc.org/) libc port (syscalls dispatched into `libBareMetal` calls instead of trapped), a [BMFS](https://github.com/ReturnInfinity/BMFS) file I/O layer, a [lwIP](https://savannah.nongnu.org/projects/lwip/)-based TCP/IP networking, [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) for TLS/SSL, and [curl](https://curl.se/)/libcurl (HTTP/HTTPS only) on top of all of it. See `OPENISSUES.md` for what's supported and what isn't.

## Requirements

`gcc`, `ld`, `make`, `curl`, `tar`, `unzip` (a standard Linux toolchain works).

## Setup

Run once, from this directory:

```
./setup.sh
```

This downloads musl 1.2.6 and applies the BareMetal port patch, then downloads lwIP 2.2.0, Mbed TLS 3.6.6, and curl 8.21.0 (all three used as-is, unmodified), creating `build/musl-1.2.6/`, `build/lwip-2.2.0/`, `build/mbedtls-3.6.6/`, and `build/curl-8.21.0/`. All are pinned versions -- the patch and the `port/lwip_port/`/`port/mbedtls_port/`/`port/curl_port/` glue are written against these exact releases. (`setup.sh` just runs `scripts/get-musl.sh`, `scripts/get-lwip.sh`, `scripts/get-mbedtls.sh`, and `scripts/get-curl.sh` in turn, if you want to re-run one on its own.)

## Building an app

```
./build-app.sh myapp.c     # builds your own app -> myapp.app
```

Downloaded sources and intermediate `.o` files live under `build/`; the final `.app` is placed here in the top-level directory. It's a flat binary linked at `0xFFFF800000000000` (see `port/c.ld`), ready to load as a BareMetal app (e.g. copy it onto a BMFS disk image and load it from the BareMetal monitor or run it as a unikernel).

`./clean.sh` removes library code and build artifacts (`.o`/`.a`/`.app`) from this directory and `build/` without touching the fetched `musl-1.2.6/`/`lwip-2.2.0/`/`mbedtls-3.6.6/`/`curl-8.21.0/` zip/tarball.

## What's in here

- `setup.sh` -- fetches musl, lwIP, Mbed TLS, and curl (see Setup above).
- `build-app.sh` -- builds an app (see Building an app above).
- `clean.sh` -- removes build artifacts.
- `hello.c` -- minimal demo app (musl `printf`, argc/argv/envp).
- `crawler.c`/`https_crawler.c` -- a small HTTP(S) web crawler, speaking
  raw HTTP by hand over `port/net_shim.c`'s sockets and TLS by hand
  over `port/tls_shim.c`'s mbedTLS wrapper.
- `curltest.c` -- a minimal demo of libcurl's easy interface (an HTTP/
  HTTPS GET) -- the same sockets and the same vendored mbedTLS as
  above, but reached through curl's own APIs instead.
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
  - `dns_shim.c` -- `gethostbyname()`, backed by lwIP's resolver.
  - `tls_shim.c`/`.h`, `mbedtls_port/` -- a small blocking HTTPS-shaped
    TLS client wrapper over Mbed TLS, plus its port config
    (`baremetal_mbedtls_config.h`) and RNG hook
    (`entropy_hardware_poll.c`, via `rdrand`).
  - `curl_port/curl_config.h` -- libcurl's build config for this port
    (HTTP/HTTPS only, mbedTLS backend, `gethostbyname()`-based
    resolver, no threads -- see its own file header and
    `OPENISSUES.md`'s "libcurl" section for the reasoning behind each).
  - `libBareMetal.c`/`.h`/`.asm` -- the low-level calls into the
    BareMetal kernel (`b_output`, `b_net_tx`, ...) everything above is
    built on.
- `scripts/` -- the fetch scripts `setup.sh` calls:
  - `get-musl.sh` -- downloads musl 1.2.6 and applies
    `port/musl_port/musl-1.2.6-baremetal.patch`, the 3-file patch
    (syscall transport, TLS bootstrap, cancellation-point syscalls),
    then installs `port/musl_port/musl-1.2.6-config.mak` as musl's
    `config.mak` (equivalent to running musl's `./configure` with the
    flags this port needs, without you having to run `configure`
    yourself).
  - `get-lwip.sh` -- downloads lwIP 2.2.0. lwIP is vendored
    unmodified; all lwIP-side port work lives in `port/lwip_port/`
    and `port/net_glue.c`/`net_shim.c` instead of patches to lwIP
    itself.
  - `get-mbedtls.sh` -- downloads Mbed-TLS 3.6.6. Mbed-TLS is vendored
    unmodified; all Mbed-TLS-side port work lives in `port/tls_shim.c`
    instead of patches to Mbed-TLS itself.
  - `get-curl.sh` -- downloads curl 8.21.0. curl is vendored unmodified
    too; all curl-side port work lives in `port/curl_port/curl_config.h`
    instead of patches to curl itself.

## Limitations

This is not a general-purpose POSIX environment: no `fork`/`exec`, no threads (yet), no signals (yet?), flat BMFS namespace (no subdirectories), TCP/UDP only (no raw sockets exposed), 30s timeout on blocking socket calls. See `OPENISSUES.md` for the full list and the reasoning behind each cut.
