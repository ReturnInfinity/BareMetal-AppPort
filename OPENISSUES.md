# Open Issues

Known gaps and limitations in the musl libc port (`app/`), lwext4-based
EXT2 file I/O, and lwIP-based networking. Most of these are deliberate
scope cuts made while getting each phase working end to end, not bugs —
they're listed here so they're easy to find again when someone hits
one.

## Process model

- **No `fork`/`vfork`/`clone`/`execve`/`wait4`.** Not wired into
  `posix_shim.c`'s dispatcher at all (falls through to the default
  `-ENOSYS` case). There is exactly one process, ever — matches
  BareMetal's model (an app is `call`ed directly by the kernel/monitor,
  there's no process table), but any program that tries to spawn a
  child (a shell, a `system()` call, a forking server) will fail.
- **No real command-line args or environment.** `crt0.c` fabricates a
  minimal fake initial stack for musl's startup path with `argc=0` and
  an empty `envp`. Programs that read `argv`/`getenv()` will always see
  nothing, regardless of how the app was invoked.
- **`getpid`/`getppid`/`kill`/`uname`/`sysinfo`/`times` are unimplemented.**
  Anything that queries "what process am I" or sends itself a signal
  will get `-ENOSYS`.
- **No signal delivery.** `rt_sigaction`, `rt_sigprocmask`, and
  `sigaltstack` are accepted and silently ignored (`posix_shim.c`) —
  handlers can be registered but will never actually fire, since
  BareMetal has no mechanism to deliver a signal into running app code.
- **Single-threaded only.** `pthread_create` isn't wired up (would need
  a `clone` implementation); `__set_thread_area`/TLS bootstrap works
  and reports `can_do_threads=1` to musl since the FS-base wrmsr
  genuinely succeeds, but there is nowhere to run a second thread.

## Missing common syscalls

Not implemented (all fall through to `-ENOSYS`):
- `clock_gettime`/`gettimeofday`/`nanosleep`/`clock_nanosleep` — no
  wall-clock time exposed to programs yet, only `b_system(TIMECOUNTER)`
  (nanoseconds since boot) used internally by the heap/TLS/lwIP code.
  Any program that calls `time()`, benchmarks itself, or does
  `sleep()` will fail or misbehave.
- `getrandom` — nothing backs `/dev/urandom`-equivalent randomness for
  application code (musl's own internal entropy needs, e.g. the stack
  canary and mallocng's hardening secret, are seeded via `crt0.c`'s
  `fill_random()` using `rdrand`/`rdtsc`, but that path isn't exposed
  as a syscall).
- `poll`/`select`/`epoll_*` — no way to multiplex across multiple fds
  (stdin, an EXT2 file, a socket) in one blocking call. Combined with
  every blocking socket/file op in this port already being a
  synchronous spin-loop internally, this means a program can't
  currently wait on "whichever of these fds is ready first."
- `pipe`/`pipe2`/`dup`/`dup2`/`dup3` — no in-process fd duplication or
  pipes between fds.
- `chmod`/`chown`/`umask` — no concept of permissions or ownership on
  this port, even though lwext4 itself supports a mode/owner per inode
  (see `ext4_mode_set()`/`ext4_owner_set()` in lwext4's `ext4.h`) —
  see the EXT2 section below on why that's a bigger gap than a missing
  syscall. (`getcwd`/`chdir`/`mkdir`/`rmdir` *are* wired up now, via
  `ext4_shim.c`.)

## Heap (`posix_shim.c`)

- **`mmap()` never really unmaps.** `munmap()` is a documented no-op —
  it returns success but the memory is never reclaimed. A program that
  mmaps and unmaps in a loop will exhaust the heap arena.
- **No growth beyond the initial `b_system(FREE_MEMORY)` ceiling.** The
  heap size is fixed once, at first use; there's no mechanism to claim
  more RAM even if more becomes available (e.g. if BareMetal's own
  memory management changes).
- **Large (≥128KB) `malloc()`s share the same bump arena as `brk()`.**
  Works, but means a single big allocation can exhaust room that
  smaller `brk()`-backed allocations would otherwise have used, with
  no way to trade space back.

## EXT2 file I/O (`ext4_shim.c`, `lwext4_port/`)

- **No `access()`/`chmod()`/permission enforcement.** `open()`'s `mode`
  argument is ignored entirely; whatever mode bits are already on an
  inode (or lwext4's own default for newly-created files) are reported
  as-is, but nothing checks them against anything. `chmod()` itself
  could be wired up via lwext4's `ext4_mode_set()`, but real
  enforcement needs a uid/gid model this port has none of anywhere
  (matches the "no process model" cuts above) — not just a missing
  syscall.
- **Block device capacity is a hard-coded upper bound, not the real
  disk size.** There's no `b_system()` call to ask the kernel how big
  the backing drive actually is, so `blockdev_baremetal.c` just
  declares a generous ceiling (`BAREMETAL_BLK_COUNT`, currently 2 GiB)
  for lwext4's own bounds-checking; it needs raising by hand if a
  larger EXT2 image is ever used.
- **Superblock free-space counters only flush on process exit.**
  lwext4 keeps `free_blocks_count`/`free_inodes_count` accurate in
  memory but only writes them back to disk on `ext4_umount()`, not on
  every individual file op — `posix_shim.c`'s `sys_exit()` calls
  `ext4_shim_sync()` to cover this for the normal exit path, but a
  hard crash or power-loss mid-run would still leave those two
  superblock fields stale (harmless and auto-fixed by `e2fsck`, but
  worth knowing about if `e2fsck` ever reports "Free
  blocks/inodes count wrong" after a non-graceful shutdown).

## Networking (`net_glue.c`, `net_shim.c`, `dns_shim.c`)

- **TCP and UDP only, no raw IP sockets.** `socket(AF_INET, SOCK_DGRAM,
  ...)` works (`net_shim.c`), but there's no `listen`/`accept` for UDP
  (`-EOPNOTSUPP`) and no multicast/broadcast support.
- **No IPv6** (`LWIP_IPV6=0` in `lwip_port/lwipopts.h`). Programs must
  use literal IPv4 addresses.
- **`gethostbyname()` only, no `getaddrinfo()`/`gethostbyname_r()`.**
  `dns_shim.c` provides `gethostbyname()` itself (IPv4 only, single
  static `struct hostent` -- not thread-safe, but this port has no
  threads), backed by lwIP's `dns_gethostbyname()` rather than musl's
  own resolver: musl's reads `/etc/resolv.conf`, which nothing writes
  on this port's EXT2 image, so it'd fall back to querying
  `127.0.0.1` instead of the DNS servers `net_glue.c` actually
  configures (the fc `ip=` param's optional `dns0-ip`/`dns1-ip`
  fields, DHCP's DNS option, or -- if neither provides one -- a
  fallback to 8.8.8.8/1.1.1.1; see `dns_apply_fallback()`).
- **All blocking socket calls (`connect`/`accept`/`send`/`recv`) have
  a hard-coded 30s timeout**, not indefinite POSIX blocking. Deliberate
  — there's no way to interrupt or recover a truly stuck call in a
  single-threaded unikernel VM — but it means a legitimately
  slow/idle connection (e.g. a server waiting hours for the next
  client) will be torn down.
- **No non-blocking mode.** `SOCK_NONBLOCK`/`O_NONBLOCK` and
  `MSG_DONTWAIT` are accepted but not honored — every socket call
  blocks (up to the timeout above) regardless.
- **`setsockopt`/`getsockopt` are accept-and-ignore stubs.** Nothing
  like `SO_REUSEADDR`, `SO_RCVTIMEO`, or `TCP_NODELAY` actually takes
  effect.
- **Unaccepted connections are leaked on listener `close()`.** If a
  listening socket is closed while connections are sitting in its
  accept queue (arrived but not yet `accept()`ed), those `tcp_pcb`s
  are never explicitly closed.
- **Single NIC, hard-coded interface id 0** — matches the current
  kernel (`init_net` only brings up one virtio-net device), so this
  isn't a shim limitation so much as a note that multi-NIC support
  would need kernel-side work first.
- **Fixed socket table** (`SOCK_MAX` = 16 concurrent sockets).

## General

- **No dynamic linking, by design** — everything is statically linked
  into one flat binary at a fixed load address. Not a gap so much as
  a permanent constraint of this environment (no ELF loader, no
  syscall trap to service `mmap`-based `dlopen`).
