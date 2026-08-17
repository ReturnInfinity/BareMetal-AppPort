/* pyconfig_baremetal.h -- see PYTHON.md.
 *
 * Hand-written build config for CPython 3.14.7 on this port (see
 * scripts/get-python.sh), the same role curl_config.h/
 * sqlite_baremetal_config.h play for curl/SQLite: CPython's own build
 * generates pyconfig.h by running ./configure, which can't run against
 * this freestanding, no-syscall-trap target (no shell/fork to execute
 * its hundreds of AC_CHECK_FUNC/AC_CHECK_HEADER probe binaries against).
 * This is a hand-written substitute -- unlike curl/SQLite, CPython's
 * own Include/Python.h includes "pyconfig.h" directly and
 * unconditionally, no -DHAVE_CONFIG_H indirection needed.
 *
 * IMPORTANT -- this file itself is not what gets `#include`'d during a
 * build (that's port/python_port/pyconfig.h -- see its own header for
 * how the two relate). This file only takes a position on the macros
 * that are a real port-design decision; the other ~150 mechanical ones
 * (does <sys/foo.h> exist, is struct stat's st_blksize field present,
 * is off_t 64-bit) are architecture-level, not something this port
 * needs its own opinion on, and are answered by pyconfig.h's own real
 * ./configure-derived base instead -- see that file's header.
 *
 * What *is* below are the macros whose answer is a real port-design
 * decision, not a mechanical header probe -- the same split
 * curl_config.h's own header comment describes ("hand-corrected every
 * HAVE_/USE_ macro... configure detects via the host's libc but this
 * port's musl+posix_shim.c stack doesn't actually back"). Each one
 * ties back to a specific OPENISSUES.md section or port/ file. When in
 * doubt, left *undefined*: CPython, like curl, degrades a missing
 * HAVE_ to a compile-time-omitted code path rather than a runtime
 * crash -- posixmodule.c in particular is already written to run on
 * platforms (Windows, WASI) missing most of what's cut here.
 */

#ifndef PYTHON_PORT_PYCONFIG_BAREMETAL_H
#define PYTHON_PORT_PYCONFIG_BAREMETAL_H

/* ---------------------------------------------------------------------
 * Process model -- see OPENISSUES.md's "Process model" section.
 * No fork/exec/wait/spawn family at all: posix_shim.c's dispatcher has
 * no SYS_clone (for a real fork, as opposed to thread_shim.c's own
 * unrelated SYS_clone use for threads)/SYS_execve/SYS_wait4 case, all
 * fall through to -ENOSYS. Every one of these left undefined disables
 * the corresponding os.* function at the posixmodule.c level (it's
 * simply not defined on the os module) rather than exposing one that
 * always fails at call time -- matches how this port already prefers
 * to fail (see curl_config.h's own stated preference).
 * --------------------------------------------------------------------- */
#undef HAVE_FORK
#undef HAVE_FORK1
#undef HAVE_FORKPTY
#undef HAVE_EXECV
#undef HAVE_FEXECVE
#undef HAVE_WEXECV
#undef HAVE_POSIX_SPAWN
#undef HAVE_POSIX_SPAWNP
#undef HAVE_SPAWNV
#undef HAVE_WAIT
#undef HAVE_WAIT3
#undef HAVE_WAIT4
#undef HAVE_WAITID
#undef HAVE_WAITPID
#undef HAVE_SYSTEM        /* system() needs fork+exec under the hood too */

/* No pid/uid/gid model at all (OPENISSUES.md: "getpid/getppid/kill/
 * uname/sysinfo/times are unimplemented"). */
#undef HAVE_GETPID
#undef HAVE_GETPPID
#undef HAVE_KILL
#undef HAVE_KILLPG
#undef HAVE_GETUID
#undef HAVE_GETEUID
#undef HAVE_GETGID
#undef HAVE_GETEGID
#undef HAVE_SETUID
#undef HAVE_SETEUID
#undef HAVE_SETGID
#undef HAVE_SETEGID
#undef HAVE_SETREUID
#undef HAVE_SETREGID
#undef HAVE_SETRESUID
#undef HAVE_SETRESGID
#undef HAVE_GETRESUID
#undef HAVE_GETRESGID
#undef HAVE_GETGROUPS
#undef HAVE_GETGROUPLIST
#undef HAVE_SETGROUPS
#undef HAVE_INITGROUPS
#undef HAVE_GETPGID
#undef HAVE_GETPGRP
#undef HAVE_SETPGID
#undef HAVE_SETPGRP
#undef HAVE_GETSID
#undef HAVE_SETSID
#undef HAVE_GETPRIORITY
#undef HAVE_SETPRIORITY
#undef HAVE_UNAME
#undef HAVE_TIMES         /* per-process CPU-time accounting; no process table to account against */
#undef HAVE_GETLOGIN
#undef HAVE_GETLOADAVG
#undef HAVE_GETRUSAGE
#undef HAVE_NICE
#undef HAVE_SCHED_SETAFFINITY  /* no process/CPU-set concept -- single core, one flat address space */
#undef HAVE_SCHED_SETPARAM
#undef HAVE_SCHED_SETSCHEDULER /* matches thread_shim.c not wiring up sched_setscheduler either */

/* No real signal delivery (OPENISSUES.md: rt_sigaction/rt_sigprocmask/
 * sigaltstack are accepted-and-ignored). Leave the libc-level
 * HAVE_SIGACTION/HAVE_SIGNAL_H etc. defined -- musl really does provide
 * those symbols and signalmodule.c will link and let a program
 * register handlers exactly like posix_shim.c already allows -- but
 * cut everything that assumes a handler can ever actually fire, or that
 * depends on a process to signal in the first place. */
#undef HAVE_SIGWAIT
#undef HAVE_SIGWAITINFO
#undef HAVE_SIGTIMEDWAIT
#undef HAVE_SIGPENDING
#undef HAVE_PAUSE
#undef HAVE_ALARM
#undef HAVE_SETITIMER
#undef HAVE_GETITIMER
#undef HAVE_PTHREAD_KILL      /* signal-to-a-specific-thread -- same "no delivery" gap */
#undef HAVE_PTHREAD_SIGMASK

/* No env/argv (OPENISSUES.md: "crt0.c fabricates ... argc=0 and an
 * empty envp"). getenv()/setenv() themselves still link (musl provides
 * them against its own empty in-process environ), just never see
 * anything meaningful -- same "let it link and silently do nothing"
 * choice as chdir()/getcwd() below, not something to gate at compile
 * time. Nothing to undef here; noted for PYTHON.md's Phase 1
 * writeup instead (sys.path/site.py bootstrapping can't rely on
 * PYTHONHOME/PYTHONPATH env vars the way a normal build's Modules/
 * getpath.c does). */

/* ---------------------------------------------------------------------
 * Dynamic loading of extension modules -- originally cut entirely (c.ld's
 * single flat binary at a fixed load address, no real ld.so anywhere in
 * this port) but re-enabled once port/dlfcn_shim.c existed: a hand-rolled
 * ELF64 loader that reads a real ET_DYN .so off the ext4 disk, processes
 * its .rela.dyn/.rela.plt by hand (there's still no ld.so to do it), and
 * resolves external symbols against a small curated export table
 * (dl_exports[] in that file) rather than the module's full host symbol
 * table -- see that file's own header for why. Python/dynload_shlib.c
 * (CPython's real dlopen()/dlsym()-based loader) just calls straight into
 * that -- swapped in for Python/dynload_stub.c ("necessary stubs for when
 * dynamic loading is not present") in the port's own build script (there
 * is no configure to pick it automatically).
 *
 * Consequence of the curated-export-table design: an extension module can
 * only call back into whatever dlfcn_shim.c's dl_exports[] lists -- add
 * entries there as modules need more of the CPython C API/libc/libm.
 * Function exports are just the function's own address; a *data* symbol
 * (extern PyObject *PyExc_ValueError-style) instead needs
 * (void*)&PyExc_ValueError -- the address of the variable, not its
 * current value -- see dlfcn_shim.c's comment for why.
 *
 * Modules/Setup.bootstrap.in's mandatory-static set is unaffected either
 * way ("Built-in modules required to get a functioning interpreter;
 * cannot be built as shared!") -- those still go through
 * Modules/config.c.in's static frozen/builtin table
 * (config_baremetal.c here), which coexists with dynamic loading by
 * module name, not by exclusivity. ctypes remains a casualty regardless
 * of dlopen() now existing -- it needs far more of libffi/the C API than
 * one export table entry, not attempted.
 * --------------------------------------------------------------------- */
#define HAVE_DYNAMIC_LOADING 1
#define HAVE_DLOPEN 1
#define HAVE_DLFCN_H 1
#define HAVE_DECL_RTLD_LAZY 1
#define HAVE_DECL_RTLD_NOW 1
#define HAVE_DECL_RTLD_GLOBAL 1
#define HAVE_DECL_RTLD_LOCAL 1
#define HAVE_DECL_RTLD_NODELETE 1
#define HAVE_DECL_RTLD_NOLOAD 1
/* musl's dlfcn.h doesn't declare either of these two -- left cut, matching reality: */
#undef HAVE_DECL_RTLD_DEEPBIND
#undef HAVE_DECL_RTLD_MEMBER
#define Py_NO_ENABLE_SHARED 1   /* build the interpreter itself as one static binary, no libpython*.so */

/* FIXED (was a real, deterministic bug -- not a hypervisor/hardware
 * issue, despite looking exactly like one at first): importing any
 * dynamically-loaded extension module used to corrupt every CPython
 * C-API call the loaded module's own code made afterward -- e.g.
 * PyModule_Create2 (called from ../pyexttest.c's PyInit_pyexttest)
 * would segfault deep inside Objects/obmalloc.c's pymalloc_alloc(),
 * with its `state` argument coming back as exactly
 * offsetof(PyInterpreterState, obmalloc). Traced to get_state() ->
 * _PyInterpreterState_GET() -> _PyThreadState_GET() reading
 * _Py_tss_tstate (a `_Thread_local`/__thread variable, `mov %fs:-8,
 * %reg`) as a bogus pointer whose ->interp field happened to be NULL.
 *
 * Root cause, confirmed via readelf -S on the linked binary: port/c.ld
 * had no explicit .tdata/.tbss output-section rule (only .bss had one
 * -- *(.bss .bss.*) -- to glob -fdata-sections's separate per-symbol
 * sections back together). Without it, ld placed CPython's two
 * `_Thread_local` variables -- _Py_tss_tstate (Python/pystate.c, "the
 * current thread state") and pkgcontext (Python/import.c,
 * "_Py_PackageContext", the module name mid-import) -- at the exact
 * same address (both .tbss.pkgcontext and .tbss._Py_tss_tstate showed
 * identical VMAs). _PyImport_SwapPackageContext() writes pkgcontext
 * (a plain module-name string pointer) right before calling the
 * loaded module's PyInit_* function -- since that write landed on the
 * SAME memory as _Py_tss_tstate, every CPython C-API call the module
 * made afterward read a string pointer back where it expected a
 * PyThreadState*.
 *
 * This is why it looked so much like a %fs/hypervisor bug during
 * investigation: FS_BASE itself (read directly via rdmsr on
 * IA32_FS_BASE, bypassing the compiler's own TLS-access instruction
 * entirely) was proven byte-identical in both the working and broken
 * contexts, deterministically, across repeated boots -- ruling out any
 * register/virtualization explanation and pointing at the *memory*
 * two fixed, correct addresses resolved to actually being one and the
 * same. No PT_TLS program header or runtime TLS-copy machinery was
 * ever involved -- musl's own PT_TLS-walking init (Python-3.12.8's
 * vendored env/__init_tls.c, via aux[AT_PHDR]) is dead code on this
 * port regardless (crt0.c's fabricated auxv carries only
 * AT_PAGESZ/AT_RANDOM), since x86-64 Local-Exec TLS access computes
 * its offset entirely at *link* time -- the fix is purely giving ld
 * one correctly-sized, non-overlapping .tdata/.tbss pair to place,
 * the same way .bss's own rule already does for everything else.
 * Verified fixed: readelf -S now shows one 16-byte .tbss holding both
 * variables contiguously (was two separate, colliding 8-byte
 * sections), _Py_tss_tstate reads correctly from inside a dlopen()'d
 * module, and ../pyexttest.c's real PyModule_Create2/PyArg_ParseTuple/
 * PyLong_FromLong path runs end-to-end with no crash.
 */

/* ---------------------------------------------------------------------
 * Threading -- the one section that's *good* news. thread_shim.c
 * (OPENISSUES.md's "Process model" -> "Threads" subsection) gives real
 * pthread_create/join/detach/self/equal, mutexes (normal/trylock/
 * recursive), condvars (signal/broadcast/timedwait), rwlocks,
 * pthread_once, TSD (pthread_key_*/__thread), spinlocks, barriers, and
 * sched_yield, all backed by musl's own pthread_*.c built on top of
 * SYS_futex (FUTEX_WAIT/WAKE/REQUEUE only -- see thread_shim.c's file
 * header for why real preemptive round-robin switching makes even the
 * futex-free primitives like pthread_spin_lock() safe). That is
 * Python/thread_pthread.h's entire dependency surface for the ordinary
 * (non-semaphore) path -- CPython's GIL and _thread module need nothing
 * this port doesn't already have. musl's sem_open/sem_wait/sem_post
 * (POSIX unnamed semaphores, thread_pthread.h's USE_SEMAPHORES path)
 * are implemented directly against the same futex syscall, not against
 * any *additional* pthread_* entry point thread_shim.c would need to
 * grow -- so this should work as-is, unverified until a real build
 * reaches Modules/_threadmodule.c and Programs/python.o link together.
 * --------------------------------------------------------------------- */
#define HAVE_PTHREAD_H 1
/* _POSIX_THREADS: not defined here -- musl's own <unistd.h> already
 * defines it (to _POSIX_VERSION); redefining it produces a harmless
 * but noisy macro-redefined warning on every file (found the hard way
 * running this port's build (setup.sh/build-app.sh) -- see that script). */
/* HAVE_PTHREAD_SIGMASK is left undefined in the Process-model section
 * above -- pthread_sigmask() itself is real in musl, but
 * thread_pthread.h only calls it to block signals for delivery this
 * port doesn't have, so leaving it undefined just skips a harmless
 * mask call, not a hard requirement. */
#undef HAVE_BROKEN_POSIX_SEMAPHORES
#define HAVE_SEM_TIMEDWAIT 1   /* confirmed: build/musl-1.2.6/src/thread/sem_timedwait.c exists, futex-based like the rest of musl's semaphore code */
#undef HAVE_SEM_CLOCKWAIT      /* confirmed absent -- musl 1.2.6 has no sem_clockwait.c (a later musl addition) */
#undef PTHREAD_SYSTEM_SCHED_SUPPORTED  /* matches thread_shim.c not wiring up sched_setscheduler (OPENISSUES.md) */
/* THREAD_STACK_SIZE deliberately left at thread_pthread.h's own
 * default rather than 0 (system default) -- thread_shim.c hand-builds
 * each thread's second call stack itself (SYS_clone, see its file
 * header) from a fixed-size allocation; what pthread_attr_setstacksize
 * actually does with that value on this port needs checking against
 * thread_shim.c's THREAD_SHIM_MAX_THREADS=32 fixed table before Phase 1. */

/* ---------------------------------------------------------------------
 * Filesystem -- ext4_shim.c (OPENISSUES.md's EXT2 section) now backs a
 * real subset of POSIX file I/O, better than most of what curl/SQLite
 * needed. Left available; still missing the permission/ownership model
 * entirely (no uid/gid, see Process model above), so everything
 * chmod/chown/xattr-shaped stays cut.
 * --------------------------------------------------------------------- */
#define HAVE_STAT 1
#define HAVE_LSTAT 1
#define HAVE_SYMLINK 1
#define HAVE_READLINK 1
#define HAVE_REALPATH 1     /* musl provides this in terms of open/lstat/readlink, all real now */
#undef HAVE_CHMOD           /* OPENISSUES.md: "open()'s mode argument is ignored entirely" */
#undef HAVE_FCHMOD
#undef HAVE_FCHMODAT
#undef HAVE_LCHMOD
#undef HAVE_CHOWN
#undef HAVE_FCHOWN
#undef HAVE_FCHOWNAT
#undef HAVE_LCHOWN
#undef HAVE_CHFLAGS
#undef HAVE_LCHFLAGS
#undef HAVE_CHROOT          /* no concept of a filesystem root beyond the one EXT2 mount */
#undef HAVE_SYS_XATTR_H
#undef HAVE_UMASK           /* no permission model for a umask to apply to */
#undef HAVE_LINK            /* lwext4/ext4_shim.c expose no hardlink call, only symlink */
#undef HAVE_LINKAT
#undef HAVE_MKFIFO           /* no special-file types beyond regular files/dirs/symlinks */
#undef HAVE_MKFIFOAT
#undef HAVE_MKNOD
#undef HAVE_MKNODAT
#undef HAVE_PIPE            /* OPENISSUES.md: "no in-process fd duplication, pipes" */
#undef HAVE_PIPE2
#undef HAVE_DUP             /* not in posix_shim.c's SYS_ dispatch table */
#undef HAVE_DUP3
#undef HAVE_FDWALK
#undef HAVE_TRUNCATE         /* not wired in ext4_shim.c -- ftruncate() (fd-based) is; audit before Phase 1 */
#define HAVE_FTRUNCATE 1
#undef HAVE_FDATASYNC        /* ext4_shim.c has no per-fd sync -- lwext4 flushes on its own cadence, see OPENISSUES.md's "Superblock free-space counters" note */
#undef HAVE_FSYNC
#undef HAVE_SYNC
#undef HAVE_STATVFS           /* no equivalent in ext4_shim.c yet -- would need a small lwext4 wrapper */
#undef HAVE_FSTATVFS
#undef HAVE_POSIX_FADVISE
#undef HAVE_POSIX_FALLOCATE
#undef HAVE_SENDFILE
#undef HAVE_COPY_FILE_RANGE
#undef HAVE_SPLICE
#undef HAVE_LOCKF             /* fcntl() advisory locking is a stub (see sqlite_vfs.c's ext2Lock comment for the same reasoning) */

/* ---------------------------------------------------------------------
 * Heap/mmap -- posix_shim.c's mmap() is a bump allocator, anonymous
 * mappings only (OPENISSUES.md's "Heap" section: "munmap()'d memory
 * can't be handed back to the OS", "No growth beyond the initial
 * b_system(FREE_MEMORY) ceiling"). Objects/obmalloc.c's arena allocator
 * calls exactly mmap(NULL, size, PROT_READ|PROT_WRITE,
 * MAP_ANONYMOUS|MAP_PRIVATE, -1, 0) -- verified against this port's
 * sys_mmap() (posix_shim.c), which requires MAP_ANONYMOUS and rejects
 * file-backed mappings -- so this is a real match, not just a
 * plausible one. The practical effect worth flagging in PYTHON.md
 * rather than here: total interpreter heap is capped at whatever
 * BareMetal reports as free RAM at boot, arenas obmalloc frees are
 * reused by later mmap() calls but never shrink the process's
 * footprint, same caveat SQLite/curl/mbedTLS already live with. */
#define HAVE_MMAP 1
#define HAVE_SYS_MMAN_H 1
#define WITH_PYMALLOC 1
#undef WITH_PYMALLOC_RADIX_TREE  /* radix-tree arena tracking assumes a much larger, sparser address space than this port's fixed arena; leave the simpler linked-list obmalloc arena map on */
#undef WITH_MEMORY_LIMITS

/* ---------------------------------------------------------------------
 * Clocks -- posix_shim.c backs CLOCK_MONOTONIC(_RAW/_COARSE) via
 * b_system(TIMECOUNTER) and CLOCK_REALTIME(_COARSE) via
 * b_system(WALLCLOCK) (OPENISSUES.md's "Missing common syscalls"
 * section) -- real, not stubs. nanosleep/clock_nanosleep are real too
 * (chained in 10ms chunks so lwIP timers keep servicing during a
 * sleep). CLOCK_PROCESS_CPUTIME_ID and anything else return -EINVAL,
 * matching "no per-process accounting" above.
 * --------------------------------------------------------------------- */
#define HAVE_CLOCK_GETTIME 1
#define HAVE_CLOCK_GETRES 1
#undef HAVE_CLOCK_SETTIME     /* no RTC write path exposed */
#define HAVE_NANOSLEEP 1
#define HAVE_CLOCK_NANOSLEEP 1
/* HAVE_CLOCK: reinstated (not cut) -- Modules/timemodule.c's own
 * comment at its _PyTime_GetClockWithInfo() call site says "Currently,
 * Python 3 requires clock() to build: see issue #22624". That helper
 * is only *defined* under #if HAVE_CLOCK but *called* unconditionally
 * as time.process_time()'s last-resort fallback, so leaving this
 * undefined fails the build, not just a runtime call -- found the hard
 * way running this port's build (setup.sh/build-app.sh). musl does provide a real clock();
 * whether it returns anything meaningful here depends on times()/
 * CLOCK_PROCESS_CPUTIME_ID, neither of which posix_shim.c backs, so
 * time.process_time() likely fails or returns garbage at runtime --
 * same "let it link, fail at the call site" choice as HAVE_SIGACTION. */
#define HAVE_CLOCK 1
#define HAVE_MKTIME 1
#define HAVE_TIMEGM 1
#undef HAVE_TZSET              /* no tzdata / TZ env on this port -- matches sqlite_baremetal_config.h's SQLITE_OMIT_LOCALTIME reasoning exactly */
#undef HAVE_WORKING_TZSET

/* ---------------------------------------------------------------------
 * Networking -- dns_shim.c now provides real getaddrinfo()/
 * getnameinfo()/freeaddrinfo() of its own (backed by the same
 * dns_gethostbyname()-via-lwIP resolver gethostbyname() already used,
 * OPENISSUES.md's Networking section), so both are defined here
 * rather than left cut. This replaces an earlier choice to leave
 * HAVE_GETADDRINFO/HAVE_GETNAMEINFO undefined and let
 * Modules/socketmodule.c fall back to its own bundled
 * Modules/getaddrinfo.c/getnameinfo.c RFC-2553 emulation layer
 * (itself built in terms of gethostbyname()) -- that emulation is
 * old-style K&R C and produced -Wold-style-definition warnings on
 * every build for no benefit once dns_shim.c can do the same job
 * directly. Same "shadow gethostbyname/getaddrinfo instead of
 * trusting musl's real ones/resolv.conf" choice curl_config.h already
 * documents making for curl.
 *
 * dns_shim.c's getaddrinfo() only ever returns a single AF_INET
 * result (always SOCK_STREAM unless hints.ai_socktype says
 * otherwise -- there's no per-socktype enumeration), and its
 * getnameinfo() is numeric-only (no reverse/PTR DNS support -- see
 * that file's own header for why). Both are correct within that
 * scope, which is all this port's apps need.
 *
 * socket() itself and friends need no shim of their own (PYTHON.md's
 * Phase 2): Modules/socketmodule.c just calls ordinary socket()/
 * connect()/send()/recv(), which posix_shim.c already dispatches to
 * net_shim.c (SOCK_MAX=16 sockets, 30s timeout, no SO_REUSEADDR/
 * nonblocking -- same limits every other app here lives with) for
 * every caller, not just Python's.
 * --------------------------------------------------------------------- */
#define HAVE_GETADDRINFO 1
#define HAVE_GETNAMEINFO 1
#define HAVE_SYS_SOCKET_H 1

/* ---------------------------------------------------------------------
 * Found empirically, not by reading OPENISSUES.md ahead of time --
 * running this port's build (setup.sh/build-app.sh) against the merged pyconfig.h (built from
 * a real x86-64 glibc configure run, see pyconfig.h's own header)
 * surfaced these because glibc has them and musl's sysroot either
 * doesn't provide the header at all or doesn't back the syscall:
 * --------------------------------------------------------------------- */
/* Modules/posixmodule.c's own close_range() wrapper. No SYS_close_range
 * in posix_shim.c's dispatcher (OPENISSUES.md doesn't call this out by
 * name, but it's the same shape as every other cut syscall there). */
#undef HAVE_CLOSE_RANGE
/* Python/bootstrap_hash.c's os.getrandom()/urandom seeding path --
 * matches OPENISSUES.md's "getrandom -- nothing backs /dev/urandom-
 * equivalent randomness for application code" directly. Also cuts the
 * #include <linux/random.h> (for GRND_* flags) that only fires when
 * this is set without HAVE_GETRANDOM -- musl's sysroot vendors no
 * linux/ uapi headers at all, so that include would fail regardless. */
#undef HAVE_GETRANDOM
#undef HAVE_GETRANDOM_SYSCALL
/* Same include, gated separately (Modules/posixmodule.c's own
 * os.getrandom() has an independent HAVE_LINUX_RANDOM_H-gated
 * #include <linux/random.h> that HAVE_GETRANDOM/_SYSCALL don't cover). */
#undef HAVE_LINUX_RANDOM_H
/* Modules/faulthandler.c's alternate-signal-stack sizing (AT_MINSIGSTKSZ)
 * and Modules/posixmodule.c's P_PIDFD waitid() ID type both come from
 * <linux/auxvec.h>/<linux/wait.h> -- Linux uapi headers, not libc ones.
 * musl's sysroot here vendors no linux/ subtree at all (it only ships
 * musl's own libc headers, see build/musl-1.2.6/sysroot/.../include),
 * and this port has no ELF loader/auxv (crt0.c fabricates its own
 * startup) or waitid() (Process model section) for either to matter to
 * anyway. */
#undef HAVE_LINUX_AUXVEC_H
#undef HAVE_LINUX_WAIT_H
/* Same story again: os.memfd_create()'s <linux/memfd.h>/MFD_* flags --
 * no SYS_memfd_create in posix_shim.c's dispatcher, no anonymous
 * shared-memory-file concept on this port either. */
#undef HAVE_LINUX_MEMFD_H
#undef HAVE_MEMFD_CREATE
/* Python/perf_trampoline.c's Linux perf(1) JIT-symbol-map integration
 * (needs Python/asm_trampoline.S's _Py_trampoline_func_start/_end
 * symbols, mmap'd+named code arenas, and a /tmp to write perf map
 * files into) -- no perf(1) on this port, no purpose. Host's real
 * Linux x86-64 configure run detects this unconditionally (it's a
 * pyconfig.h.in AC_DEFINE gated only on __linux__/arch, not a libc
 * feature probe), so it rode along in the merge until link time
 * surfaced the missing asm symbols. */
#undef PY_HAVE_PERF_TRAMPOLINE

/* ---------------------------------------------------------------------
 * Phase 2 (see PYTHON.md) -- Modules/socketmodule.c/socketmodule.h
 * declare struct members and #include headers for every optional
 * address family CPython knows about (AF_NETLINK, AF_CAN, AF_TIPC,
 * AF_VSOCK, AF_ALG, AF_QIPCRTR, ...), all Linux-specific and all gated
 * behind their own HAVE_*_H macro. net_shim.c only ever implements
 * AF_INET (TCP/UDP, OPENISSUES.md's Networking section) -- none of
 * these would work at runtime even if they compiled, but musl's sysroot
 * settles it at compile time anyway: it vendors no linux/ uapi headers
 * at all (same story as HAVE_LINUX_RANDOM_H/HAVE_LINUX_WAIT_H/etc in
 * the Phase-1 section above). Left AF_UNIX (HAVE_SYS_UN_H)/AF_PACKET
 * (HAVE_NETPACKET_PACKET_H)/net/if.h alone -- musl's sysroot does
 * provide real headers for those (confirmed present, unlike the linux/
 * ones), so they compile; net_shim.c still won't accept those domains
 * at the socket()-call level, same "link, fail at the call site" story
 * as everything else cut only at the syscall-dispatch layer instead of
 * here.
 * --------------------------------------------------------------------- */
#undef HAVE_LINUX_NETLINK_H
#undef HAVE_ASM_TYPES_H
#undef HAVE_LINUX_QRTR_H
#undef HAVE_LINUX_TIPC_H
#undef HAVE_LINUX_CAN_H
#undef HAVE_LINUX_CAN_RAW_H
#undef HAVE_LINUX_CAN_BCM_H
#undef HAVE_LINUX_CAN_J1939_H
#undef HAVE_LINUX_VM_SOCKETS_H
#undef HAVE_SOCKADDR_ALG
/* No IPv6 anywhere on this port (LWIP_IPV6=0 in port/lwip_port/
 * lwipopts.h, matches curl_config.h's own #undef USE_IPV6) --
 * ENABLE_IPV6 also silences Modules/getaddrinfo.c's
 * getipnodebyname()/getipnodebyaddr() calls (deprecated RFC 2553
 * functions musl doesn't provide) in its IPv6 branch. */
#undef ENABLE_IPV6
/* AC_CHECK_DECL-style macros distinct from HAVE_LINUX_CAN_RAW_H itself
 * -- same AF_CAN cut, musl vendors no linux/can/raw.h regardless. */
#undef HAVE_LINUX_CAN_RAW_FD_FRAMES
#undef HAVE_LINUX_CAN_RAW_JOIN_FILTERS

/* ---------------------------------------------------------------------
 * CPython 3.14 bump (see PYTHON.md) -- Python/ceval.c's new C-stack-
 * overflow guard (hardware_stack_limits()/tstate_set_stack(), absent
 * in 3.12.8) sizes itself from Py_C_STACK_SIZE, a hardcoded 4,000,000-
 * byte guess on x86-64 with none of WIN32/__APPLE__/
 * HAVE_PTHREAD_GETATTR_NP defined (no real pthreads here). BareMetal's
 * own kernel/monitor stack (BareMetal-Firecracker's kernel.asm: `mov
 * rax, [os_StackBase] / add rax, 65536`) is a fixed 64 KiB -- nowhere
 * near 4 MB -- so subtracting the default guess underflowed and
 * tripped tstate_set_stack()'s `assert(base < top)` on every boot.
 * _Py_LINKER_THREAD_STACK_SIZE is CPython's own documented override
 * knob for exactly this (an embedder with a known, non-default thread
 * stack size). 64 KiB itself turned out too tight once actually sized
 * correctly (only just over _PyOS_MIN_STACK_SIZE's 48 KiB floor --
 * importlib bootstrap alone hit "RecursionError: Stack overflow (used
 * 37 kB)"), so python.c's own main() now switches to a dedicated
 * static buffer before running any real code instead of using
 * BareMetal's kernel stack at all; this must match that buffer's size
 * exactly. */
#define _Py_LINKER_THREAD_STACK_SIZE (1 * 1024 * 1024)

#endif
