# Python Port -- Plan (EXPERIMENTAL)

**Status: Phases 1 and 2 done and boot-verified.** A minimal CPython
3.12.8 actually boots as a BareMetal-Firecracker unikernel and runs
Python code -- `python.app`, built by `port/python_port/xbuild-phase1.sh`
(despite the name, now covers both phases -- see that script's own
header), combined into a unikernel via `BareMetal-Firecracker/build.sh`
the same way `1-build.sh` does for a normal app. Phase 1 printed `2`
from `print(1 + 1)`; Phase 2, on top of that, imported `_socket` and
ran a real `socket()`/`bind()`/`close()` round trip through
`posix_shim.c` -> `net_shim.c`. Both exited cleanly under Firecracker
(1 vCPU, 256 MiB -- see "Open questions" below on why more than the
usual 4 MiB was needed). Everything under "Phase 1"/"Phase 2" below is
no longer a plan, it's what was actually done, kept as a record of
*why* each piece is the way it is. Phase 3 is still just a plan.

Investigating whether CPython can run as a BareMetal app now that
`port/thread_shim.c` gives this port real `pthread_*` (see
`OPENISSUES.md`'s "Process model" -> "Threads" section). Short answer:
threading was the one *fundamental* blocker (a GIL-based interpreter
genuinely cannot start without it), and it's gone now. Everything else
below is real but bounded engineering, following the same
vendor-unmodified-source-plus-hand-written-config pattern already used
for curl/SQLite/Mbed TLS/lwIP/lwext4/libsodium -- not a new kind of
problem for this repo, just a much bigger one. This document is that
plan, not a working port: `port/python_port/pyconfig_baremetal.h` is a
first-pass draft covering the macros that are real design decisions,
`scripts/get-python.sh` fetches CPython 3.12.8 unmodified, and neither
is wired into `setup.sh`/`build-app.sh` yet.

## Why this is bigger than curl/SQLite/lwext4

Those each have one job (transfer HTTP, read/write a B-tree, read/write
an EXT2 volume) reached through a fairly narrow OS surface, and each
needed one hand-written config header plus (at most) one shim file
(`tls_shim.c`, `sqlite_vfs.c`, `ext4_shim.c`) to bridge it to
`posix_shim.c`. CPython's `pyconfig.h.in` alone references roughly
300 distinct `HAVE_*`/`WITH_*` macros just from `Modules/posixmodule.c`
+ `Python/thread_pthread.h` + the mandatory bootstrap module set (see
below) -- `posixmodule.c` by itself tries to expose most of POSIX.
`pyconfig_baremetal.h` only takes a real position on the macros tied to
an actual port-design decision (fork/exec, signals, dlopen, chmod/
chown, mmap, clocks, getaddrinfo); the remaining ~150 mechanical ones
(does `<sys/foo.h>` exist, is `off_t` 64-bit, does `struct stat` have
`st_blksize`) are answerable identically to any other x86-64 musl
target and are left for a real build attempt to fill in, the same
empirical way `build-app.sh`'s own `--gc-sections`/`OUTPUT_FORMAT(binary)`
comment describes confirming linker behavior by testing it rather than
reasoning about it in the abstract.

## What's confirmed, from reading CPython 3.12.8's actual source (not
just pyconfig.h.in) against this port's files

- **Threading is a real match, not just "present".**
  `Python/thread_pthread.h` (CPython's only POSIX thread backend) needs
  ordinary `pthread_create`/mutexes/condvars/TSD plus, on the
  `USE_SEMAPHORES` path, POSIX unnamed semaphores (`sem_init`/
  `sem_wait`/`sem_post`). musl's own `sem_*.c` is implemented directly
  against the futex syscall, not against any additional `pthread_*`
  entry point -- so `thread_shim.c`'s existing `SYS_futex` wiring
  (`FUTEX_WAIT`/`WAKE`/`REQUEUE`) should cover it with no new shim code,
  unverified until a build actually links `Modules/_threadmodule.c`.
- **No dynamic loading is CPython's own documented degraded mode, not
  something this port has to invent.** `Python/dynload_stub.c`
  ("necessary stubs for when dynamic loading is not present") already
  ships in CPython for exactly `HAVE_DYNAMIC_LOADING` left undefined.
  `Modules/Setup.bootstrap.in`'s header literally says "Built-in
  modules required to get a functioning interpreter; cannot be built as
  shared!" and lists them: `posix`, `_thread`, `_io`, `_signal`,
  `atexit`, `faulthandler`, `_codecs`, `_collections`, `errno`,
  `itertools`, `_sre`, `time`, `_weakref`, `_abc`, `_functools`,
  `_locale`, `_operator`, `_stat`, `_symtable`, `_typing`,
  `_tracemalloc`. A statically-linked interpreter with just that set is
  a first-class, intended CPython configuration -- not a hack this port
  would be inventing.
- **`socket.getaddrinfo()` can ride on `dns_shim.c` for free.**
  `Modules/socketmodule.c` has its own bundled `getaddrinfo.c` fallback,
  compiled in whenever `HAVE_GETADDRINFO` is left undefined, and that
  fallback is itself built on `gethostbyname()` -- exactly the function
  `dns_shim.c` already provides. Same "shadow the real resolver, it'd
  read a `/etc/resolv.conf` nothing writes" reasoning
  `curl_config.h`/`OPENISSUES.md` already documents for curl. Actually
  reaching the network still needs a small `net_shim.c`-backed
  `socket()`/`connect()`/`send()`/`recv()` shim at the C level (see
  Phase 2) -- `getaddrinfo()` alone doesn't move any bytes -- but the
  DNS half of that work is already done by an existing file.
- **`Objects/obmalloc.c`'s arena allocator is a verified match for
  `posix_shim.c`'s `mmap()`, not just a plausible one.** obmalloc calls
  exactly `mmap(NULL, size, PROT_READ|PROT_WRITE,
  MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)`; `posix_shim.c`'s `sys_mmap()`
  requires `MAP_ANONYMOUS` and rejects file-backed mappings -- the same
  shape. Functional, with the same caveat SQLite/curl/mbedTLS already
  live with (`OPENISSUES.md`'s "Heap" section): total interpreter heap
  is capped at whatever `b_system(FREE_MEMORY)` reports at boot, and
  freed arenas are reused but never shrink the process's footprint.
- **Filesystem is in unusually good shape for this.** `ext4_shim.c` now
  backs real `stat`/`lstat`/`symlink`/`readlink`/`realpath` (`musl`'s
  `realpath()` composes `open`/`lstat`/`readlink`, all real here) --
  better than what curl/SQLite needed. This matters more for Python
  than most C programs: `import` walks `sys.path` doing exactly this
  kind of stat/open/readdir sequence, so once Phase 1 boots, shipping
  real `.py`/`.pyc` files on the EXT2 image and importing them
  normally is plausible, not just frozen-bytecode-only.

## What's a real cut (most of `pyconfig_baremetal.h`)

Everything `OPENISSUES.md` already rules out for every other app here
rules it out for Python the same way, `Modules/posixmodule.c` just has
a name for each one: no `os.fork`/`os.exec*`/`subprocess`/
`multiprocessing` (no process model), no `os.chmod`/`chown`/`umask`
(no uid/gid model), no `os.pipe`/`dup`/`dup2` (not in `posix_shim.c`'s
dispatch table), no `os.kill`/real `signal` delivery (accepted-and-
ignored, same as every other syscall in that category), no `ctypes`
(needs `dlopen`), no ability to import a compiled `.so` extension
module ever (same reason). None of these are surprises -- they're the
same list `OPENISSUES.md` already has, just with the specific Python
symbol names attached. See `pyconfig_baremetal.h`'s own comments for
the macro-by-macro detail and which `OPENISSUES.md` section each traces
back to.

## Phased plan

**Phase 1 -- does it even start. DONE, boot-verified.** What this
actually took, beyond the plan originally written here:

- **A native host CPython 3.12.8** (`build/host-python-build/`, a
  plain `./configure && make` on the build machine, not the target --
  ~15 min, not checked into the repo). Not just for `--with-build-python`
  bookkeeping: CPython's *generated* sources (the pegen parser tables
  in `Parser/parser.c`, the AST node definitions in
  `Python/Python-ast.c`, the bytecode dispatch table in
  `Python/generated_cases.c.h`, and -- the big one -- the frozen
  importlib/os/site/getpath/etc bytecode in
  `Python/deepfreeze/deepfreeze.c`) are architecture-independent C, not
  target-specific. A native build produces every one of them for free;
  Phase 1 never had to regenerate any of it, only recompile the
  existing generated `.c`/`.h` files against musl instead of glibc.
  `build/Python-3.12.8/Python/frozen_modules/*.h` and
  `Python/deepfreeze/deepfreeze.c` were copied over from the host build
  once, by hand.
- **`port/python_port/pyconfig.h`**, not `pyconfig_baremetal.h` alone --
  a full, real ~2000-line `pyconfig.h` generated by taking the host
  build's own real `./configure` output (architecture-level answers --
  `SIZEOF_*`, header presence, struct layout -- apply the same under
  musl) and mechanically applying every `pyconfig_baremetal.h` decision
  on top. `pyconfig_baremetal.h` stays as the annotated *why*;
  `pyconfig.h` is what's actually `#include`'d.
- **`port/python_port/xbuild-phase1.sh`** -- hand-drives `gcc`/`ld`
  directly per file, bypassing CPython's own `./configure`/`Makefile`
  entirely, the same choice `build-app.sh` already makes for curl.
  Compiles PARSER_OBJS+PYTHON_OBJS+OBJECT_OBJS+the frozen/getpath
  glue+`Modules/Setup.bootstrap.in`'s module set (all object lists
  copied straight from the host Makefile), then links against this
  port's `crt0.o`/`posix_shim.o`/`thread_shim.o` plus (found necessary
  at link time, not planned for -- see below) `ext4_shim.o`/
  `net_glue.o`/`net_shim.o`/`dns_shim.o`.
- **`port/python_port/pymain_baremetal.c`** in place of CPython's own
  `Programs/python.c` (`Py_BytesMain`) -- uses the lower-level
  `Py_InitializeFromConfig()` embedding API with a hand-built
  `PyConfig` instead: `site_import=0`, `use_environment=0`,
  `module_search_paths_set=1` with zero real entries (no installed
  `lib/python3.12/` tree on this port yet, see Phase 3), and
  `parse_argv=0` (`crt0.c`'s argv is always empty anyway). Runs
  `PyRun_SimpleString("print(1 + 1)\n")`.
- **`port/python_port/config_baremetal.c`** -- a hand-written
  `_PyImport_Inittab` (CPython's own `Modules/config.c`, normally
  autogenerated by `makesetup` from `Modules/Setup`), diffed directly
  against the host build's real `Modules/config.c`, with `pwd` dropped
  (needs `getpwuid()`, no uid/gid model -- OPENISSUES.md).
- **New gaps found only by actually compiling/linking/booting, not by
  reading source ahead of time** (all now reflected in
  `pyconfig_baremetal.h`'s own comments and `pyconfig.h`):
  - Compiler-provided freestanding headers (`stdatomic.h`, needed by
    `Include/internal/pycore_atomic.h`) aren't musl's job to ship and
    aren't on the `-nostdinc`-restricted include path by default --
    `xbuild-phase1.sh` adds `gcc -print-file-name=include` after
    musl's own `-isystem` entry. curl/mbedTLS/lwIP/SQLite/lwext4 never
    needed this; CPython is the first thing built against this port to
    use C11 atomics.
  - `HAVE_CLOSE_RANGE`, `HAVE_GETRANDOM`(`_SYSCALL`), `HAVE_LINUX_RANDOM_H`,
    `HAVE_LINUX_AUXVEC_H`, `HAVE_LINUX_WAIT_H`, `HAVE_LINUX_MEMFD_H`,
    `HAVE_MEMFD_CREATE`, `PY_HAVE_PERF_TRAMPOLINE` all needed cutting --
    the host build's real Linux glibc `configure` run detects all of
    these (real syscalls/headers on real Linux), and they rode along
    into the merged `pyconfig.h` until compiling/linking actually
    exercised them. Each maps to a real, already-documented cut
    (`OPENISSUES.md`'s Process model section) or a musl sysroot gap
    (no vendored `linux/*.h` uapi headers at all).
  - `HAVE_CLOCK` needed *reinstating* over `pyconfig_baremetal.h`'s
    original cut -- `Modules/timemodule.c`'s own comment says "Python 3
    requires clock() to build" (issue #22624); it's a hard compile-time
    dependency, not just an optional feature.
  - `Modules/getpath.c` needs `-DPREFIX`/`-DEXEC_PREFIX`/`-DVERSION`/
    `-DVPATH`/`-DPLATLIBDIR` (normally supplied by the Makefile from
    `--prefix`/etc) -- given placeholder values since Phase 1's
    `pymain_baremetal.c` never calls `calculate_path()` anyway.
  - `Modules/gcmodule.c` isn't in `Modules/Setup.bootstrap.in` at all
    (it's one of the modules `Modules/config.c.in`'s own "ADDMODULE
    MARKER" mechanism always force-builds, alongside `marshal`/`_imp`/
    `_ast`/`_tokenize`, which *were* already covered by files already
    on the PARSER/PYTHON_OBJS list) -- missed on the first pass, only
    caught at link time (`undefined reference to PyInit_gc`).
  - `posix_shim.c`'s syscall dispatcher references `ext4_shim_*`/
    `net_shim_*` unconditionally, not behind any `#ifdef` -- so even
    though `pymain_baremetal.c` touches no filesystem or network,
    those objects (and `net_glue.o`/`dns_shim.o`/lwIP's/lwext4's own
    vendored objects) still have to be linked in. Not a Phase-1-specific
    problem: `build-app.sh` already links all of this into every app
    regardless of use (see its own link line) -- CPython just
    exercises `fstat()` on stdio's fds during startup where `hello.c`'s
    plain `printf()` never happened to.
  - **Hash randomization is fatal by default with no entropy source.**
    `Python/bootstrap_hash.c`'s `_Py_HashRandomization_Init()` treats
    finding *no* random source (this port has none exposed to app code
    yet -- OPENISSUES.md) as a hard boot failure, not a silent
    fallback: `Fatal Python error: _Py_HashRandomization_Init: failed
    to get random numbers`. Fixed the standard, documented way
    (equivalent to `PYTHONHASHSEED=0`): `pymain_baremetal.c` sets
    `config.use_hash_seed=1; config.hash_seed=0;`. Real fix later:
    give `bootstrap_hash.c` an RDRAND-backed source the same way
    `port/mbedtls_port/entropy_hardware_poll.c`/
    `port/libsodium_port/randombytes_baremetal.c` already do.
  - **`encodings` isn't in the frozen bootstrap set.** CPython's core
    init (`init_fs_encoding()`) unconditionally imports the `encodings`
    package to resolve the filesystem codec, before any app code runs
    -- unlike hash randomization, there's no `PyConfig` knob to skip
    it. `Modules/Setup.bootstrap.in`'s frozen set covers importlib/os/
    site/etc, not `Lib/encodings/`. Fixed without touching the
    filesystem (staying in Phase 1's scope, not reaching for Phase 3
    early): froze just `encodings/__init__.py` + `encodings/aliases.py`
    + `encodings/utf_8.py` via the host build's own
    `Programs/_freeze_module.py`, and registered them through
    `PyImport_FrozenModules` -- a real CPython embedding hook
    (`Include/cpython/import.h`: "Embedding apps may change this
    pointer to point to their favorite collection of frozen modules"),
    checked in preference to the built-in frozen tables by
    `Python/import.c`'s own `look_up_frozen()`. See
    `port/python_port/frozen_encodings_baremetal.c`.
- **Exit criterion met**: `python.app`, combined into a unikernel via
  `BareMetal-Firecracker/build.sh` and booted under Firecracker,
  printed `2` (from `print(1 + 1)`) and exited cleanly. See "Open
  questions" for the one environmental change this needed
  (256 MiB of VM RAM instead of the usual 4 MiB).

**Phase 2 -- sockets. DONE, boot-verified.** The original plan above
guessed a `net_shim.c`-backed C module would be needed, on the
assumption it'd be shaped like `sqlite_vfs.c`'s relationship to
`ext4_shim.c`/`posix_shim.c`. That guess was wrong, in the good
direction: `sqlite_vfs.c` exists because `SQLITE_OS_OTHER=1` makes
SQLite bypass libc's OS layer entirely and demand a hand-written
replacement. `Modules/socketmodule.c` doesn't do that -- it just calls
ordinary `socket()`/`bind()`/`connect()`/`send()`/`recv()`, exactly
like `net_test.c` or any other app here, and `posix_shim.c` already
dispatches every one of those to `net_shim.c` regardless of caller.
Compiled with **zero new C shim code**, only config work:

- `Modules/socketmodule.c` added to `xbuild-phase1.sh` as
  `PHASE2_MODOBJS`, registered in `config_baremetal.c`'s
  `_PyImport_Inittab` as `PyInit__socket` (not in the host build's own
  `config.c` at all -- there it was built as a shared `.so`, since this
  port has no dynamic loading it needs the same static-linking
  treatment as every bootstrap module).
- `HAVE_GETADDRINFO` was already undefined (Phase 1's `pyconfig.h`) --
  `Modules/socketmodule.c` self-`#include`s its own bundled
  `getaddrinfo.c`/`getnameinfo.c` fallback in that case, built on
  `gethostbyname()`, so DNS resolution rides `dns_shim.c`'s real
  resolver for free, no new code, exactly as guessed.
- A new round of Linux-only `HAVE_*`/config macros needed cutting,
  found the same empirical way as Phase 1's -- `socketmodule.h`
  declares struct members and headers for every optional address
  family CPython knows about, almost all Linux-specific and almost all
  absent from musl's sysroot: `HAVE_LINUX_NETLINK_H`, `HAVE_ASM_TYPES_H`
  (AF_NETLINK), `HAVE_LINUX_QRTR_H` (AF_QIPCRTR), `HAVE_LINUX_TIPC_H`
  (AF_TIPC), `HAVE_LINUX_CAN_H`/`_RAW_H`/`_BCM_H`/`_J1939_H`/
  `_RAW_FD_FRAMES`/`_RAW_JOIN_FILTERS` (AF_CAN), `HAVE_LINUX_VM_SOCKETS_H`
  (AF_VSOCK), `HAVE_SOCKADDR_ALG` (AF_ALG) -- and `ENABLE_IPV6` (no
  IPv6 anywhere on this port, `LWIP_IPV6=0`; also silences
  `getaddrinfo.c`'s IPv6 branch, which calls the deprecated
  `getipnodebyname()`/`getipnodebyaddr()` musl doesn't provide). Left
  `HAVE_SYS_UN_H`/`HAVE_NETPACKET_PACKET_H`/`HAVE_NET_IF_H` alone --
  musl's sysroot has real (non-uapi) headers for those, so they
  compile; `net_shim.c` still won't accept `AF_UNIX`/`AF_PACKET` at the
  `socket()`-call level, same "link, fail at the call site" pattern as
  the rest of this port.
- **Boot test**: `import _socket; _socket.socket(AF_INET, SOCK_STREAM);
  s.bind(('0.0.0.0', 0)); s.close()` -- all real, all through
  `posix_shim.c` -> `net_shim.c`, no code path bypassed. Output:
  `_socket constants: 2 1`, `socket() fileno: 100`, `bind() ok`,
  `close() ok`, `2` (see `pymain_baremetal.c`). Deliberately doesn't
  attempt a real `connect()`/DNS lookup -- this build/test host's
  `tap0` is configured but down (no carrier, see this repo's own
  `2-run.sh` warning), a host networking setup question, not a Phase 2
  code question. `s.getsockname()` was tried first and dropped: not in
  `posix_shim.c`'s `SYS_` dispatch table at all (`-ENOSYS`), a real,
  separate gap (`getpeername()` too), not exercised by this test.
- **`import socket` (the ergonomic pure-Python wrapper,
  `Lib/socket.py`) doesn't work yet** -- only the low-level `_socket`
  C extension is frozen/built in. `socket.py` itself would need
  freezing (Phase 1's `encodings` precedent) or a real file on the EXT2
  image (Phase 3) before `socket.socket(...)`, `socket.create_connection()`,
  etc. are usable the normal way.
- Every limit `OPENISSUES.md`'s Networking section already documents
  (16-socket fixed table, 30s blocking-call timeout, no
  `SO_REUSEADDR`/nonblocking mode, TCP/UDP/IPv4 only, single NIC)
  applies unchanged -- a Python program hits these as
  `socket.timeout`/silently-ignored `setsockopt()`, not new limits
  Phase 2 introduced.

**Phase 3 -- real filesystem-backed imports and stdlib growth.** Ship
`.py`/`.pyc` files on the EXT2 image instead of (or alongside) frozen
bytecode, using `ext4_shim.c`'s now-real `stat`/`readdir`/`open`.
Pull in additional `Modules/Setup.stdlib.in` modules one at a time
(`_datetime`, `_json`, `_struct`, `array`, ...), each needing its own
pass through the same "which `HAVE_*` macros does this file actually
touch" audit `pyconfig_baremetal.h`'s header describes for the
bootstrap set. `_socket` from Phase 2 unlocks `urllib`/`http.client`
(pure-Python, no extra C module) for free at that point.

## Open questions / risks

- ~~CPython's own build tooling~~ -- resolved by avoiding it entirely,
  the same way anticipated: `xbuild-phase1.sh` never runs CPython's
  `./configure`/`Makefile`, only a native host build (for its
  architecture-independent generated sources, see Phase 1 above) plus
  hand-driven `gcc`/`ld` invocations against the real target flags,
  exactly like `build-app.sh` already does for curl. This turned out
  to *not* be "unscoped effort... larger than everything else in this
  document combined" as originally guessed here -- it was mostly
  mechanical once the object lists were copied from the host Makefile.
- **VM RAM: needed 256 MiB, not the usual 4 MiB, to boot at all.**
  `baremetal.sh`'s Firecracker config hardcodes `mem_size_mib: 4`
  (fine for `hello.c`/`threads.app`); `python.app`'s flat binary alone
  is ~8.3 MB (statically-linked interpreter + frozen bytecode +
  lwIP/mbedTLS/curl/SQLite/lwext4/libsodium all still linked in, see
  Phase 1's `ext4_shim.o`/`net_shim.o` note above), which doesn't fit
  in 4 MiB before the heap even starts. Not yet measured: the actual
  *working-set* minimum once running (vs. just "large enough to load
  the binary and get through startup") -- 256 MiB was picked
  generously to get a clean first boot, not tuned down. Confirms
  `OPENISSUES.md`'s "no growth beyond the initial `b_system(FREE_MEMORY)`
  ceiling" caveat is a real, not just theoretical, concern for Python
  specifically -- pymalloc arenas, the bytecode compiler, and any real
  workload's object graph are all meaningfully larger than
  `hello.c`/`sqltest.c`'s footprint. `1-build.sh`/cloud deployment
  sizing (`3-upload.sh`) hasn't been looked at yet either.
- **`thread_shim.c`'s `THREAD_SHIM_MAX_THREADS = 32` fixed table** vs.
  whatever `pthread_attr_setstacksize`/CPython's own default thread
  stack size actually request -- still unaddressed; Phase 1's
  `pymain_baremetal.c` never calls `_thread.start_new_thread()`, so
  `Modules/_threadmodule.c` links but is still functionally untested.
- ~~musl's `sem_timedwait`/`sem_clockwait` presence~~ -- resolved:
  `build/musl-1.2.6/src/thread/sem_timedwait.c` exists (futex-based, no
  new shim needed); `sem_clockwait.c` does not (a later musl addition),
  matching `pyconfig_baremetal.h` leaving `HAVE_SEM_CLOCKWAIT` undefined.
- **Only 3 of `Lib/encodings/`'s ~100 codec modules are frozen**
  (`encodings`, `encodings.aliases`, `encodings.utf_8` -- see Phase 1).
  Any code path that needs a different codec (`'latin-1'`, `'ascii'`
  as their own explicit `encodings.ascii` import rather than the C-level
  fast path, `'cp1252'`, ...) will `ModuleNotFoundError` until Phase 3
  puts a real `Lib/` tree on the EXT2 image or more get frozen by hand
  the same way.
- **`socket.getsockname()`/`getpeername()` aren't wired up in
  `posix_shim.c`** (found running Phase 2's boot test, dropped from it
  rather than fixed) -- not in the `SYS_` dispatch table at all, falls
  to the default `-ENOSYS` case. `OPENISSUES.md`'s Networking section
  doesn't call these out by name; worth adding there independent of
  Python.
- **Real network connectivity (DNS + `connect()`) still unverified end
  to end from Python.** Phase 2's boot test proved `socket()`/`bind()`/
  `close()` reach `net_shim.c` correctly, but deliberately didn't
  attempt a live connection -- this test host's `tap0` is configured
  but down (no carrier). `curltest.c`/`net_test.c` prove the
  underlying `gethostbyname()`->`connect()` path works on this port in
  C; re-running Phase 2's test with a real `tap0`/bridge up (see
  `BareMetal-Firecracker/scripts/mkbr0.sh`) would confirm it from
  Python too, not yet done.
- **`import socket` (the ergonomic `Lib/socket.py` wrapper) isn't
  available**, only the low-level `_socket` C module -- same shape as
  the `encodings` gap above, same fix (freeze it by hand, or wait for
  Phase 3's real `Lib/` tree). `socket.py` itself is more involved than
  `encodings/__init__.py` was (imports `os`, `enum`, `errno`, `select`
  -- not all necessarily frozen/available yet), not checked.

## Bottom line

No fundamental primitive is missing -- threading was the last one, and
Phases 1 and 2 prove it end to end: a real CPython 3.12.8 now boots on
BareMetal-Firecracker, runs Python code, and drives real sockets
through `posix_shim.c`/`net_shim.c` with zero new C shim code (only
config-header cuts, the same kind Phase 1 needed). What's left (Phase 3
and the smaller "freeze more stdlib" gaps in Open questions above) is
more of the same kind of bounded, mostly-mechanical work, not a new
category of problem -- a real `Lib/` tree on the EXT2 image once
`ext4_shim.c`-backed imports are wired up.
