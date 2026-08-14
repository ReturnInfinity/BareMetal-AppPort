// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// pymain_baremetal.c -- EXPERIMENTAL, Phase 1/2/3 (see ../../PYTHON_PORT.md).
// This port's own "main()" for the interpreter, in place of CPython's
// normal Programs/python.c (Py_BytesMain(argc, argv), which drives full
// command-line parsing and Modules/getpath.c's filesystem-searching
// calculate_path() to find an installed stdlib tree). Neither fits
// this port's own goals: crt0.c fabricates an empty argv/envp
// (OPENISSUES.md's Process model section) so there's no real command
// line to parse, and calculate_path()'s search assumes an installed
// prefix layout (bin/, lib/pythonX.Y/, ...) this port doesn't have.
//
// Instead this uses CPython's lower-level embedding API directly
// (Py_InitializeFromConfig(), documented for exactly this "embed
// Python without a normal python3 command line" case) with a hand-
// built PyConfig: site_import=0 (site.py itself is one of the frozen
// modules, but pulls in site-packages scanning this port has no real
// use for yet) and module_search_paths_set=1 with one real entry,
// "/pylib" -- see Phase 3 below.
//
// Phase 3 (see PYTHON_PORT.md): "/pylib" is a real directory on the
// EXT2 disk image (port/python_port/install-stdlib-phase3.sh writes
// it there via debugfs, no host root/loop-mount needed), holding a
// curated slice of CPython's own unmodified Lib/*.py files -- not the
// frozen-bytecode approach Phase 1/2 used for `encodings`/`_socket`'s
// gaps. importlib._bootstrap_external's PathFinder walks this path the
// normal way, using ext4_shim.c's now-real stat()/open()/readdir() --
// no new C code needed here either, same story as Phase 2's
// _socket.
#include "Python.h"

extern void baremetal_install_frozen_modules(void);

int main(void)
{
	PyStatus status;
	PyConfig config;

	PyConfig_InitPythonConfig(&config);

	baremetal_install_frozen_modules();

	config.site_import = 0;
	config.use_environment = 0;
	config.user_site_directory = 0;
	config.install_signal_handlers = 0;   // no real signal delivery to install a handler for (OPENISSUES.md)
	config.parse_argv = 0;                // crt0.c's argv is always empty anyway
	config.pathconfig_warnings = 0;       // suppress warnings about the empty search path below
	config.configure_c_stdio = 1;         // stdout/stderr -> posix_shim.c's write(), same as printf() in every other app here

	// Hash randomization needs a real source of randomness
	// (_Py_HashRandomization_Init(), Python/bootstrap_hash.c) that
	// this port doesn't have yet -- pyconfig_baremetal.h leaves
	// HAVE_GETRANDOM/HAVE_GETENTROPY undefined (OPENISSUES.md: nothing
	// backs /dev/urandom-equivalent randomness for application code),
	// and CPython treats failing to find *any* entropy source as
	// fatal by default, not a silent fallback. Equivalent to
	// PYTHONHASHSEED=0 -- the standard, documented way to embed
	// CPython in an environment with no wired-up RNG yet, not a hack;
	// makes dict/set iteration order and hash() values deterministic
	// instead of random, a real (tracked) Phase-1 limitation rather
	// than a silently-accepted one -- see PYTHON_PORT.md. A real fix
	// means giving bootstrap_hash.c an RDRAND-backed source the same
	// way port/mbedtls_port/entropy_hardware_poll.c and
	// port/libsodium_port/randombytes_baremetal.c already do for
	// mbedTLS/libsodium.
	config.use_hash_seed = 1;
	config.hash_seed = 0;

	status = PyConfig_SetString(&config, &config.program_name, L"python");
	if (PyStatus_Exception(status)) {
		goto fail;
	}

	// Phase 3 (see file header) -- a real, single-entry sys.path,
	// pointing at install-stdlib-phase3.sh's target directory on the
	// EXT2 image. Not calculate_path()'s search (still bypassed, see
	// file header) -- just this one hand-picked entry.
	config.module_search_paths_set = 1;
	status = PyWideStringList_Append(&config.module_search_paths, L"/pylib");
	if (PyStatus_Exception(status)) {
		goto fail;
	}

	status = Py_InitializeFromConfig(&config);
	if (PyStatus_Exception(status)) {
		goto fail;
	}
	PyConfig_Clear(&config);

	// Phase 3 (see file header): frozen *packages* (only "encodings"
	// here, see frozen_encodings_baremetal.c) get an empty --
	// importlib._bootstrap.FrozenImporter builds their ModuleSpec with
	// submodule_search_locations=[], not None, since it's a package --
	// but still real, appendable __path__. Left alone, that means
	// PathFinder (consulted after FrozenImporter in sys.meta_path) has
	// nowhere to look for a submodule that isn't itself frozen (e.g.
	// "encodings.ascii" -- "encodings.aliases"/".utf_8" don't hit this,
	// they're matched directly by FrozenImporter on their own exact
	// frozen name, never touching encodings.__path__ at all). Appending
	// /pylib's copy here -- found the hard way, by hitting exactly this
	// ModuleNotFoundError first -- gets both worlds: Phase 1 still
	// boots with zero filesystem dependency (this is a no-op if /pylib
	// isn't there), and once it is, previously-unreachable real
	// submodules of a frozen package become importable too.
	PyRun_SimpleString(
		"import encodings\n"
		"encodings.__path__.append('/pylib/encodings')\n"
	);

	// Phase 2 (see PYTHON_PORT.md): exercises _socket (the C extension
	// module, Modules/socketmodule.c -- see config_baremetal.c) end to
	// end -- module init, its constant tables, and a real socket()/
	// bind()/close() round trip through posix_shim.c -> net_shim.c.
	// No getsockname()/getpeername() here -- not in posix_shim.c's
	// SYS_ dispatch table (-ENOSYS), a real gap, not a test omission.
	// Deliberately does *not* attempt a real connect()/gethostbyname()
	// either: this build's host environment has tap0 configured but
	// down (no carrier), which is a test-host networking setup
	// question (see this repo's own 2-run.sh warning), not something
	// Phase 2's code needs a working network to prove. `import socket`
	// (the pure-Python wrapper in Lib/socket.py) isn't available yet
	// either -- that .py file isn't frozen or on disk, only _socket is.
	PyRun_SimpleString(
		"import _socket\n"
		"print('_socket constants:', _socket.AF_INET, _socket.SOCK_STREAM)\n"
		"s = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)\n"
		"print('socket() fileno:', s.fileno())\n"
		"s.bind(('0.0.0.0', 0))\n"
		"print('bind() ok')\n"
		"s.close()\n"
		"print('close() ok')\n"
		"print(1 + 1)\n"
	);

	// Phase 3 (see file header): real filesystem imports off /pylib --
	// works now because of the encodings.__path__ patch above.
	// encodings.ascii isn't frozen (only encodings/__init__.py,
	// .aliases, .utf_8 are), so a successful import here can only be
	// coming from the real /pylib/encodings/ascii.py file, found via
	// PathFinder walking the patched __path__.
	PyRun_SimpleString(
		"import encodings.ascii\n"
		"print('encodings.ascii:', encodings.ascii.getregentry().name)\n"
	);

	// json was never frozen at all (Phase 1/2 didn't touch it) --
	// success here can only be real PathFinder-driven filesystem
	// imports off /pylib, exercising json's own 4-file package plus
	// its dependency closure (collections/re/enum/functools/etc --
	// see install-stdlib-phase3.sh's own comment for how that list was
	// derived) and a real package-relative import (json/decoder.py's
	// "from json import scanner"). Run as its own PyRun_SimpleString
	// call so the encodings.ascii outcome above can't affect it either
	// way.
	PyRun_SimpleString(
		"import json\n"
		"print('json.dumps:', json.dumps({'a': [1, 2, 3]}))\n"
		"print('json.loads:', json.loads('{\"a\": [1, 2, 3]}'))\n"
	);

	return Py_FinalizeEx() < 0 ? 1 : 0;

fail:
	PyConfig_Clear(&config);
	Py_ExitStatusException(status);
}
