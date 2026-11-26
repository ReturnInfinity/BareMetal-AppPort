# Go on BareMetal

Ordinary Go programs build into a BareMetal app, using the host's own Go
toolchain (tested with Go 1.26.8). Goroutines, `sync`, channels, timers,
`time.Sleep` and `os` file I/O on the EXT2 disk (from several goroutines at
once) work. Networking (`net`, `net/http`) is next to be tested.

```
./build-go-app.sh examples/go/hello/hello.go   # -> hello.app
./build-go-app.sh path/to/package-dir           # a main package in a module
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
- **`mmap` semantics** (`posix_shim.c`): `MAP_FIXED` over an
  already-allocated range re-zeroes it in place (how Go commits reserved
  memory), `PROT_NONE` reservations aren't zeroed up front, and
  `madvise(MADV_DONTNEED)` zeroes the range.
- **`epoll` and `eventfd`** (`posix_shim.c`). Go's timers and network
  poller run on them. `EPOLLET` is emulated (see that section's comment).
  Plain level-triggered reporting would make the netpoller spin.
- **Small stubs**: `gettid`, `getpid`, `sched_getaffinity` (one CPU),
  `prlimit64`/`getrlimit`/`setrlimit`, `mprotect`, `prctl`.

Building `posix_shim.c` with `POSIX_SHIM_CFLAGS=-DBMOS_TRACE_ENOSYS` (an
environment variable read by `build-app.sh`, so it works through
`build-go-app.sh` too) prints every syscall that falls through to `-ENOSYS`.
That's the quickest way to find what a new program is missing.

## Memory

`examples/go/hello` boots with `MEMSIZE=6` and the default hot-plug
settings. The app image is about 1.8 MiB. The runtime then reserves a
64 MiB-aligned heap arena, which takes 128 MiB of guest RAM through
hot-plug, but untouched memory costs the host nothing.

## Known gaps

- **Networking is untested.** `epoll` is in place and sockets go through
  `net_shim.c`, but `net`/`net/http` haven't been run yet.
- **Idle CPU.** Once there is more than one thread, `poll_wait()` (which
  `epoll_wait()` uses) wakes every 1 ms instead of sleeping until an
  interrupt (see `thread_shim_io_wait_until()`). Go always runs several
  threads, so an idle Go app polls.
- **No `os.Args`/environment from Go's point of view beyond what
  `crt0.c` passes** (`argv` from Firecracker's `args=` token, empty
  environment).
- **Signals.** `os/signal` won't see anything; see `thread_shim.c`'s
  "Signals" section for what this port delivers at all.
