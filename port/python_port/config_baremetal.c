// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// config_baremetal.c -- EXPERIMENTAL, Phase 1 (see ../../PYTHON_PORT.md).
// Hand-written equivalent of CPython's own Modules/config.c (normally
// generated from Modules/config.c.in by the makesetup script, driven by
// Modules/Setup -- see create_builtin() in Python/import.c, which walks
// this table to satisfy "import posix"/"import _thread"/etc without any
// .so to dlopen, the same _PyImport_Inittab mechanism CPython itself
// uses for a static/no-dynamic-loading build. Content diffed directly
// against build/host-python-build/Modules/config.c (a real ./configure
// + make run of this same release -- see PYTHON_PORT.md's Phase 1),
// with `pwd` dropped (needs getpwuid(), no uid/gid model on this port --
// OPENISSUES.md's Process model section) and everything else kept.
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

	/* Sentinel */
	{0, 0}
};
