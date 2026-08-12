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
- `clock_gettime` supports `CLOCK_MONOTONIC`/`CLOCK_MONOTONIC_RAW`/
  `CLOCK_MONOTONIC_COARSE` (`posix_shim.c`), backed by
  `b_system(TIMECOUNTER)` (nanoseconds since boot, the same source the
  heap/TLS/lwIP code already used internally), and `CLOCK_REALTIME`/
  `CLOCK_REALTIME_COARSE`, backed by `b_system(WALLCLOCK)` (seconds
  since the Unix epoch, read from the RTC at boot — no sub-second
  component). `time()` and `gettimeofday()` both go through musl's
  `clock_gettime(CLOCK_REALTIME, ...)` so they work too (see
  `clock.c`). Other `clk_id`s (e.g. `CLOCK_PROCESS_CPUTIME_ID`) still
  return `-EINVAL`.
- `nanosleep`/`clock_nanosleep` are implemented (`posix_shim.c`) on top
  of `b_system(SLEEP, ns, 0)`, which HLTs the CPU until the APIC timer
  fires rather than busy-spinning. The sleep is chained in
  `NET_POLL_INTERVAL_NS` (10ms) chunks with `net_poll()` called between
  each so lwIP's timers/retransmits keep getting serviced instead of
  stalling for the whole sleep. Since there's no signal delivery on this
  port (see below), a sleep can never legitimately be interrupted early,
  so `rem`/`remain` is always left zeroed rather than tracking real
  remaining time. `clock_nanosleep`'s `TIMER_ABSTIME` deadline is exact
  for `CLOCK_MONOTONIC` (its timeline *is* `TIMECOUNTER`), but there's
  no wall-clock↔`TIMECOUNTER` conversion wired up yet, so a
  `CLOCK_REALTIME` absolute deadline is treated the same way for
  now — fine for "sleep until roughly now plus a bit", wrong otherwise.
- `getrandom` — nothing backs `/dev/urandom`-equivalent randomness for
  application code (musl's own internal entropy needs, e.g. the stack
  canary and mallocng's hardening secret, are seeded via `crt0.c`'s
  `fill_random()` using `rdrand`/`rdtsc`, but that path isn't exposed
  as a syscall).
- `epoll_*` — no way to multiplex across multiple fds and learn
  *which* is ready first. `poll`/`select` themselves are implemented
  (`posix_shim.c`), but not as a real wait: every fd this port
  recognizes is reported ready immediately for whatever of read/write
  the caller asked about, with the real blocking then happening for
  real inside whichever `read()`/`write()`/`recv()`/`send()` follows
  (every one of those already blocks synchronously up to its own
  timeout regardless). Good enough for libcurl's easy-interface
  transfer loop (the reason these exist at all) and anything else
  driving one blocking connection at a time; a program juggling
  several fds to learn which one has data first won't get that.
- `pipe`/`pipe2`/`dup`/`dup2`/`dup3`/`socketpair` — no in-process fd
  duplication, pipes, or `AF_UNIX` socket pairs between fds.
- `chmod`/`chown`/`umask` — no concept of permissions or ownership on
  this port, even though lwext4 itself supports a mode/owner per inode
  (see `ext4_mode_set()`/`ext4_owner_set()` in lwext4's `ext4.h`) —
  see the EXT2 section below on why that's a bigger gap than a missing
  syscall. (`getcwd`/`chdir`/`mkdir`/`rmdir` *are* wired up now, via
  `ext4_shim.c`.)

## Heap (`posix_shim.c`)

- **`munmap()`'d memory can't be handed back to the OS, only reused by
  later `mmap()` calls.** `sys_munmap()` (`posix_shim.c`) keeps freed
  ranges on an address-sorted, coalescing free list rather than truly
  unmapping them; `sys_mmap()` checks that list before bumping the
  arena further. This avoids the old failure mode of an mmap/unmap
  loop exhausting the arena, but freed mmap space still can't flow
  back to `brk()`-backed small allocations (see next point) or to the
  OS.
- **No growth beyond the initial `b_system(FREE_MEMORY)` ceiling.** The
  heap size is fixed once, at first use; there's no mechanism to claim
  more RAM even if more becomes available (e.g. if BareMetal's own
  memory management changes).
- **Large (≥128KB) `malloc()`s share the same bump arena as `brk()`.**
  Works, but means a single big allocation can exhaust room that
  smaller `brk()`-backed allocations would otherwise have used; the
  mmap free list (above) doesn't help here since `brk()` never
  consults it.

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
- **Boot-relative timestamps, not wall-clock.** atime/mtime/ctime come
  from `b_system(TIMECOUNTER)` plus a fixed epoch anchor (see
  `ext4_shim.c`'s `EXT4_SHIM_EPOCH_BASE`), not a real RTC-backed clock
  — always non-zero and monotonically increasing with uptime, but not
  meaningful as a real calendar date.
- **`telldir()`/`seekdir()` aren't meaningfully supported.** lwext4's
  directory iterator only supports rewind-to-0, not arbitrary seek/
  tell positions, so `ext4_shim_getdents()` always reports `d_off` as
  0 — only the common `opendir()`/`readdir()`/`closedir()` sequence
  works.
- **Small fixed open-file table** (`EXT4_SHIM_MAX_OPEN` = 32 concurrent
  files/directories across the whole process).

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

## libcurl (`port/curl_port/`)

curl 8.21.0 is vendored unmodified (`scripts/get-curl.sh`); the config
this port builds it with (`port/curl_port/curl_config.h`, see its own
file header for how each choice was derived) narrows it down along the
same lines as everything else here:

- **HTTP and HTTPS only.** No FTP/FILE/TELNET/TFTP/RTSP/DICT/GOPHER/
  LDAP(S)/POP3/IMAP/SMTP/MQTT/WebSockets/IPFS -- all disabled at
  compile time (`CURL_DISABLE_*`).
- **No certificate verification**, same stance and same reason as
  `tls_shim.c` (see its file header): `CURLOPT_SSL_VERIFYPEER`/
  `VERIFYHOST` are off in `curltest.c`. No CA store is vendored, and
  there's still no clock to check a certificate's validity period
  against even if one were.
- **No proxy support** (`CURL_DISABLE_PROXY`) and **no alt-svc/HSTS/
  netrc** (all file- or wall-clock-expiry-based, neither of which fits
  this port well -- see `curl_config.h`).
- **Resolves via `gethostbyname()` only**, same as `crawler.c`/
  `https_crawler.c`/`tls_shim.c` -- `HAVE_GETADDRINFO` is deliberately
  left undefined even though musl itself links a real `getaddrinfo()`,
  for the same reason `dns_shim.c` shadows `gethostbyname()` in the
  first place (nothing writes `/etc/resolv.conf` on this port's EXT2
  image). IPv4 only, matching lwIP's `LWIP_IPV6=0`.
- **No threading** (`HAVE_THREADS_POSIX` undefined) -- matches this
  port being single-threaded throughout; `curl_multi_wakeup()` (for
  interrupting a wait from another thread) is consequently a no-op,
  irrelevant to the single easy-handle, single-threaded usage this
  port's apps actually do.
- **TLS via mbedTLS only** (`USE_MBEDTLS`) -- the same vendored copy
  `tls_shim.c` uses, reached through curl's own `vtls/mbedtls.c`
  instead of `tls_shim.c` itself. This pulled `MBEDTLS_PSA_CRYPTO_C`
  back on in `port/mbedtls_port/baremetal_mbedtls_config.h` (off
  before this) -- curl 8.21.0's mbedTLS backend hard-requires PSA
  crypto calls for RNG/hashing against any mbedTLS >= 3.2.0, with no
  legacy-API fallback; see that file's `MBEDTLS_PSA_CRYPTO_C` comment
  for why enabling it doesn't otherwise change how TLS itself runs.

## SQLite (`port/sqlite_port/`)

SQLite 3.46.1 is vendored unmodified as its own amalgamation
(`scripts/get-sqlite.sh`); `SQLITE_OS_OTHER=1`
(`port/sqlite_port/sqlite_baremetal_config.h`) skips SQLite's own
`os_unix.c` entirely in favor of a small hand-written VFS
(`port/sqlite_port/sqlite_vfs.c`, see its own file header for the
full reasoning) built directly over `posix_shim.c`/`ext4_shim.c`:

- **No WAL.** `SQLITE_OMIT_WAL` plus an `iVersion 1` `sqlite3_io_methods`
  (no `xShmMap`/`xShmLock`/`xShmBarrier`/`xShmUnmap` slots at all) --
  WAL's shared-memory negotiation between connections is meaningless
  with exactly one process, ever, on this port. The default rollback-
  journal mode is unaffected and is what every app gets unless it asks
  for WAL explicitly (which will simply fail).
- **No mmap.** `SQLITE_MAX_MMAP_SIZE`/`SQLITE_DEFAULT_MMAP_SIZE` are both
  0 -- `posix_shim.c`'s `mmap()` is a bump allocator over the same fixed
  heap arena `brk()` draws from, with `munmap()` a no-op (see this
  file's "Heap" section); an mmap'd file view would just be heap this
  port can't get back.
- **Locking is a pure no-op that always succeeds/reports uncontended.**
  Same reasoning as `posix_shim.c`'s `fcntl()` stub: there is never a
  second connection, in this process or any other, for a lock to
  conflict with.
- **No load extension** (`SQLITE_OMIT_LOAD_EXTENSION`) -- no dynamic
  linking on this port at all (see "General" below).
- **No `'localtime'`/`'utc'` datetime() modifiers** (`SQLITE_OMIT_LOCALTIME`)
  -- no timezone database on this port, and `b_system(WALLCLOCK)` (this
  port's only wall-clock source) is already UTC-only.
- **`PRAGMA temp_store` is pinned to memory** (`SQLITE_TEMP_STORE=3`) --
  ordinary TEMP tables/indices and the transient sorters/statement
  journals `ORDER BY`/`GROUP BY`/`CREATE INDEX` etc. use never touch
  disk, regardless of what a program requests. The one on-disk temp
  file this doesn't cover -- a multi-database (`ATTACH`) transaction's
  master journal -- is still handled (`sqlite_vfs.c`'s `ext2Open()`
  invents a name via the same hardware RNG `port/mbedtls_port/
  entropy_hardware_poll.c` uses for mbedTLS), just untested by
  `sqltest.c`, which only ever has one database open.
- **Single-threaded only** (`SQLITE_THREADSAFE=0`), matching this port
  throughout.

## General

- **No dynamic linking, by design** — everything is statically linked
  into one flat binary at a fixed load address. Not a gap so much as
  a permanent constraint of this environment (no ELF loader, no
  syscall trap to service `mmap`-based `dlopen`).
