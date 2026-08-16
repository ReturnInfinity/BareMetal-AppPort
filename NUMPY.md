# NumPy Port — Scoping

**Status: not started.** This is a roadmap, not a build log — nothing
below is built yet. It exists so a future session can pick up Phase 1
without re-deriving this from scratch, the same way `PYTHON.md` records
*why* each piece of the Python port is the way it is, not just *that* it
works.

`test_numpy.py` in the repo root is the acceptance test this whole effort
targets: a pass/fail suite covering array creation, reshape/indexing,
arithmetic/broadcasting, aggregation, `numpy.linalg.det`/`inv`, sorting,
and `numpy.random.default_rng`, meant to run unmodified on the actual
BareMetal target once this is done. It currently only runs host-side
(`python3 test_numpy.py`, `pip install numpy` first) — its own docstring
says so.

## What this depends on (already done)

Dynamically loading a real compiled Python C extension now works on this
port at all — it didn't before. Two pieces made that possible:

1. **`port/dlfcn_shim.c`** — a real `dlopen()`/`dlsym()`/`dlclose()`/
   `dlerror()`, loading an ET_DYN ELF64 `.so` off the ext4 disk, hand-
   processing its `.rela.dyn`/`.rela.plt` relocations (there's no `ld.so`
   here to do it), and resolving external symbols against a curated
   export table (`dl_exports[]` in that file) rather than the full host
   symbol table.
2. **`Python/dynload_shlib.c` wired in** (`pyconfig.h`'s
   `HAVE_DYNAMIC_LOADING`, `setup.sh`'s `PYTHON_SRCS`) instead of the
   always-fails `Python/dynload_stub.c` — CPython's own real dlopen()-
   based extension loader, calling straight into (1).

Getting a real extension module (`pyexttest.c`, calling `PyModule_Create2`/
`PyArg_ParseTuple`/`PyLong_FromLong`) to actually run this way surfaced a
genuine, previously-latent bug: `port/c.ld` had a rule to reassemble
`-fdata-sections`'s split `.bss.*` sections back into one `.bss`, but no
equivalent for `.tdata`/`.tbss`. CPython's two `_Thread_local` variables
(`_Py_tss_tstate`, the current thread state, and `pkgcontext`/
`_Py_PackageContext`, the module name mid-import) landed at the exact same
address as a result. `_PyImport_SwapPackageContext()` writing the module
name right before calling a loaded module's `PyInit_*` was silently
clobbering the thread-state pointer — corrupting every CPython C-API call
the loaded module's own code made afterward. Fixed with explicit
`.tdata`/`.tbss` output-section rules in `c.ld` mirroring `.bss`'s existing
one. Both fixes are already committed and verified: `main_test.py`'s
`pyexttest` check passes 11/11 on real target hardware.

That's the foundation numpy rides on. What follows is new work.

## Key facts

- **Target numpy 1.26.4** (last 1.x release) — the first version to
  officially claim CPython 3.12 support (this port's exact target), and
  the last one before NumPy 2.0's C-ABI changes (new dtype API, NEP 50
  promotion rules) add a second layer of unfamiliar surface on top of an
  already large port. The build system is bypassed entirely either way
  (same choice already made for CPython itself — its `./configure`/
  `Makefile` were never run, see `PYTHON.md`), so 1.26.x vs. 2.x is
  purely about C source compatibility with 3.12 headers and API surface
  size, not which build tool numpy nominally uses.
- **No `pip`/`cython`/`meson` on this build host today** (checked
  directly: `pip3` not found, `python3 -m pip` reports no module named
  pip). numpy's C sources need two kinds of host-side code generation
  before they're plain, hand-compilable `.c` files (see Phase 2) — one
  of the two needs Cython specifically. Resolving this (bootstrap `pip`
  via `get-pip.py` or a system package, then `pip install cython`, or
  fetch Cython's sdist directly and run its pure-Python CLI unpackaged)
  is a real prerequisite, but only for `numpy/random` — core
  ndarray/ufunc, linalg, and fft need no Cython at all.
- **`dlfcn_shim.c`'s `dl_exports[]` is the mechanism everything below
  rides on.** It currently has a hand-picked handful of CPython C-API
  entries, enough for `pyexttest.c`. numpy's compiled extensions will
  need several hundred more (CPython C-API functions/data, libc, libm).
  Guessing that list by reading numpy's source is exactly the kind of
  thing this project avoids elsewhere (see `PYTHON.md`'s "verify
  empirically" ethos) — there's a mechanical, exact alternative instead:
  build numpy 1.26.4 normally on any machine with `pip`, then
  `nm -D --undefined-only` its compiled `.so` files for the definitive
  symbol list.
- **`numpy.linalg.lapack_lite` needs no external BLAS/LAPACK** — it's a
  bundled, portable-C, decades-old f2c translation of a minimal
  LAPACK/BLAS subset checked into numpy's own repo
  (`numpy/linalg/lapack_lite/`). This is what makes `linalg.det`/`inv`
  tractable without a separate Fortran-toolchain project.
- **numpy isn't one `.so`.** Real modules needed:
  `numpy/core/_multiarray_umath.so` (the big one — ndarray, dtype
  system, ufunc machinery; everything else imports this first),
  `numpy/linalg/_umath_linalg.so` + `lapack_lite.so`,
  `numpy/fft/_pocketfft_internal.so`, and `numpy/random`'s 8 modules
  (`_generator`, `_bounded_integers`, `_common`, `_mt19937`, `_pcg64`,
  `_philox`, `_sfc64`, `mtrand`). `_multiarray_tests`/`_umath_tests`/
  `_rational_tests`/`_struct_ufunc_tests`/`_operand_flag_tests`/`_simd`
  are test-only — skip them, don't port them.
- **SIMD dispatch has to be flattened to one baseline path.** numpy's
  real build generates/selects multiple object variants at runtime
  (`--cpu-baseline`/`--cpu-dispatch`); this port has no runtime CPU
  dispatch machinery and shouldn't grow one just for this — compile
  against the SSE2 baseline only (always present on x86-64), exclude the
  AVX2/AVX512-specific translation units entirely. Exact file-level
  boundaries need verifying once the source tree is actually in hand.
- **Threading isn't a new requirement.** `lapack_lite` is naive/single-
  threaded, ordinary array/ufunc ops don't spawn threads — this port's
  existing `thread_shim.c`/GIL support (already proven via
  `main_test.py`'s `_thread` check) is all that's needed.

## Phased roadmap

### Phase 0 — prerequisites
- New `scripts/get-numpy.sh` (mirrors `scripts/get-python.sh`): fetch
  numpy 1.26.4's sdist tarball.
- Resolve the Cython gap (see above) — can be deferred/parallelized,
  doesn't block Phase 1 or the non-`random` phases.

### Phase 1 — symbol surface discovery (do this first)
- On any machine with `pip`, install/build numpy 1.26.4 against a real
  CPython 3.12, then `nm -D --undefined-only` its
  `_multiarray_umath*.so` (start with just this one) for the exact,
  complete external-symbol list.
- Partition into libc/libm entries (~40-80, straightforward additions),
  CPython C-API *function* entries (hundreds, mechanical — all already
  compiled into `python.app`'s core per `setup.sh`'s `PYTHON_SRCS`), and
  CPython C-API *data* entries (`PyExc_*`, `PyLong_Type`/`PyFloat_Type`/
  etc., `_Py_NoneStruct` — need the `(void*)&Symbol` address-of
  convention `dlfcn_shim.c`'s export-table comment already documents,
  not a plain function-pointer entry).
- Repeat per additional submodule once each is reached in Phase 4 — no
  need to front-load all of them.

### Phase 2 — host-side code generation
- Run `numpy/distutils/conv_template.py` over
  `numpy/core/src/**/*.c.src`/`*.h.src` to materialize real `.c`/`.h`
  files — pure Python, same "generate once, hand-copy the flat result
  into `build/`" pattern `setup.sh` already uses for CPython's own
  pegen/deepfreeze output.
- Run `numpy/core/code_generators/generate_umath.py` (and any sibling
  generators it needs) for the ufunc loop tables.
- Run Cython over `numpy/random/*.pyx` once Phase 0's tooling gap is
  resolved.

### Phase 3 — cross-compile `numpy.core._multiarray_umath`
The largest, highest-risk single piece — dozens of files, heavy
macro/template use, the first thing of numpy's scale this toolchain has
ever compiled. Build via `build-module.sh` (already supports pass-through
`-I`/`-D`/`-isystem` flags) with:
```
-I numpy/core/include -I numpy/core/src/common -I numpy/core/src/multiarray \
-I numpy/core/src/umath -I build/Python-3.12.8/Include -I port/python_port \
-isystem "$(gcc -print-file-name=include)"
```
SSE2-baseline-only per the SIMD note above. Milestone:
`import numpy.core._multiarray_umath` succeeds (every relocation
resolves). Expect a slow, iterative "hits an unresolved symbol → add it
to `dl_exports[]` → rebuild → retry" loop, exactly like this port's own
`_PyArg_ParseTuple_SizeT` discovery during the dlopen() work, just at much
larger scale.

### Phase 4 — linalg, fft, random
- `numpy.linalg.lapack_lite` + `_umath_linalg`: plain C, same
  `build-module.sh` pattern, no new host tooling.
- `numpy.fft._pocketfft_internal`: same, portable C (pocketfft).
- `numpy.random`'s 8 modules: needs Phase 2's Cython-generated `.c`;
  otherwise the same compile/link pattern.

### Phase 5 — on-disk package layout
- Extend `port/python_port/install-stdlib.sh`'s exact pattern (or add a
  sibling `install-numpy.sh`) to write numpy's pure-Python
  `__init__.py`/support files onto `/pylib/numpy/...` via `debugfs -w` —
  trace the real import closure the same way that script's own header
  describes doing for `json`/`encodings.ascii` (diff `sys.modules`
  before/after a real `import numpy`), not shipping numpy's whole
  Python-level source speculatively.
- Install each compiled `.so` at its real package-relative path
  (`scripts/install-file.sh`, already built and working) — the bare
  `.so` fallback in `_PyImport_DynLoadFiletab` (already relied on for
  `pyexttest.so`) means the `SOABI` tag stays irrelevant here too.

### Phase 6 — acceptance
`test_numpy.py` is the finish line — install it as `/pylib/main.py` the
same way `main_test.py` is deployed today, boot, check for
`N/N checks passed`. No changes needed to that file itself; it already
covers this plan's full scope.

## Effort/risk

Phases 1 and 2 are mechanical and low-risk once the Cython gap is
resolved. Phase 3 is the real unknown — expect genuine surprises specific
to this port's freestanding/musl environment (libm coverage gaps,
alignment assumptions, anything numpy's C code assumes about a hosted
libc this port's minimal one doesn't provide) that can't be fully
enumerated until the source is actually in hand and being compiled.
Overall this is a multi-session undertaking on the scale of `PYTHON.md`'s
own multi-phase history — Phase 1 (symbol discovery) is the concrete,
self-contained next step whenever work resumes.
