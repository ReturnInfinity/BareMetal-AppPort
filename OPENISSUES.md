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
- **`getpid`/`getppid`/`sysinfo`/`times` are unimplemented.** Anything
  that queries "what process am I" will get `-ENOSYS`. `uname` *is*
  implemented (`posix_shim.c`), returning fixed, honest placeholder
  values (`sysname` "BareMetal", `machine` "x86_64", etc.) — unlike
  `getpid`/`getuid`, a hostname/machine type is real, expected input to
  ordinary code (e.g. Python's `platform` module), not something this
  port's single-process model makes meaningless.
- **Signal delivery (`kill`/`tkill`/`tgkill`/`rt_sigaction`/
  `rt_sigprocmask`) is real, but software-raised only, and delivered at
  timer-tick granularity, not truly asynchronously.** `thread_shim.c`'s
  "Signals" section reuses the same `b_system(CALLBACK_TIMER, ...)`
  injection point that already drives thread preemption (see below): a
  currently-running thread's pending, unblocked signals are checked and
  its handler called directly, as an ordinary C call, on every timer
  tick (~1ms) — no real hardware signal frame or `rt_sigreturn` needed.
  A thread parked in `thread_shim_futex()`'s `FUTEX_WAIT` is woken early
  (`-EINTR`) the same way; `sleep_until_ns()` (`posix_shim.c`, backing
  `nanosleep`/`clock_nanosleep`) and every blocking call in
  `net_shim.c` (`accept`/`connect`/`recv`/`send`) check for this too, so
  a caught signal without `SA_RESTART` now genuinely interrupts them
  with real remaining time reported back, rather than always running to
  completion. See `signals.c` for a program exercising all of this.
  Known limits:
  - **No hardware-fault-derived signals.** A real `SIGSEGV`/`SIGFPE`/
    `SIGBUS` from an actual trap needs BareMetal kernel-side work to
    translate a fault into this mechanism, not attempted here — only
    software-raised signals (`kill`/`raise`/`pthread_kill`) work.
  - **`SA_SIGINFO` handlers get a real `siginfo_t`/`ucontext_t`, but
    with an all-zero `uc_mcontext`** — there's no real interrupted-
    hardware-frame to expose the way a genuine signal trampoline would.
    Handlers that only look at the signal number (the common case) are
    unaffected.
  - **`sigaltstack` is accepted and ignored** — a handler always runs on
    the victim thread's own stack, which the timer tick was already
    running on.
  - **No timer-/alarm-generated signals** (`alarm()`/`setitimer()`
    aren't wired up) — every signal seen here has to be raised by some
    thread explicitly.
- **Threads (`pthread_create` and friends) work, but are cooperative
  user-level threads, not real kernel threads** (`thread_shim.c`,
  wired into `posix_shim.c`'s `SYS_clone`/`SYS_futex`/`SYS_sched_yield`
  cases). `SYS_clone` builds a second call stack and context by hand
  rather than trapping into a real `clone(2)` (there's no kernel task
  table to fork into on this port), and `b_system(CALLBACK_TIMER, ...)`
  drives a real round-robin timer tick between them — see
  `thread_shim.c`'s file header for the full design and why that still
  gives genuine concurrency (a thread blocked inside another blocking
  `read()`/`recv()`/`nanosleep()` doesn't starve everyone else) despite
  there being exactly one core. musl's own `pthread_mutex_t`/
  `pthread_cond_t`/`pthread_rwlock_t`/`pthread_barrier_t`/
  `pthread_spinlock_t`/TSD all work unmodified on top of the
  `SYS_futex` shim (`FUTEX_WAIT`/`FUTEX_WAKE`/`FUTEX_REQUEUE` only —
  the PI-futex ops are `-ENOSYS`, matching this port not supporting
  `PTHREAD_PRIO_INHERIT`/`PTHREAD_MUTEX_ROBUST`). See `threads.c` for
  a program exercising all of the above. Known limits:
  - **Fixed thread table** (`THREAD_SHIM_MAX_THREADS` = 32 concurrent
    threads, `thread_shim.c`), same style as the EXT2/socket tables.
  - **`pthread_cancel` works for the cases this port's own blocking
    primitives cover** (a target parked in `pthread_mutex_lock`/
    `pthread_cond_wait`/`pthread_join`, or a running target checked at
    its next cancellation-point call), via the signal delivery above —
    `pthread_kill(t, SIGCANCEL)` now actually reaches `t`. musl's own
    `__syscall_cp_asm` pre-call cancel check plus the `-EINTR` path
    handles the rest without needing this port to replicate musl's
    PC-redirect trick (`cancel_handler()`'s cancellation-point-address
    check never matches here — see `thread_shim.c`'s "Signals" section
    for why that's fine, not a bug). Not stress-tested beyond
    `signals.c`'s single cross-thread case.
  - **`sched_setscheduler`/explicit `pthread_attr_setschedpolicy`
    aren't wired up** (`-ENOSYS`) — every thread is scheduled the same
    (timer-tick round-robin, no priorities); a program that asks
    `pthread_create` for an explicit scheduling policy/priority via
    `pthread_attr_setschedpolicy()` won't get one.

## Missing common syscalls

Not implemented (all fall through to `-ENOSYS`):
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
  `dns_shim.c` provides `gethostbyname()` itself (IPv4 only, backed by
  a single static `struct hostent` with no locking -- not safe to call
  from more than one thread at a time now that threads exist, see the
  "Process model" section above), backed by lwIP's `dns_gethostbyname()`
  rather than musl's own resolver: musl's reads `/etc/resolv.conf`, which nothing writes
  on this port's EXT2 image, so it'd fall back to querying
  `127.0.0.1` instead of the DNS servers `net_glue.c` actually
  configures (the fc `ip=` param's optional `dns0-ip`/`dns1-ip`
  fields, DHCP's DNS option, or -- if neither provides one -- a
  fallback to 8.8.8.8/1.1.1.1; see `dns_apply_fallback()`).
- **Blocking socket calls (`connect`/`accept`/`send`/`recv`) default
  to a 30s timeout, but it's now per-socket and overridable via
  `setsockopt(SO_RCVTIMEO/SO_SNDTIMEO)`** (`net_shim.c`) — `accept`/
  `recv`/`recvfrom` bound on `SO_RCVTIMEO`, `connect`/`send` on
  `SO_SNDTIMEO` (`connect` has no dedicated timeout option in real
  POSIX; this port ties it to `SO_SNDTIMEO` as the closest fit). A
  value of `{0, 0}` means indefinite POSIX blocking, same as real
  `SO_RCVTIMEO`/`SO_SNDTIMEO` semantics — safe to offer now that
  threads exist (see "Process model" above): a call parked here no
  longer risks wedging the whole VM, and another thread can still
  break it out early via `pthread_kill()`/`pthread_cancel()`
  (delivered through the same signal mechanism described above). The
  30s default remains for any caller that never touches
  `setsockopt()`.
- **No non-blocking mode.** `SOCK_NONBLOCK`/`O_NONBLOCK` and
  `MSG_DONTWAIT` are accepted but not honored — every socket call
  blocks (up to the timeout above) regardless.
- **`setsockopt`/`getsockopt` only honor `SO_RCVTIMEO`/`SO_SNDTIMEO`**
  (above); every other option — `SO_REUSEADDR`, `TCP_NODELAY`, etc. —
  is still an accept-and-ignore stub with no effect.
- **Unaccepted connections are no longer leaked on listener `close()`.**
  Closing a listening socket now walks its accept queue
  (`net_shim.c`'s `close_queued_conn()`) and, for each connection lwIP
  already handed over (`tcp_accepted()` called) but the app never
  `accept()`ed, closes its `tcp_pcb` (falling back to `tcp_abort()`,
  same as a normal connected socket's `close()`) and frees the `bsock`
  slot `on_accept()` allocated for it — otherwise both would sit there
  forever, eventually exhausting the fixed socket table (`SOCK_MAX`,
  below) even though the app closed every fd it ever saw.
- **Single NIC, hard-coded interface id 0** — matches the current
  kernel (`init_net` only brings up one virtio-net device), so this
  isn't a shim limitation so much as a note that multi-NIC support
  would need kernel-side work first.
- **Fixed socket table** (`SOCK_MAX` = 16 concurrent sockets).
- **`getsockname()`/`getpeername()` are implemented** (`net_shim.c`),
  found missing while boot-testing Python's `socketserver.TCPServer`
  (`server_bind()` calls `getsockname()` right after `bind()` to learn
  the port a bind to port 0 actually picked — see "Python" below) but
  not specific to Python. No new state needed: `tcp_pcb`/`udp_pcb`
  (both built on lwIP's `IP_PCB` base) already track
  `local_ip`/`local_port` once `bind()` succeeds and
  `remote_ip`/`remote_port` once a connection is established — these
  just read them back out. `getpeername()` returns `-ENOTCONN` on a
  `SOCK_DGRAM` socket or a `SOCK_STREAM` socket that isn't connected
  yet, matching real POSIX semantics for an unconnected socket.

## libcurl (`port/curl_port/`)

curl 8.21.0 is vendored unmodified (`scripts/get-curl.sh`); the config
this port builds it with (`port/curl_port/curl_config.h`, see its own
file header for how each choice was derived) narrows it down along the
same lines as everything else here:

- **HTTP and HTTPS only.** No FTP/FILE/TELNET/TFTP/RTSP/DICT/GOPHER/
  LDAP(S)/POP3/IMAP/SMTP/MQTT/WebSockets/IPFS -- all disabled at
  compile time (`CURL_DISABLE_*`).
- **Certificate verification is on**, same stance as `tls_shim.c` (see
  its file header): `CURLOPT_SSL_VERIFYPEER`/`VERIFYHOST` are on in
  `curltest.c`/`wiki_discord.c`, checked against disk.img's CA bundle
  (`CURLOPT_CAINFO`, `scripts/get-cacert.sh`, installed by
  `port/mbedtls_port/install-cacert.sh`) if it's there, or the same
  bundle compiled directly into the binary (`CURLOPT_CAINFO_BLOB`,
  `port/mbedtls_port/gen-cacert-data.sh`) if it's not -- e.g. no disk
  attached at all.
- **No proxy support** (`CURL_DISABLE_PROXY`) and **no alt-svc/HSTS/
  netrc** (all file- or wall-clock-expiry-based, neither of which fits
  this port well -- see `curl_config.h`).
- **Resolves via `gethostbyname()` only**, same as `crawler.c`/
  `https_crawler.c`/`tls_shim.c` -- `HAVE_GETADDRINFO` is deliberately
  left undefined even though musl itself links a real `getaddrinfo()`,
  for the same reason `dns_shim.c` shadows `gethostbyname()` in the
  first place (nothing writes `/etc/resolv.conf` on this port's EXT2
  image). IPv4 only, matching lwIP's `LWIP_IPV6=0`.
- **No threading support in libcurl itself** (`HAVE_THREADS_POSIX`
  undefined), even though the port now has real threads (see "Process
  model" above) -- curl's own multi-thread machinery (its c-ares
  resolver thread pool, `curl_multi_wakeup()` for interrupting a wait
  from another thread) is unneeded here since `dns_shim.c` shadows its
  resolver anyway; `curl_multi_wakeup()` is consequently a no-op,
  irrelevant to the single easy-handle usage this port's apps actually
  do (whether from one thread or several).
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
- **No SQLite-level thread safety** (`SQLITE_THREADSAFE=0`) -- a single
  `sqlite3*` connection (or the mutex-free VFS state `sqlite_vfs.c`
  shares across connections) still isn't safe to use from more than
  one thread at a time, even though the port now has real threads (see
  "Process model" above); `sqltest.c` only ever has one thread open a
  database at all.

## Python (`port/python_port/`)

CPython 3.14.7 is vendored unmodified (`scripts/get-python.sh`);
`port/python_port/pyconfig.h` is this port's hand-written build config
(the role `curl_config.h`/`sqlite_baremetal_config.h` play for
curl/SQLite), and `python.c`/`config_baremetal.c`/
`frozen_encodings_baremetal.c` are this port's own entry point, static
built-in module table, and frozen `encodings` slice in place of
CPython's normal `Programs/python.c`/generated `Modules/config.c`. See
`PYTHON.md` for the full account of how this port works and why each
piece is the way it is -- this section is the condensed version, same
role as every other section here:

- **No dynamic loading** (matches "General" below) -- every module a
  program needs must be statically linked in and registered in
  `config_baremetal.c`'s `_PyImport_Inittab`, the same *static*
  mechanism `Modules/Setup.bootstrap.in`'s own header comment
  describes. No `ctypes` (needs `dlopen`), no installable third-party
  packages, no compiled `.so` extension modules ever.
- **A curated set of C extension modules are built in**: the
  `Modules/Setup.bootstrap.in` mandatory set (`posix`, `_thread`,
  `_io`, `_signal`, `_codecs`, `_collections`, `itertools`, `_sre`,
  `time`, `_weakref`, `_abc`, `_functools`, `_locale`, `_operator`,
  `_stat`, `_symtable`, `_typing`, `_tracemalloc`, `gc`, ...) plus
  `_socket`, `select`, `math`, `_struct`, `binascii`, `_random`,
  `_sha2`, `array`, and `unicodedata` (`config_baremetal.c`'s
  `_PyImport_Inittab`) -- the latter group was added specifically to
  unblock the `http.server`/`socketserver`/`threading` closure below.
  Still nothing from `Modules/Setup.stdlib.in` beyond that
  (`_datetime`, `_json`, `_decimal`, ...) -- each would need its own
  `HAVE_*` audit and a `setup.sh` addition, none attempted.
- **A curated slice of the pure-Python standard library is present**,
  not the whole `Lib/` tree -- a handful of bootstrap modules frozen
  into the binary (`importlib`/`os`/`site`/etc, plus
  `encodings`/`encodings.aliases`/`encodings.utf_8`) plus two traced
  dependency closures installed onto the EXT2 disk image's `/pylib`
  (`install-stdlib.sh`): the original `json`/`re`/`collections`/
  `encodings.ascii` closure, and a larger closure added later for
  `import http.server, socketserver, threading` (`socket`,
  `datetime` -- via a `_pydatetime` fallback that avoids needing
  `_datetime` -- `email.*`, `html.*`, `http.*`, `urllib.parse`,
  `random`, `ipaddress`, ...; `ssl`/`zlib` were skipped by relying on
  their own try/except `ImportError` fallback already in the stdlib
  source; see `install-stdlib.sh`'s comments for the full list and
  reasoning). `import` of anything else -- a different codec, most of
  the rest of the standard library -- still fails with
  `ModuleNotFoundError` until its own files are added to `/pylib` the
  same way.
- **No hash randomization** -- equivalent to `PYTHONHASHSEED=0`,
  standard and documented, not a bug: `Python/bootstrap_hash.c` treats
  finding no entropy source as a fatal boot error otherwise. A real
  `getrandom()` now exists (see "Missing common syscalls" above), but
  `pyconfig.h` doesn't define `HAVE_GETRANDOM` and this hasn't been
  wired up/tested from the Python side yet, so `dict`/`set` iteration
  order and `hash()` values remain deterministic, not random, for now.
- **Every other cut this port already makes elsewhere applies the same
  way to `os.*`**: no `os.fork`/`exec*`/`subprocess`/`multiprocessing`
  (no process model), no `os.chmod`/`chown`/`umask` (no uid/gid model),
  no `os.pipe`/`dup`/`dup2`. None of these are Python-specific --
  `Modules/posixmodule.c` just puts a name on each syscall this port
  already doesn't have. `os.kill`/`signal.raise_signal`/`signal.signal`
  itself now have a real syscall underneath (see "Process model"
  above) -- CPython's own C-level `signal_handler()` trampoline
  (`Modules/signalmodule.c`) is an ordinary `sa_handler(int)` callback
  like any other, so it should reach Python-level handlers the same way
  a plain C program's would, but this hasn't actually been exercised
  from Python (`main_test.py` predates it).
- **`_thread`/threading is real but only smoke-tested.**
  `_thread.start_new_thread()` plus `Lock` work correctly (see
  `port/python_port/main_test.py`), backed by the same
  `thread_shim.c` cooperative pthreads as `threads.c`. Not stress-
  tested -- `thread_shim.c`'s `THREAD_SHIM_MAX_THREADS = 32` fixed
  table under real concurrent load (more than a thread or two, real
  contention) is unexercised from Python.
- **VM RAM: `python.app` needs noticeably more than the ~4 MiB other
  apps here get by with.** The flat binary alone (statically-linked
  interpreter + frozen bytecode + every other port's library code, all
  still linked in the same way curl/SQLite/etc are into every app) is
  several MB, before the heap even starts -- 256 MiB was used for
  testing, not tuned down to a real minimum. `posix_shim.c`'s "no
  growth beyond the initial `b_system(FREE_MEMORY)` ceiling" caveat
  (see "Heap" above) is a real, not just theoretical, concern for
  Python specifically.

## C++ (`port/cpp_port/`)

The host's own `g++`/libstdc++.a (Ubuntu's, built against glibc) is
reused directly as a freestanding compiler + static archive, the same
"host compiler, our own musl at final link" trick `build-app.sh`
already uses for C -- see `CPP.md` for the full account of why this
works and everything it took (a real bug in `port/c.ld`'s own
`.init_array` handling, found via this work, is also fixed there; it
affects every language, not just C++, though nothing before C++
happened to trip it).

- **Real C++ exceptions don't work, by design.** Apps build with
  `-fno-exceptions -fno-rtti` (`try`/`catch`/`throw` are compile
  errors in app code), and `port/cpp_port/cxxabi_stub.cpp` overrides
  every `std::__throw_*()` helper libstdc++'s containers/iostream/etc
  call internally to abort (via `b_output()`/`b_exit()`) instead of
  really throwing -- confirmed via `std::vector::at()` out-of-range.
  The one known residual risk: a `throw` compiled directly into some
  libstdc++.a internal *without* going through a `std::__throw_*()`
  helper (locale/facet edge cases are the most likely place) would
  still reach the real (but non-functional -- no `.eh_frame` exists
  anywhere in this port) unwinder and abort there instead, with a
  less specific message.
- **Locale is always "C"/POSIX**, always -- there's no locale data on
  disk anywhere in this port for a real `setlocale(LC_ALL, "xx_YY")`
  to load. `<iostream>`'s ctype/classification backing
  (`port/cpp_port/glibc_ctype_shim.c`, standing in for glibc's
  `__ctype_b_loc()`/etc, which musl has no equivalent of at all) is
  correct for every byte value under that one locale, same as running
  under `LC_ALL=C` on a real Linux box.
- **`std::thread`/`std::mutex` not yet exercised** the way Rust's
  `std::thread`/`Arc<Mutex<_>>` was (see `RUST.md`) -- libstdc++'s
  `<thread>`/`<mutex>` ultimately call the same real `thread_shim.c`
  pthreads Rust/Python already use, so there's no known reason it
  wouldn't work, just not yet verified end to end.
- **`std::filesystem`/`std::regex`/wide-stream (`std::wcin`/file-based
  `std::wifstream` etc) are unverified.** `std::wcout`/`std::wcin`'s
  in-memory construction is real (see `CPP.md` -- `ios_base::Init`
  constructs the wide streams unconditionally alongside the narrow
  ones, which is what surfaced the `.init_array` bug), but nothing has
  actually exercised wide-character *output* end to end, and
  `std::filesystem`/`std::regex` haven't been tried at all -- both
  pull substantial additional libstdc++.a surface (real syscalls via
  `std::filesystem`, ICU-adjacent tables for `std::regex`) that may
  need more shims the same way `<iostream>` did.

## Zig (`port/zig_port/`)

Zig cross-compiles to `x86_64-linux-musl` natively (`zig build-obj
-target x86_64-linux-musl -lc`), with ordinary file I/O and most of
`std.c`/`std.posix` routing through real, interceptable calls into
this port's own patched musl -- no shim needed for any of that,
simpler than either the C++ or Rust ports. See `ZIG.md` for the full
account of why this works and everything it took.

- **Requires `BareMetal-Firecracker`'s `zig` branch, not `main`.**
  Ordinary Zig code (`std.debug.print`, `std.Thread`, ...) routinely
  emits the raw `syscall` x86 instruction directly, with no libc call
  in sight for this port's patched musl to intercept the way it does
  for Rust's `libc` crate or C++'s libstdc++.a. `BareMetal-Firecracker`'s
  `zig` branch adds real kernel-side support for that raw instruction
  (`EFER.SCE`/`STAR`/`LSTAR`/`SFMASK`, a new `int_syscall_fast` entry
  stub -- see `ZIG.md`'s "SYSCALL/SYSRET" section for the full account,
  including two real bugs -- a stack-alignment bug and a missing
  register-preservation bug -- found and fixed while getting it
  working). Built against a `main`-branch kernel instead, the exact same
  code crashes: `Exception 0x06 (UD)`, confirmed by an actual boot
  before the kernel fix existed.
- **`std.debug.print` and `std.Thread`/`Mutex`/`Futex` are verified
  working end to end** against that kernel branch -- a real
  `std.debug.print` call (plain and formatted) and a real
  `std.Thread.spawn`/`join` + atomics test both link, boot, and produce
  correct output repeatedly (see `ZIG.md`'s "Verified so far"). Real
  threads route through the same `thread_shim.c` cooperative pthreads
  every other language's threading already uses -- `std.Thread` never
  went through libc's pthread API to get there, but the kernel-side
  `syscall` support means it doesn't need to anymore.
- **Debug-mode (`-O` omitted) builds don't link.** Zig's Debug-mode
  codegen for `std.Io.Writer`'s internals emits some anonymous
  constant/vtable references as absolute 32-bit relocations that can't
  reach this port's high-canonical load address -- "relocation
  truncated to fit" at link time. `-O ReleaseSmall` (required regardless,
  see `build-zig-app.sh`) avoids this; an LLVM/Zig code-model
  limitation, not something this port's build can work around.
- **`std.net.Stream.write()`/`writeAll()` don't work -- use
  `std.posix.write()` instead.** Zig 0.15's `Io.Writer`-backed
  `Stream.write()` sends via `sendmsg()`, and this port's `posix_shim.c`
  has no `SYS_sendmsg`/`SYS_recvmsg` case (`sys_writev`/`sys_readv` do
  handle socket fds correctly, `sendmsg`/`recvmsg` are a separate,
  unhandled syscall pair) -- silently drops to `-ENOSYS`, which Zig maps
  to `error.Unexpected`. Found and worked around building
  `examples/zig/webserver/webserver.zig` (see `ZIG.md`'s "Known gaps"
  for the full account and the workaround). A real fix would add
  `SYS_sendmsg`/`SYS_recvmsg` to `posix_shim.c`/`net_shim.c`; not
  attempted.
- **Not exhaustively audited beyond `std.debug.print`/`std.Thread`/basic
  TCP server sockets.** The rest of `std` (more of `std.fs`,
  `std.process`, UDP, ...) is presumed to work the same way (real libc
  calls, or now real syscalls via `int_syscall_fast`) but hasn't been
  individually exercised.
- **No real exceptions/unwinding needed, unlike C++/Rust** -- Zig has
  no unwinding runtime at all; a panic always aborts, so no
  `unwind_stub.c`/`cxxabi_stub.cpp`-equivalent stub was needed.
- **No Zig-idiomatic `pub fn main() !void` entry point.** This port
  supplies its own `crt0.c`/`_start`, not Zig's own `start.zig` -- apps
  must export a C-ABI `main` themselves (see `ZIG.md`/
  `examples/zig/hello/hello.zig`).
- **Locale is always "C"/POSIX**, same posture as every other language
  here.

## QuickJS (see `QUICKJS.md`; no `port/quickjs_port/` needed)

- **No `quickjs-libc.c`, by design.** Only the four core-engine files
  are built (`quickjs.c`/`libregexp.c`/`libunicode.c`/`dtoa.c`) -- no
  `js_std_*`/`js_os_*` helpers, no module loader, no `console.log`. An
  app supplies its own globals via `JS_NewCFunction`.
- **No custom allocator wired to this port's heap.** Uses quickjs-ng's
  own default `malloc`/`realloc`/`free`-backed `JSMallocFunctions`
  (`JS_NewRuntime()`, not `JS_NewRuntime2()`) -- shares the same bump-
  allocator heap arena every other language's `malloc` already draws
  from (see "Heap" above), with the same headroom caveats.
- **No JIT, so no `mprotect`/W^X gap to begin with** -- quickjs-ng is a
  pure bytecode interpreter; this is a property of the engine, not
  something this port had to work around.
- **`Atomics.*` opcodes use raw `__atomic_*()` GCC builtins**
  (`-DGCC_BUILTIN_ATOMICS`, see `QUICKJS.md`) instead of `<stdatomic.h>`
  (musl 1.2.6 doesn't ship one) -- compiles and runs fine single-
  threaded; never exercised from more than one cooperative thread.
- **MEMSIZE needs bumping well past the 4MiB Firecracker default** to
  boot at all -- see `QUICKJS.md`'s boot-testing note.
- **No DOM/HTML/CSS/`fetch`** -- this is just the JS engine. See
  "Headless browser bindings" below for the DOM<->QuickJS glue layer
  built on top of this and lexbor.

## Lexbor (see `LEXBOR.md`; no `port/lexbor_port/` needed)

- **`fs.c` (directory listing / whole-file-read helpers) is not
  built.** Nothing in the built modules calls `lexbor_fs_*` (confirmed
  by grep), so this isn't reachable yet -- but if some future module
  addition ever needs it, it would require real `opendir`/`readdir`
  support in `posix_shim.c`, which doesn't exist (see "Missing common
  syscalls" above doesn't even list these -- they're not implemented
  at all, not just falling through to `-ENOSYS`).
- **`encoding`/`url`/`unicode`/`punycode`/`style`/`engine` modules not
  built**, by scope choice, not a blocker -- see `LEXBOR.md`'s "What's
  vendored" for why each was left out and what would need adding first
  (real-world charset detection, relative-URL resolution, CSS cascade/
  layout respectively).
- **MEMSIZE needs bumping well past the 4MiB Firecracker default** to
  boot at all -- see `LEXBOR.md`'s boot-testing note, same story as
  Python/QuickJS.
- **Fetch -> parse pipeline verified working** (`examples/lexbor/
  fetch/fetch.c`): libcurl GET into a buffer, straight into
  `lxb_html_document_parse`, real `<title>`/`<a>`-count queries
  against the result. No new networking setup was needed -- DHCP
  fallback and `baremetal.sh`'s existing `tap0` auto-attach already
  covered it. See `LEXBOR.md`'s "Fetch + parse example" section.

## Headless browser bindings (see `BROWSER.md`; no new library port)

- **DOM<->QuickJS binding layer done for a first pass** -- `console.log`,
  `document.querySelector()`, `Element.textContent`/`.tagName`/
  `.getAttribute()`, and inline `<script>` execution (in document
  order, skipping `src`-bearing scripts, with exception reporting that
  doesn't abort the rest of the page). Hand-written glue, not a vendored
  library -- see `BROWSER.md`'s "What's bound" for the exact API
  surface and why each piece works the way it does.
- **No `getElementById`/`querySelectorAll`/DOM mutation, no external
  `<script src>` fetching, no event loop/timers/`fetch()` from JS** --
  all explicit scope cuts for this pass, not blockers; see `BROWSER.md`'s
  "Explicit non-goals / follow-ups".
- **Live fetch + live script execution now wired up and boot-tested
  against four real URLs** -- `examples/lexbor/browser-fetch/
  browser_fetch.c` (new; doesn't modify `fetch.c` or `browser.c`).
  Every real page tried failed, each for a different documented
  reason: no `window` (httpbin.org), a missing global from a correctly-
  skipped external script (`$`/jQuery, iana.org), and (originally) a
  fixed 32KB fetch buffer silently truncating a larger page mid-
  document (wikipedia.org, 119KB) -- see `BROWSER.md`'s new section for
  the exact boot logs.
- **Fixed:** the 32KB fetch-buffer cap above. `fetch.c` and
  `browser_fetch.c` both moved from a fixed `response_buf[32*1024]` to
  a `realloc`-doubling growable buffer (see `BROWSER.md`'s "Growable
  fetch buffer" section) -- boot-verified against Wikipedia's full
  119KB body and a regression check against `example.com`'s default
  case. No remaining fixed-size cap on fetched response bodies.
- **Fixed:** the "no `window`" gap above, partially -- `window` is now
  a real global (an alias for the global object, `window ===
  globalThis`, same as a real browser). Re-tested against all three
  pages that had thrown `window is not defined`: `httpbin.org`'s
  inline script now runs with zero exceptions; `wikipedia.org`'s
  `window is not defined` is gone but two other real gaps surface in
  its place (`document.documentElement` unimplemented, and a
  `window.<method>()` call throwing `TypeError: not a function` since
  `window` has none of a real `Window` interface's methods yet);
  `iana.org`'s unrelated `$ is not defined` is unaffected, as expected.
  See `BROWSER.md`'s "Window stub" section for the exact boot logs.
- **Fixed: external `<script src>` fetching, including a real crash
  bug found and root-caused along the way.** lexbor's `url` module is
  now vendored (see `LEXBOR.md`); `browser_fetch.c` resolves and
  fetches external scripts for real (verified against jQuery and
  Swagger UI's real 1.4MB bundle -- both fetch and run correctly,
  throwing an honest DOM-gap exception each). This initially crashed
  every real page tried with `Exception 0x13(GP)` on the *second*
  external script -- root-caused (not a kernel bug, not networking, not
  QuickJS/lexbor memory pressure) to `resolve_script_url()` calling
  `lxb_url_memory_destroy()` on each resolved URL, which tears down
  lexbor's *entire* memory arena rather than just that one allocation,
  leaving the reused `g_url_parser`'s arena a dangling pointer for the
  next resolution. One-line fix: `lxb_url_destroy()` instead (frees
  just that object, leaves the arena intact) -- lexbor's own `url.h`
  documents this exact gotcha verbatim. Boot-verified against
  `iana.org`/`httpbin.org` (both previously crashing, now run to
  completion with real per-script exceptions) and a full regression
  set (`example.com`, `wikipedia.org`, the static `browser.c` test).
  Also found and fixed along the way, independent of the crash: a
  `curl_global_cleanup()` ordering bug (called after only the first
  fetch, leaving later `curl_easy_init()` calls in undefined-behavior
  territory per libcurl's own contract). A kernel-side stack-layout
  theory was investigated and disproven along the way --
  `BareMetal-Firecracker` ended this investigation unmodified. See
  `BROWSER.md`'s "External script fetching" / "The crash, root-caused"
  sections for the full story and boot logs.
- **Fixed:** the two `wikipedia.org` DOM gaps from the "window stub"
  entry above -- `document.documentElement`/`.body`/`.head` (direct
  lexbor accessors, not a selector query), `Element.className` (a
  dedicated getter, `""` not `null` when absent -- matches real DOM
  semantics, unlike `getAttribute("class")`), and
  `window.addEventListener`/`removeEventListener` as honest no-op
  stubs (accepted, never invoked -- there's still no event loop).
  Re-tested against `wikipedia.org`: the `className`/`documentElement`
  `TypeError` is gone, the inline script now runs further before
  hitting a new, later `TypeError: not a function`; its two external
  scripts (not attempted by any prior round's write-up) also fetch and
  run with real distinct errors, no crash. Full regression sweep
  (`example.com`, `httpbin.org`, `iana.org`, static `browser.c`) shows
  no regressions -- see `BROWSER.md`'s "DOM properties and Window stub
  methods" section for exact boot logs. `getElementById`/
  `querySelectorAll`/DOM mutation, external-script-triggered
  `document.write`, and any `Window` interface method beyond the two
  event-listener stubs remain open, addable follow-ups.
- **MEMSIZE needs bumping well past the 4MiB Firecracker default**,
  same story as every other QuickJS/lexbor example.

## General

- **No dynamic linking, by design** — everything is statically linked
  into one flat binary at a fixed load address. Not a gap so much as
  a permanent constraint of this environment (no ELF loader, no
  syscall trap to service `mmap`-based `dlopen`).
