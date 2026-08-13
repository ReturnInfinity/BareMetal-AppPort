# BareMetal AppPort

A build system for compiling your own C applications to run as BareMetal apps: a [musl](https://musl.libc.org/) libc port (syscalls dispatched into `libBareMetal` calls instead of trapped), an EXT2 file I/O layer via [lwext4](https://github.com/gkostka/lwext4), a [lwIP](https://savannah.nongnu.org/projects/lwip/)-based TCP/IP networking, [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) for TLS/SSL, [curl](https://curl.se/)/libcurl (HTTP/HTTPS only) on top of all of it, and [SQLite](https://sqlite.org/) on top of EXT2 via its own small VFS. See `OPENISSUES.md` for what's supported and what isn't.

## Requirements

`gcc`, `ld`, `make`, `curl`, `tar`, `unzip` (a standard Linux toolchain works).

## Setup

Run once, from this directory:

```
./setup.sh
```

This downloads musl 1.2.6 and applies the BareMetal port patch, then downloads lwIP 2.2.0, Mbed TLS 3.6.6, curl 8.21.0, the SQLite 3.46.1 amalgamation, and a pinned lwext4 commit (all used as-is, unmodified), creating `build/musl-1.2.6/`, `build/lwip-2.2.0/`, `build/mbedtls-3.6.6/`, `build/curl-8.21.0/`, `build/sqlite-3.46.1/`, and `build/lwext4-58bcf89/`. All are pinned versions -- the patch and the `port/lwip_port/`/`port/mbedtls_port/`/`port/curl_port/`/`port/sqlite_port/`/`port/lwext4_port/` glue are written against these exact releases. (`setup.sh` just runs `scripts/get-musl.sh`, `scripts/get-lwip.sh`, `scripts/get-mbedtls.sh`, `scripts/get-curl.sh`, `scripts/get-sqlite.sh`, and `scripts/get-lwext4.sh` in turn, if you want to re-run one on its own.)

## Building an app

```
./build-app.sh myapp.c     # builds your own app -> myapp.app
```

Downloaded sources and intermediate `.o` files live under `build/`; the final `.app` is placed here in the top-level directory. It's a flat binary linked at `0xFFFF800000000000` (see `port/c.ld`), ready to load as a BareMetal app (e.g. copy it onto a disk image formatted with a plain EXT2 filesystem -- `mkfs.ext2` -- and load it from the BareMetal monitor or run it as a unikernel).

`./clean.sh` removes library code and build artifacts (`.o`/`.a`/`.app`) from this directory and `build/` without touching the fetched `musl-1.2.6/`/`lwip-2.2.0/`/`mbedtls-3.6.6/`/`curl-8.21.0/`/`sqlite-3.46.1/`/`lwext4-58bcf89/` zip/tarball.

## What's in here

- `setup.sh` -- fetches musl, lwIP, Mbed TLS, curl, and lwext4 (see Setup above).
- `build-app.sh` -- builds an app (see Building an app above).
- `clean.sh` -- removes build artifacts.
- `hello.c` -- minimal demo app (musl `printf`, argc/argv/envp).
- `clock.c` -- prints the current wall-clock time (via `time()` and a
  direct `b_system(WALLCLOCK, ...)` call) and time elapsed since boot
  (`clock_gettime(CLOCK_MONOTONIC, ...)`).
- `crawler.c`/`https_crawler.c` -- a small HTTP(S) web crawler, speaking
  raw HTTP by hand over `port/net_shim.c`'s sockets and TLS by hand
  over `port/tls_shim.c`'s mbedTLS wrapper.
- `curltest.c` -- a minimal demo of libcurl's easy interface (an HTTP/
  HTTPS GET) -- the same sockets and the same vendored mbedTLS as
  above, but reached through curl's own APIs instead.
- `sqltest.c` -- a minimal demo of SQLite: creates a table on a real
  EXT2-backed database file, inserts rows across two transactions, and
  queries them back -- exercising `port/sqlite_port/sqlite_vfs.c`'s
  read/write/journal handling end to end.
- `fs_test.c` -- exercises `port/ext4_shim.c`'s POSIX file I/O
  end to end: create/read/write/lseek/fstat/stat/unlink, `chdir`/
  `getcwd`, `mkdir`/`opendir`/`readdir`/`rmdir`, and `symlink`/
  `readlink`, all against the EXT2 image lwext4 mounts.
- `threads.c` -- exercises `port/thread_shim.c`'s cooperative pthreads
  end to end: `pthread_create`/`join`/`detach`/`self`/`equal`, mutexes
  (normal/`trylock`/recursive), condition variables (`signal`/
  `broadcast`/`timedwait`), rwlocks, `pthread_once`, thread-specific
  data (`pthread_key_*` and `__thread`), spinlocks, barriers, and
  `sched_yield`.
- `port/` -- the port glue every app links against:
  - `crt0.c`, `c.ld` -- startup and linker script for the flat-binary,
    ring-0, fixed-address BareMetal environment (no ELF loader, no
    syscall trap).
  - `posix_shim.c`/`.h` -- the syscall dispatcher musl's patched
    `syscall_arch.h` calls into, plus the heap (`brk`/`mmap`) backing
    it.
  - `ext4_shim.c`/`.h` -- POSIX file I/O (`open`/`read`/`write`/`stat`/
    `mkdir`/`readdir`/`symlink`/`chdir`/...) on top of a real EXT2
    filesystem, mounted and served through lwext4.
  - `lwext4_port/` -- lwext4's block device glue over
    `b_nvs_read`/`b_nvs_write`, plus the EXT2-only feature config
    (`generated/ext4_config.h`).
  - `net_glue.c`/`.h`, `net_shim.c`/`.h`, `lwip_port/` -- a blocking
    BSD-socket-shaped layer over lwIP's raw callback API, plus the
    Ethernet netif driver and port config.
  - `dns_shim.c` -- `gethostbyname()`, backed by lwIP's resolver.
  - `thread_shim.c`/`.h` -- cooperative pthreads (`SYS_clone`/
    `SYS_futex`/`SYS_sched_yield`), scheduled by a
    `b_system(CALLBACK_TIMER, ...)`-driven round-robin tick -- see its
    own file header for the design and `OPENISSUES.md`'s "Process
    model" section for what's supported.
  - `tls_shim.c`/`.h`, `mbedtls_port/` -- a small blocking HTTPS-shaped
    TLS client wrapper over Mbed TLS, plus its port config
    (`baremetal_mbedtls_config.h`) and RNG hook
    (`entropy_hardware_poll.c`, via `rdrand`).
  - `curl_port/curl_config.h` -- libcurl's build config for this port
    (HTTP/HTTPS only, mbedTLS backend, `gethostbyname()`-based
    resolver, no threads -- see its own file header and
    `OPENISSUES.md`'s "libcurl" section for the reasoning behind each).
  - `sqlite_port/sqlite_baremetal_config.h`, `sqlite_port/sqlite_vfs.c`
    -- SQLite's build config for this port (`SQLITE_OS_OTHER=1`,
    single-threaded, no WAL/mmap/load-extension -- see its own file
    header) and the small `sqlite3_vfs` implementation it requires in
    place of SQLite's own `os_unix.c`, built directly over
    `posix_shim.c`/`ext4_shim.c` the same way `tls_shim.c`/`net_shim.c` are
    (see `sqlite_vfs.c`'s own header and `OPENISSUES.md`'s "SQLite"
    section for the reasoning behind each choice).
  - `libBareMetal.c`/`.h`/`.asm` -- the low-level calls into the
    BareMetal kernel (`b_output`, `b_net_tx`, ...) everything above is
    built on.
- `scripts/` -- the fetch scripts `setup.sh` calls:
  - `get-musl.sh` -- downloads musl 1.2.6 and applies
    `port/musl_port/musl-1.2.6-baremetal.patch` (syscall transport, TLS
    bootstrap, cancellation-point syscalls, and the two raw-`syscall`
    asm sites -- `clone`/`__unmapself` -- thread_shim.c's threads need
    routed through the same dispatcher), then installs
    `port/musl_port/musl-1.2.6-config.mak` as musl's
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
  - `get-sqlite.sh` -- downloads the SQLite 3.46.1 amalgamation
    (`sqlite3.c`/`sqlite3.h`). Vendored unmodified as well; all
    SQLite-side port work lives in `port/sqlite_port/` instead of
    patches to `sqlite3.c` itself.
  - `get-lwext4.sh` -- downloads a pinned lwext4 commit (its last
    tagged release predates six years of upstream fixes). Vendored
    unmodified; all lwext4-side port work lives in `port/ext4_shim.c`
    and `port/lwext4_port/` instead of patches to lwext4 itself.

## Limitations

This is not a general-purpose POSIX environment: no `fork`/`exec`, no signals (yet?), TCP/UDP only (no raw sockets exposed), 30s timeout on blocking socket calls. Threading (`pthread_create` and friends, `threads.c`) works, but as cooperative user-level threads on one core, not real kernel threads -- see `port/thread_shim.c`'s file header. See `OPENISSUES.md` for the full list and the reasoning behind each cut.
