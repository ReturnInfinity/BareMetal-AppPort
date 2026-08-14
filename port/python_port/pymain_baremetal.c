// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// pymain_baremetal.c -- EXPERIMENTAL, Phase 1/2 (see ../../PYTHON_PORT.md).
// This port's own "main()" for the interpreter, in place of CPython's
// normal Programs/python.c (Py_BytesMain(argc, argv), which drives full
// command-line parsing and Modules/getpath.c's filesystem-searching
// calculate_path() to find an installed stdlib tree). Neither fits
// Phase 1's goal (does the interpreter even start, running only the
// frozen bootstrap modules Python/deepfreeze/deepfreeze.c already
// embeds -- see PYTHON_PORT.md): crt0.c fabricates an empty argv/envp
// (OPENISSUES.md's Process model section) so there's no real command
// line to parse, and there's no installed lib/python3.12/ tree on this
// port's EXT2 image yet for calculate_path() to find (that's Phase 3).
//
// Instead this uses CPython's lower-level embedding API directly
// (Py_InitializeFromConfig(), documented for exactly this "embed
// Python without a normal python3 command line" case) with a hand-
// built PyConfig that skips both of those: module_search_paths_set=1
// with zero real entries (sys.path ends up empty -- the frozen
// bootstrap modules importlib needs are reached through the frozen
// table directly, not a sys.path search) and site_import=0 (site.py
// itself is one of the frozen modules, but pulls in site-packages
// scanning this port has no real use for yet).
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

	// Empty on purpose (see file header) -- calculate_path()'s real
	// filesystem search is Phase 3's job, once there's a real
	// lib/python3.12/ tree on the EXT2 image to find.
	config.module_search_paths_set = 1;

	status = Py_InitializeFromConfig(&config);
	if (PyStatus_Exception(status)) {
		goto fail;
	}
	PyConfig_Clear(&config);

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

	return Py_FinalizeEx() < 0 ? 1 : 0;

fail:
	PyConfig_Clear(&config);
	Py_ExitStatusException(status);
}
