# Python Port

A CPython 3.12.8 port for this repo, following the same
vendor-unmodified-source-plus-hand-written-config pattern already used
for curl/SQLite/Mbed TLS/lwIP/lwext4/libsodium: `port/python_port/`
holds this port's own glue (`python.c`, `config_baremetal.c`,
`frozen_encodings_baremetal.c`, `pyconfig.h`), `scripts/get-python.sh`
fetches CPython 3.12.8 unmodified, and both `setup.sh` and
`build-app.sh` build it the same way they build every other port.
Unlike a library other apps merely link against, though, CPython's
whole point *is* the app -- so `./setup.sh` alone leaves `python.app`
already built and ready to go, no separate `build-app.sh` invocation
needed. (Re-run the same build by hand any time
`port/python_port/`'s own sources change:

```
./build-app.sh port/python_port/python.c port/python_port/config_baremetal.c port/python_port/frozen_encodings_baremetal.c
```

-- this is exactly what `setup.sh` itself runs at the end.)

`python.app` is ready to combine into a unikernel via
`BareMetal-Firecracker/build.sh` the same way `1-build.sh` does for any
other app. It boots, runs real Python code, drives real sockets through
`posix_shim.c`/`net_shim.c`, and imports real, unmodified `.py` files
off the EXT2 disk image through `ext4_shim.c` -- see "Running your own
program" below for how to point it at your own script.

This became possible once `port/thread_shim.c` gave this port real
`pthread_*` (see `OPENISSUES.md`'s "Process model" -> "Threads"
section) -- a GIL-based interpreter genuinely cannot start without
threading. What follows is a record of how this port was built and
*why* each piece is the way it is, organized into three parts (labeled
Phase 1/2/3 below) in the order they were tackled: the interpreter
itself, sockets, then real filesystem-backed imports.

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

## How it was built

**Phase 1 -- the interpreter itself.** What it took to get a
boot-verified interpreter running:

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
- **`setup.sh`/`build-app.sh` build CPython the same way they build
  every other port** -- `setup.sh` compiles PARSER_OBJS+PYTHON_OBJS+
  OBJECT_OBJS+the frozen/getpath glue+`Modules/Setup.bootstrap.in`'s
  module set once (object lists copied straight from a native host
  build's own Makefile) into `build/python_*.o`, exactly like it
  already does for curl/mbedTLS/lwIP/SQLite/libsodium/lwext4's own
  objects; `build-app.sh` links those into every app's final binary the
  same uniform way it already links curl/SQLite/etc into every app,
  whether that app is `port/python_port/python.c` or `hello.c`
  (`--gc-sections` drops what's unreachable either way). Neither runs
  CPython's own `./configure`/`Makefile` -- bypassed entirely, the same
  choice `build-app.sh` already makes for curl. `ext4_shim.o`/
  `net_glue.o`/`net_shim.o`/`dns_shim.o` end up linked in too (found
  necessary at link time, not planned for -- see below), same as they
  already are for every other app.
- **`port/python_port/python.c`** in place of CPython's own
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
    `setup.sh`/`build-app.sh` adds `gcc -print-file-name=include` after
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
    `python.c` never calls `calculate_path()` anyway.
  - `Modules/gcmodule.c` isn't in `Modules/Setup.bootstrap.in` at all
    (it's one of the modules `Modules/config.c.in`'s own "ADDMODULE
    MARKER" mechanism always force-builds, alongside `marshal`/`_imp`/
    `_ast`/`_tokenize`, which *were* already covered by files already
    on the PARSER/PYTHON_OBJS list) -- missed on the first pass, only
    caught at link time (`undefined reference to PyInit_gc`).
  - `posix_shim.c`'s syscall dispatcher references `ext4_shim_*`/
    `net_shim_*` unconditionally, not behind any `#ifdef` -- so even
    though `python.c` touches no filesystem or network,
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
    (equivalent to `PYTHONHASHSEED=0`): `python.c` sets
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

**Phase 2 -- sockets.** The initial guess was that a `net_shim.c`-backed C module would be needed, shaped like `sqlite_vfs.c`'s relationship to
`ext4_shim.c`/`posix_shim.c`. That guess was wrong, in the good
direction: `sqlite_vfs.c` exists because `SQLITE_OS_OTHER=1` makes
SQLite bypass libc's OS layer entirely and demand a hand-written
replacement. `Modules/socketmodule.c` doesn't do that -- it just calls
ordinary `socket()`/`bind()`/`connect()`/`send()`/`recv()`, exactly
like `net_test.c` or any other app here, and `posix_shim.c` already
dispatches every one of those to `net_shim.c` regardless of caller.
Compiled with **zero new C shim code**, only config work:

- `Modules/socketmodule.c` added to `setup.sh`/`build-app.sh` as
  setup.sh's `PYTHON_BUILTIN_SRCS`, registered in `config_baremetal.c`'s
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
  `close() ok`, `2` (see `python.c`). Deliberately doesn't
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

**Phase 3 -- real filesystem-backed imports.** The plan was to ship
`.py`/`.pyc` files on the EXT2 image, using `ext4_shim.c`'s now-real
`stat`/`readdir`/`open` -- that part was right. What it actually took:

- **No host root needed to write files onto the disk image.**
  `disk.sh`'s own approach (`sudo mount -o loop`) needs interactive
  sudo and can't run against an image a Firecracker VM might have
  open. `port/python_port/install-stdlib.sh` uses `debugfs -w`
  (e2fsprogs) instead -- writes ext2 structures directly against the
  image file, needing only read/write access to that file, no loop
  device, no root, no VM-stopped precondition beyond what `disk.sh`
  itself already requires.
- **What gets installed is a small, precisely-traced dependency
  closure, not "all of Lib/".** Rather than guessing which files
  `import json` needs by reading source (Phase 1/2's style for
  `HAVE_*` macros doesn't apply to Python-level dependencies), it was
  traced directly: `build/host-python-build/python -S -c "import
  json"`, diffing `sys.modules` before/after. Result: 19 files --
  `json`'s own 4-file package, plus `collections`, `re` (itself a
  5-file package in 3.12), `enum`, `functools`, `_collections_abc`,
  `copyreg`, `keyword`, `operator`, `reprlib`, `types` -- plus
  `encodings/ascii.py` for the other test (see below). `_json` (json's
  optional C accelerator) is genuinely optional -- not built, and
  `import json` doesn't need it, pure Python `json/scanner.py` covers
  it.
- **`python.c`'s `PyConfig.module_search_paths` gets one real
  entry now**, `/pylib` (`install-stdlib.sh`'s target
  directory), instead of Phase 1's empty list. `importlib._bootstrap_external`'s
  `PathFinder` walks it the normal way -- no new C code, same "it just
  works once the plumbing's real" pattern Phase 2's `_socket` showed.
- **Real finding, not anticipated by the original plan: a frozen
  *package*'s `__path__` doesn't let you add real filesystem submodules
  to it for free.** First attempt at testing `import encodings.ascii`
  (deliberately picked *because* `encodings` is one of Phase 1's frozen
  packages, to test layering) failed with `ModuleNotFoundError`, even
  though `/pylib/encodings/ascii.py` existed and `/pylib` was on
  `sys.path` correctly. Root cause: `importlib._bootstrap.FrozenImporter`
  builds a frozen package's `ModuleSpec` with `submodule_search_locations=[]`
  (empty, not `None`) -- `encodings.aliases`/`encodings.utf_8` still
  import fine despite this because `FrozenImporter` matches them
  directly by their own exact frozen name in `sys.meta_path`, before
  `PathFinder` (which needs a real `__path__`) is ever consulted; but
  `encodings.ascii`, not being frozen, falls through to `PathFinder`,
  which has nothing to search. **Fixed, not just documented**: since
  that `__path__` is a real, appendable list (just empty),
  `python.c` does `import encodings;
  encodings.__path__.append('/pylib/encodings')` right after
  `Py_InitializeFromConfig()` -- a 2-line, no-new-C-code fix. This is a
  general pattern, not an `encodings`-specific hack: *any* frozen
  package gets this same treatment if/when it needs real filesystem
  submodules later, and it's a no-op (harmless empty search location)
  if `/pylib` isn't present, so Phase 1's zero-filesystem-dependency
  boot path is unaffected.
- **Boot test**: `import encodings.ascii; encodings.ascii.getregentry().name`
  -> `ascii` (real file, not frozen -- confirms the `__path__` fix);
  `import json; json.dumps({'a': [1, 2, 3]})` -> `{"a": [1, 2, 3]}`;
  `json.loads(...)` round-trips it back. Both from real, unmodified
  CPython source files read off the EXT2 image at runtime, through
  `ext4_shim.c`, with zero new C shim code -- config/data only, the
  same shape Phase 2 turned out to have.
- **`Modules/Setup.stdlib.in`'s C-extension modules** (`_datetime`,
  `_json`, `_struct`, `array`, ...) are still not built in -- Phase 3
  only proved the *pure-Python* filesystem-import path. Each of those
  would still need its own `HAVE_*` audit and a setup.sh's `PYTHON_BUILTIN_SRCS`-style
  addition to `setup.sh`/`build-app.sh`, same as `_socket` was, not attempted
  here.

## Open questions / risks

- ~~CPython's own build tooling~~ -- resolved by avoiding it entirely,
  the same way anticipated: `setup.sh`/`build-app.sh` never runs CPython's
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
- ~~`Modules/_threadmodule.c` functionally untested~~ -- resolved
  (partially): `port/python_port/main_test.py` (see "Running your own
  program" below) added a bounded `_thread` test --
  `_thread.start_new_thread()` plus a `Lock` used as a completion
  signal, `acquire()` called with a 5s timeout rather than blocking
  forever in case `thread_shim.c`'s cooperative scheduler didn't
  actually cooperate with CPython's `Python/thread_pthread.h`
  assumptions the way `pyconfig_baremetal.h`'s Threading section
  predicted. It passed, boot-verified: a second thread ran concurrently
  with the main one, `Lock.release()`/timed `acquire()` synchronized
  them correctly, real result data crossed threads intact. Only a
  smoke test, not a stress test -- `thread_shim.c`'s
  `THREAD_SHIM_MAX_THREADS = 32` fixed table vs. CPython's default
  thread stack size under real concurrent load (more than one or two
  threads, real contention) is still unexercised.
- ~~musl's `sem_timedwait`/`sem_clockwait` presence~~ -- resolved:
  `build/musl-1.2.6/src/thread/sem_timedwait.c` exists (futex-based, no
  new shim needed); `sem_clockwait.c` does not (a later musl addition),
  matching `pyconfig_baremetal.h` leaving `HAVE_SEM_CLOCKWAIT` undefined.
- ~~Only 3 of `Lib/encodings/`'s ~100 codec modules are frozen~~ --
  resolved by Phase 3: `encodings.__path__.append('/pylib/encodings')`
  in `python.c` means any codec whose `.py` file is copied
  onto `/pylib/encodings` (via `install-stdlib.sh`, currently
  just `ascii.py`) becomes importable normally. Only `ascii`/`utf_8`
  are actually present right now -- a different codec (`'latin-1'`,
  `'cp1252'`, ...) still needs its file added the same way, but the
  *mechanism* is no longer the blocker.
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
- **`import socket` (the ergonomic `Lib/socket.py` wrapper) still isn't
  available**, only the low-level `_socket` C module -- unlike the
  `encodings` gap, this one has a clear, unblocked fix now:
  `install-stdlib.sh`-style, trace `import socket`'s real
  dependency closure (`os`/`sys`/`io` already frozen; `selectors`/`enum`
  not yet) the same way `json`'s was traced, add those files to
  `/pylib`, no `__path__` patch needed (`socket.py` is a plain module,
  not a frozen package). Not done yet, just no longer an open question
  about *how*.
- **`install-stdlib.sh` isn't idempotent.** `debugfs mkdir`
  fails outright if `/pylib` already exists -- fine for this session's
  one-time install, but re-running it (e.g. after `setup.sh` recreates
  `disk.img` from scratch) needs the old `/pylib` gone first, and there's
  no `rm -rf`-equivalent single debugfs command for that (`rm`+`rmdir`
  per entry). Worth a real fix before this becomes a normal part of the
  build flow rather than a one-off experiment.
- **`/pylib` now lives permanently on this test host's `disk.img`**,
  not just for the duration of the boot test -- `disk.img` itself isn't
  committed to git (512 MB, gitignored like every other build
  artifact), so this is local-only state, not something the `python`
  branch's commits capture. Anyone re-running Phase 3 elsewhere needs
  `install-stdlib.sh` run once against their own `disk.img`
  first.

## Running your own program

`python.c` runs `/pylib/main.py` off the EXT2 disk image as the actual
program -- nothing Python-level is baked into the binary.
`port/python_port/install-main.sh /path/to/disk.img
[/path/to/your_script.py]` puts a file there via `debugfs -w` (same
no-root approach as `install-stdlib.sh`, and unlike that script, this
one *is* idempotent -- safe to re-run on every change). With no script
argument it installs this directory's own `main_test.py`, a smoke test
covering core language, `os`/`sys`/`time`, `_socket`, and the real
`/pylib`-backed `json`/`re`/`collections`/`encodings.ascii` imports --
10/10 checks pass, boot-verified. Anything your own script imports
beyond what's already on `/pylib`/frozen/built-in needs its own files
added the same way `install-stdlib.sh`'s own comment describes (trace a
real `import` on `build/host-python-build/python`, add exactly what's
new).

## Bottom line

No fundamental primitive is missing -- threading was the last one, and
Phases 1, 2, and 3 prove it end to end: a real CPython 3.12.8 now boots
on BareMetal-Firecracker, runs Python code, drives real sockets through
`posix_shim.c`/`net_shim.c`, and imports real, unmodified `.py` files
off the EXT2 disk image through `ext4_shim.c` -- all three with zero
new C shim code, only config-header cuts and ~20 lines of embedding-API
setup in `python.c`. What's left is filling in *more* of the
same two mechanisms this now has (freeze more bootstrap-critical
modules, install more `Lib/` files onto `/pylib`) plus the smaller
concrete gaps in "Open questions" above (`_socket`'s C-extension
siblings like `_datetime`/`_json`/`_struct`, `getsockname()`/
`getpeername()`, `Lib/socket.py`'s own dependency closure) -- not a new
category of problem.
