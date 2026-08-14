// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// config_baremetal.c -- hand-written equivalent of CPython's own
// Modules/config.c (normally generated from Modules/config.c.in by the
// makesetup script, driven by Modules/Setup -- see create_builtin() in
// Python/import.c, which walks this table to satisfy "import posix"/
// "import _thread"/etc without any .so to dlopen, the same
// _PyImport_Inittab mechanism CPython itself uses for a static/
// no-dynamic-loading build. Content diffed directly against a real
// native ./configure + make run of this same release (see PYTHON.md),
// with `pwd` dropped (needs getpwuid(), no uid/gid model on this port
// -- OPENISSUES.md's Process model section) and everything else kept.
#include "Python.h"

extern PyObject* PyInit_atexit(void);
extern PyObject* PyInit_faulthandler(void);
extern PyObject* PyInit_posix(void);
extern PyObject* PyInit__signal(void);
extern PyObject* PyInit__tracemalloc(void);
extern PyObject* PyInit__codecs(void);
extern PyObject* PyInit__collections(void);
extern PyObject* PyInit_errno(void);
extern PyObject* PyInit__io(void);
extern PyObject* PyInit_itertools(void);
extern PyObject* PyInit__sre(void);
extern PyObject* PyInit__thread(void);
extern PyObject* PyInit_time(void);
extern PyObject* PyInit__typing(void);
extern PyObject* PyInit__weakref(void);
extern PyObject* PyInit__abc(void);
extern PyObject* PyInit__functools(void);
extern PyObject* PyInit__locale(void);
extern PyObject* PyInit__operator(void);
extern PyObject* PyInit__stat(void);
extern PyObject* PyInit__symtable(void);
extern PyObject* PyInit_gc(void);

extern PyObject* PyMarshal_Init(void);
extern PyObject* PyInit__imp(void);
extern PyObject* PyInit__ast(void);
extern PyObject* PyInit__tokenize(void);
extern PyObject* _PyWarnings_Init(void);
extern PyObject* PyInit__string(void);

/* Not part of a normal native build's own config.c at all -- there,
 * _socket is built as a shared extension (Modules/_socket.so), so it
 * never needs an _PyImport_Inittab entry. This port has no dynamic
 * loading (OPENISSUES.md's General section), so it's statically linked
 * in and registered here instead, the same treatment as every module
 * above. */
extern PyObject* PyInit__socket(void);

/* select()/poll() are already real (posix_shim.c's "immediate ready,
 * then block for real in the read/write/accept that follows" shim,
 * see that file's own comment) -- selectmodule.c's plain select()
 * path needs no HAVE_* gating beyond what pyconfig.h's glibc-derived
 * base already sets (HAVE_SYS_SELECT_H). math/_struct/binascii/
 * _random are all single-file, HAVE_*-gate-free Modules/*.c (verified
 * against this release's source directly), the same "just compile and
 * register" shape as _socket above -- added because http.server's
 * dependency closure (socketserver -> selectors -> select,
 * email/base64 -> struct/binascii, random -> _random/math) needs them,
 * not because any of them individually needed new port work. */
extern PyObject* PyInit_select(void);
extern PyObject* PyInit_math(void);
extern PyObject* PyInit__struct(void);
extern PyObject* PyInit_binascii(void);
extern PyObject* PyInit__random(void);

/* random.py's own "hashlib is pretty heavy to load, try lean internal
 * module first" -- sha2module.c + the HACL* Hacl_Hash_SHA2.c it wraps,
 * both portable C with no OS dependency, added instead of porting all
 * of hashlib.py's own multi-module fallback chain. */
extern PyObject* PyInit__sha2(void);

/* socket.py's own send_fds()/recv_fds() gate their `import array` on
 * `hasattr(_socket.socket, "sendmsg")` -- true here (the method exists
 * on the type even though net_shim.c has no real sendmsg/recvmsg
 * backing it), so this is a hard, not merely optional, dependency in
 * practice. Single-file, HAVE_*-gate-free like select/math/_struct/
 * binascii/_random above. */
extern PyObject* PyInit_array(void);

/* socket.py's getfqdn("0.0.0.0")/gethostbyaddr() path (server_bind()
 * on any plain HTTPServer(("0.0.0.0", port), ...)) needs
 * encodings.idna, which needs unicodedata.ucd_3_2_0 -- single-file,
 * HAVE_*-gate-free like the others (its Unicode tables are
 * unicodedata_db.h/unicodename_db.h, already generated and vendored
 * alongside unicodedata.c, no external data file). */
extern PyObject* PyInit_unicodedata(void);

struct _inittab _PyImport_Inittab[] = {
	{"atexit", PyInit_atexit},
	{"faulthandler", PyInit_faulthandler},
	{"posix", PyInit_posix},
	{"_signal", PyInit__signal},
	{"_tracemalloc", PyInit__tracemalloc},
	{"_codecs", PyInit__codecs},
	{"_collections", PyInit__collections},
	{"errno", PyInit_errno},
	{"_io", PyInit__io},
	{"itertools", PyInit_itertools},
	{"_sre", PyInit__sre},
	{"_thread", PyInit__thread},
	{"time", PyInit_time},
	{"_typing", PyInit__typing},
	{"_weakref", PyInit__weakref},
	{"_abc", PyInit__abc},
	{"_functools", PyInit__functools},
	{"_locale", PyInit__locale},
	{"_operator", PyInit__operator},
	{"_stat", PyInit__stat},
	{"_symtable", PyInit__symtable},

	/* This module lives in marshal.c */
	{"marshal", PyMarshal_Init},

	/* This lives in import.c */
	{"_imp", PyInit__imp},

	/* This lives in Python/Python-ast.c */
	{"_ast", PyInit__ast},

	/* This lives in Python/Python-tokenize.c */
	{"_tokenize", PyInit__tokenize},

	/* These entries are here for sys.builtin_module_names */
	{"builtins", NULL},
	{"sys", NULL},

	/* This lives in gcmodule.c */
	{"gc", PyInit_gc},

	/* This lives in _warnings.c */
	{"_warnings", _PyWarnings_Init},

	/* This lives in Objects/unicodeobject.c */
	{"_string", PyInit__string},

	/* See extern declaration's comment above */
	{"_socket", PyInit__socket},
	{"select", PyInit_select},
	{"math", PyInit_math},
	{"_struct", PyInit__struct},
	{"binascii", PyInit_binascii},
	{"_random", PyInit__random},
	{"_sha2", PyInit__sha2},
	{"array", PyInit_array},
	{"unicodedata", PyInit_unicodedata},

	/* Sentinel */
	{0, 0}
};
