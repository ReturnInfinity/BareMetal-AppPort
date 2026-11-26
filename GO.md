# Go on BareMetal

Ordinary Go programs build into a BareMetal app, using the host's own Go
toolchain (tested with Go 1.26.8). Goroutines, `sync`, channels, timers,
`os` file I/O on the EXT2 disk, and `net/http` (server and client, DNS,
HTTPS with certificate verification) work.

```
./build-go-app.sh examples/go/hello/hello.go           # -> hello.app
./build-go-app.sh examples/go/webserver/webserver.go   # net/http on :80
./build-go-app.sh path/to/package-dir                   # a main package in a module
```

From the top-level `BareMetal-App` repo, `./1-build.sh hello.go` does the
same and builds `baremetal.elf` around it.

Requirements: `go` on `PATH`, and a `BareMetal-Firecracker` kernel with the
ring-3 `int_syscall_fast` bounce (see "Raw syscalls" below).

Your program is a normal `package main` with a normal `func main()`.
Nothing needs to change in it.

## How it's built

`build-go-app.sh` runs `go build -buildmode=c-archive` with cgo on and
`CC` pointed at this port's musl headers, then links the archive through
`build-app.sh` like any other app:

- **c-archive** compiles Go as position-independent code, so it runs at
  `c.ld`'s `0xFFFF800000000000`. A normal Go executable is linked for
  `0x400000`.
- **cgo** makes the runtime create its threads with `pthread_create()`, so
  they're ordinary `thread_shim.c` threads, and route `mmap`/`sigaction`
  through musl.
- The runtime starts from an `.init_array` constructor, before `main()`.
  `port/go_port/go_main.c` is the app's C `main()`. It calls `bm_go_main()`,
  which `port/go_port/bm_main.go` (added to your package at build time)
  exports, and that calls your `main()`. When `main()` returns, the app
  exits.

An `-overlay` changes two things in the Go runtime without forking GOROOT:

- **`heapAddrBits = 40`** (`runtime/malloc.go`, patched with `sed` at
  build time). Go's 64-bit runtime sizes its heap metadata for a 48-bit
  address space and reserves all of it up front: about 1.1 GiB of
  `PROT_NONE` mappings. Linux backs those lazily; this port has no paging,
  so every mapping is real memory. 40 bits (what Go already uses on
  ios/arm64) shrinks that to a few MiB. The heap sits right above amd64's
  `arenaBaseOffset` (`0xffff800000000000`) here, so it stays in range.
- **The port's CA bundle** (`port/go_port/bm_x509.go`, added to package
  `crypto/x509`). `/etc/ssl/cacert.pem`, where `install-cacert.sh` puts it
  on disk.img, is appended to the files Go's root loader tries. Programs
  that don't use TLS don't link it.
- **Async preemption off** (`port/go_port/bm_runtime.go`, added to package
  `runtime`). Same as `GODEBUG=asyncpreemptoff=1`, which a BareMetal app
  has no environment to set. Async preemption delivers `SIGURG` and resumes
  from the signal frame's registers, but `thread_shim.c` runs handlers as
  plain calls with no real frame. Goroutines are still preempted
  cooperatively at function calls.

## What the port needed

- **Raw syscalls.** Even with cgo, the runtime issues raw `syscall`
  instructions for `futex`, `nanosleep`, `epoll_pwait`, `madvise` and
  more. The kernel's `int_syscall_fast` used to service these in ring 0
  on the one kernel stack with interrupts masked. That works for short
  calls, but Go blocks in them from several threads, and the first
  thread switch inside one corrupted the stack (`#GP` at
  `int_syscall_end`). Now the kernel immediately `iretq`s to
  `__bmos_syscall_tramp` (`crt0.c`) in ring 3. The trampoline calls
  `__bmos_syscall()` and returns past the red zone, so a raw syscall
  behaves exactly like a libc one. Zig uses the same path (`ZIG.md`).
- **Per-thread syscall stacks** (`thread_shim.c`). Go makes raw syscalls
  on goroutine stacks, which may have only a few hundred bytes free; a
  Linux syscall uses none of it, but `__bmos_syscall()` is C that can go
  deep (lwIP, lwext4). The trampoline therefore switches to a 64 KiB
  stack per thread (`bmos_syscall_stack_top`, updated on every thread
  switch). It leaves about 200 bytes on the caller's stack.
- **Constructor arguments.** glibc calls `.init_array` functions with
  `(argc, argv, envp)`; musl calls them with none. Go's library-mode
  entry point reads auxv (page size, random seed) through `argv`, so it
  read garbage. `crt0.c` now runs the constructors itself, with those
  arguments, right before `main()`. `c.ld` gives musl an empty range.
  This also needed `_start` pinned to the front of the image
  (`.text.start`): the kernel enters the flat binary at its first byte.
- **`mmap` semantics** (`posix_shim.c`): `PROT_NONE` reservations are only
  address space, `MAP_FIXED` over one commits it (hot-plugging memory if
  needed, and zeroing it), and `madvise(MADV_DONTNEED)` zeroes the range.
- **`epoll` and `eventfd`** (`posix_shim.c`). Go's timers and network
  poller run on them. `EPOLLET` is emulated (see that section's comment).
  Plain level-triggered reporting would make the netpoller spin.
- **Small stubs**: `gettid`, `getpid`, `sched_getaffinity` (one CPU),
  `prlimit64`/`getrlimit`/`setrlimit`, `mprotect`, `prctl`.
- **DNS through the port's resolver.** `build-go-app.sh` builds with
  `-tags netcgo`, so host lookups go through musl's `getaddrinfo()` and
  `dns_shim.c` (DHCP's DNS server, or Firecracker's `ip=` one), like every
  other language here. Go's own resolver reads `/etc/resolv.conf`, which
  doesn't exist, and falls back to asking localhost.
- **A working scheduler tick, and quiet idling** (`thread_shim.c`). Since
  the kernel went tickless nothing had switched the 1 ms tick on, so
  threads were never preempted and signals never delivered. The tick now
  runs while any thread is running. When all are parked, the scheduler
  turns it off and halts in one `SLEEP_IO` until the earliest deadline or
  an I/O interrupt. An idle Go web server went from 2.3% of a host CPU
  to about 0.15%.
- **An lwIP lock** (`net_lock()`, `net_glue.h`). Once threads really were
  preempted, two of Go's threads in lwIP at once tripped
  `tcp_write: no pbufs on queue` under parallel 1 MiB downloads. Every
  path into lwIP now holds one recursive lock, released while a socket
  call waits. DNS lookups are serialised separately (`dns_shim.c` shares
  one wait slot).

Building `posix_shim.c` with `POSIX_SHIM_CFLAGS=-DBMOS_TRACE_ENOSYS` (an
environment variable read by `build-app.sh`, so it works through
`build-go-app.sh` too) prints every syscall that falls through to `-ENOSYS`.
That's the quickest way to find what a new program is missing.

## Memory

`examples/go/hello` boots with `MEMSIZE=6` and the default hot-plug
settings; the app image is about 1.8 MiB. The runtime reserves address
space in big pieces (128 MiB per 64 MiB heap arena) but the shim only
makes the parts it commits into real memory (see `OPENISSUES.md`'s heap
section), and untouched memory costs the host nothing.

## Networking

Tested under QEMU with port forwarding (`HOSTFWD=18081:80 ./2-run-qemu.sh`):

- **Server:** 200 requests, 10 at a time, all succeeded (about 0.8 ms per
  request). Keep-alive works, and 1 MiB responses came back intact, both
  sequential and 8 in parallel.
- **Client:** HTTP to the host and to the internet, DNS, and HTTPS
  (`https://example.com`, `https://www.google.com`) with certificate
  verification.

## Known gaps

- **IPv4 only**, like the rest of the port. `LookupTXT`/`LookupSRV`/
  `LookupMX` use Go's own resolver and fail (no `/etc/resolv.conf`); host
  lookups are fine.
- **No `os.Args`/environment from Go's point of view beyond what
  `crt0.c` passes** (`argv` from Firecracker's `args=` token, empty
  environment).
- **Signals.** `os/signal` won't see anything; see `thread_shim.c`'s
  "Signals" section for what this port delivers at all.
