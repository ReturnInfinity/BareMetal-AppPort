// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// frozen_encodings_baremetal.c -- CPython's core init
// (init_fs_encoding(), Python/pylifecycle.c) unconditionally imports
// the `encodings` package to resolve the filesystem codec, before a
// single line of app code runs -- unlike hash randomization
// (python.c's use_hash_seed), there's no PyConfig knob to skip this.
// `encodings` isn't one of the frozen bootstrap modules Python/
// deepfreeze/deepfreeze.c already embeds (see PYTHON.md) -- those
// cover importlib/os/site/etc, not the codecs package.
//
// Rather than depend on the EXT2 disk image having real files present
// this early in boot, these three files are frozen the same way
// CPython freezes its own bootstrap set -- via Programs/
// _freeze_module.py, run once against a native host Python build (see
// PYTHON.md) -- and registered through PyImport_FrozenModules, the
// exact hook Python/import.c's own look_up_frozen() and
// Include/cpython/import.h's own comment ("Embedding apps may change
// this pointer to point to their favorite collection of frozen
// modules") describe: checked in preference to the built-in
// _PyImport_FrozenStdlib table, no changes to vendored CPython source
// needed. get_code is NULL for all three (unlike deepfreeze's entries)
// -- that's the plain marshalled-bytes path
// PyImport_ImportFrozenModuleObject() falls back to when there's no
// deepfreeze-generated fast constructor, the same path frozen modules
// used for years before deepfreeze existed in 3.11+.
//
// Only enough of Lib/encodings/ to satisfy the filesystem/stdio codec
// lookup -- encodings/__init__.py (the package itself, is_package=true),
// encodings/aliases.py (name->codec table __init__.py imports), and
// encodings/utf_8.py (the codec python.c's config.filesystem_encoding=
// "utf-8" asks for). A real program importing a different codec (e.g.
// "latin-1") needs its own file on /pylib/encodings/ (install-main.sh)
// -- python.c's encodings.__path__ fix makes that work once it's
// there; this isn't the whole encodings package, just the slice
// startup itself needs before any real file can be read at all.
#include "Python.h"

#include "frozen_encodings/encodings.h"
#include "frozen_encodings/encodings_aliases.h"
#include "frozen_encodings/encodings_utf_8.h"

static const struct _frozen baremetal_frozen_modules[] = {
	{"encodings", _Py_M__encodings, (int)sizeof(_Py_M__encodings), 1, NULL},
	{"encodings.aliases", _Py_M__encodings_aliases, (int)sizeof(_Py_M__encodings_aliases), 0, NULL},
	{"encodings.utf_8", _Py_M__encodings_utf_8, (int)sizeof(_Py_M__encodings_utf_8), 0, NULL},
	{0, 0, 0, 0, 0} /* sentinel */
};

void baremetal_install_frozen_modules(void)
{
	PyImport_FrozenModules = baremetal_frozen_modules;
}
