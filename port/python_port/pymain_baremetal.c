// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// pymain_baremetal.c -- EXPERIMENTAL (see ../../PYTHON_PORT.md, whose
// Phase 1/2/3 built and boot-verified everything below: frozen
// bootstrap modules, real sockets via _socket, and real filesystem
// imports off /pylib). Runs PYMAIN_SCRIPT_PATH (a real .py file on the
// EXT2 disk image, see port/python_port/install-stdlib-phase3.sh for
// how to put files there) as the program -- replace that file's
// content to run something else; nothing here needs to change.
//
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
#include <stdio.h>

extern void baremetal_install_frozen_modules(void);

// The program to run -- a real file on the EXT2 disk image (Phase 3),
// not baked into this binary. Change this (or, better, make it
// something install-stdlib-phase3.sh-adjacent tooling writes
// per-deployment) rather than hand-editing Python source into this .c
// file the way pymain_baremetal.c's own Phase 1/2/3 test snippets used
// to.
#define PYMAIN_SCRIPT_PATH "/pylib/main.py"

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

	// Run the actual program -- see PYMAIN_SCRIPT_PATH's own comment.
	// PyRun_SimpleFileExFlags (not PyRun_AnyFileExFlags) always treats
	// this as a plain script, not an interactive prompt -- isatty()
	// isn't a meaningful question for an EXT2 file, and there's no
	// real terminal on the other end of this port's serial console
	// input path to be interactive with anyway.
	int rc;
	FILE *fp = fopen(PYMAIN_SCRIPT_PATH, "r");
	if (fp != NULL) {
		rc = PyRun_SimpleFileExFlags(fp, PYMAIN_SCRIPT_PATH, 1, NULL);
	} else {
		fprintf(stderr, "pymain_baremetal: could not open " PYMAIN_SCRIPT_PATH "\n");
		rc = 1;
	}

	return (Py_FinalizeEx() < 0 || rc != 0) ? 1 : 0;

fail:
	PyConfig_Clear(&config);
	Py_ExitStatusException(status);
}
