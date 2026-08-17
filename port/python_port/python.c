// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// python.c -- this port's own "main()" for the CPython interpreter, in
// place of CPython's normal Programs/python.c (Py_BytesMain(argc,
// argv), which drives full command-line parsing and Modules/getpath.c's
// filesystem-searching calculate_path() to find an installed stdlib
// tree). Neither fits this port's own goals: crt0.c fabricates an
// empty argv/envp (OPENISSUES.md's Process model section) so there's
// no real command line to parse, and calculate_path()'s search assumes
// an installed prefix layout (bin/, lib/pythonX.Y/, ...) this port
// doesn't have. See PYTHON.md for the full account of how this port's
// CPython build works and what it took to get here.
//
// Instead this uses CPython's lower-level embedding API directly
// (Py_InitializeFromConfig(), documented for exactly this "embed
// Python without a normal python3 command line" case) with a hand-
// built PyConfig, then runs PYMAIN_SCRIPT_PATH -- a real .py file on
// the EXT2 disk image (see port/python_port/install-main.sh) -- as the
// program. Replace that file's content to run something else; nothing
// here needs to change.
#include "Python.h"
#include <stdio.h>
#include <stdlib.h>

extern void baremetal_install_frozen_modules(void);

// The program to run -- a real file on the EXT2 disk image, not baked
// into this binary. port/python_port/install-main.sh writes it there.
#define PYMAIN_SCRIPT_PATH "/pylib/main.py"

// CPython 3.14 added a real C-stack-overflow guard (Python/ceval.c's
// hardware_stack_limits()/tstate_set_stack(), absent in 3.12.8) that
// measures the *actual* machine stack via __builtin_frame_address(0)
// -- unlike 3.12.8's simple recursion-depth counter, which never cared
// how much real stack was behind it. BareMetal's own kernel/monitor
// stack (BareMetal-Firecracker's kernel.asm: `mov rax, [os_StackBase]
// / add rax, 65536`) is a fixed 64 KiB, which left just enough room to
// pass pyconfig.h's _Py_LINKER_THREAD_STACK_SIZE sizing but not enough
// to actually run: importlib._bootstrap alone hit "RecursionError:
// Stack overflow (used 37 kB)" against that budget's ~32 KiB soft
// limit. Rather than shrink every other app's shared crt0.c stack,
// python.app switches to this dedicated, much larger one before doing
// any real work -- safe because exit()/_exit() (posix_shim.c's
// sys_exit(), see crt0.c's own __bmos_entry_sp comment) never unwinds
// back through main()'s call frames; it restores RSP from the
// original entry point directly, so main() never needs to "return"
// through whatever stack it happened to be using (the pushed return
// address below is never used -- python_main() is noreturn -- but
// `call`, not `jmp`, is what leaves RSP at the SysV ABI's required
// 8-mod-16 offset for a function's own prologue; `jmp` would leave it
// 16-aligned instead, which faulted (#GP) the first real MOVAPS
// python_main() ran against it). python_main() is deliberately
// non-static (like crt0.c's _start_c()) so the `call` below resolves
// to a real relocation instead of being eligible for --gc-sections to
// drop as unreferenced.
#define PY_C_STACK_BYTES (1 * 1024 * 1024)
static unsigned char python_c_stack[PY_C_STACK_BYTES] __attribute__((aligned(16)));

void python_main(void) __attribute__((noreturn));

int main(void)
{
	__asm__ volatile(
		"leaq python_c_stack+%c0(%%rip), %%rsp\n\t"
		"call python_main\n\t"
		:: "i" (PY_C_STACK_BYTES) : "memory"
	);
	__builtin_unreachable();
}

void python_main(void)
{
	PyStatus status;
	PyConfig config;

	PyConfig_InitPythonConfig(&config);

	baremetal_install_frozen_modules();

	config.site_import = 0;               // site.py is frozen, but its site-packages scanning has no real use here
	config.use_environment = 0;
	config.user_site_directory = 0;
	config.install_signal_handlers = 0;   // no real signal delivery to install a handler for (OPENISSUES.md)
	config.parse_argv = 0;                // crt0.c's argv is always empty anyway
	config.pathconfig_warnings = 0;       // suppress warnings about the single search path entry below
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
	// instead of random -- see PYTHON.md. A real fix means giving
	// bootstrap_hash.c an RDRAND-backed source the same way
	// port/mbedtls_port/entropy_hardware_poll.c and
	// port/libsodium_port/randombytes_baremetal.c already do for
	// mbedTLS/libsodium.
	config.use_hash_seed = 1;
	config.hash_seed = 0;

	status = PyConfig_SetString(&config, &config.program_name, L"python");
	if (PyStatus_Exception(status)) {
		goto fail;
	}

	// A real, single-entry sys.path, pointing at install-stdlib.sh's
	// target directory on the EXT2 image -- not calculate_path()'s
	// search (still bypassed, see file header), just this one
	// hand-picked entry.
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

	// Frozen *packages* (only "encodings" here, see
	// frozen_encodings_baremetal.c) get an empty --
	// importlib._bootstrap.FrozenImporter builds their ModuleSpec with
	// submodule_search_locations=[], not None, since it's a package --
	// but still real, appendable __path__. Left alone, that means
	// PathFinder (consulted after FrozenImporter in sys.meta_path) has
	// nowhere to look for a submodule that isn't itself frozen (e.g.
	// "encodings.ascii" -- "encodings.aliases"/".utf_8" don't hit this,
	// they're matched directly by FrozenImporter on their own exact
	// frozen name, never touching encodings.__path__ at all). Appending
	// /pylib's copy here gets both worlds: this still boots with zero
	// filesystem dependency if /pylib isn't there (a no-op append to an
	// unused list), and once it is, previously-unreachable real
	// submodules of a frozen package become importable too. See
	// PYTHON.md for how this was found.
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
		fprintf(stderr, "python.c: could not open " PYMAIN_SCRIPT_PATH "\n");
		rc = 1;
	}

	// A plain `return` here would try to unwind back through main()'s
	// call frame the normal way -- but main() abandoned that frame's
	// stack for good the moment it jumped to python_main() (see this
	// function's own header comment), so there's nothing valid to
	// return into. exit() is the same path a normal `return rc` from
	// main() would end up at anyway (via __libc_start_main), just
	// taken directly.
	exit((Py_FinalizeEx() < 0 || rc != 0) ? 1 : 0);

fail:
	PyConfig_Clear(&config);
	Py_ExitStatusException(status);
}
