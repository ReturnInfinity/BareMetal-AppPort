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
// frozen.c's own frozen_modules/*.h headers already embed (see
// PYTHON.md) -- those cover importlib/os/site/etc, not the codecs
// package.
//
// Rather than depend on the EXT2 disk image having real files present
// this early in boot, port/python_port/frozen_encodings/*.h are frozen
// the same way CPython freezes its own bootstrap set -- via Programs/
// _freeze_module.py, run once against a native host Python build (see
// PYTHON.md) -- and registered through PyImport_FrozenModules, the
// exact hook Python/import.c's own look_up_frozen() and
// Include/cpython/import.h's own comment ("Embedding apps may change
// this pointer to point to their favorite collection of frozen
// modules") describe: checked in preference to the built-in
// _PyImport_FrozenStdlib table, no changes to vendored CPython source
// needed. Marshalled bytecode is CPython-version-specific (a real
// _GP fault on a garbage type pointer is what stale ones look like at
// runtime -- confirmed the hard way bumping to 3.14.7, see PYTHON.md)
// -- these three headers need regenerating from that version's own
// native host build any time scripts/get-python.sh's VERSION changes:
//   build/host-python-build/python build/Python-<ver>/Programs/_freeze_module.py \
//       encodings build/Python-<ver>/Lib/encodings/__init__.py port/python_port/frozen_encodings/encodings.h
// (and the same for encodings.aliases -> encodings_aliases.h,
// encodings.utf_8 -> encodings_utf_8.h).
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
	{"encodings", _Py_M__encodings, (int)sizeof(_Py_M__encodings), 1},
	{"encodings.aliases", _Py_M__encodings_aliases, (int)sizeof(_Py_M__encodings_aliases), 0},
	{"encodings.utf_8", _Py_M__encodings_utf_8, (int)sizeof(_Py_M__encodings_utf_8), 0},
	{0, 0, 0, 0} /* sentinel */
};

void baremetal_install_frozen_modules(void)
{
	PyImport_FrozenModules = baremetal_frozen_modules;
}
