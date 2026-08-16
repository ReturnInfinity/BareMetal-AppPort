// =============================================================================
// BareMetal -- a 64-bit OS written in Assembly for x86-64 systems
// Copyright (C) 2008-2026 Return Infinity -- see LICENSE.TXT
//
// dlfcn_shim.c -- dlopen()/dlsym()/dlclose()/dlerror() for BareMetal apps.
//
// musl is built here with --disable-shared (see musl_port/
// musl-1.2.6-config.mak), so libc.a only carries weak stub versions of
// these four (src/ldso/dlopen.c etc in the musl tree -- always fail).
// The strong definitions below win the link against those stubs, the same
// way ext4_shim.c/tls_shim.c/etc already layer real implementations under
// musl's own surface.
//
// What this loads: a real ET_DYN ELF64 shared object built by
// ../build-module.sh -- NOT the app pipeline, which produces
// -fno-pic -fno-pie -mcmodel=large flat binaries with no relocation
// info at all, incompatible with loading at a runtime-chosen address.
//
// The module is read whole off the ext4-mounted disk via plain
// open()/read()/close() (see sqlite_vfs.c for the same pattern), copied
// into a malloc()'d block, and its .rela.dyn/.rela.plt entries are
// processed by hand -- there's no ld.so here to do it. This works with
// zero extra plumbing to make the copy executable: posix_shim.c's heap
// (which malloc() draws from) is one flat RWX arena, same as the rest
// of the app's own image -- see that file's header comment.
//
// Symbol resolution for anything a module references but doesn't define
// itself goes through dl_exports[] below -- a curated table, not a
// search over the host's own symbols (build-app.sh's objcopy -O binary
// step throws the host's symtab away, and exposing every host symbol to
// a loaded module is more than a plugin-style loader like this needs).
// Extend that table to expose more of the host to your modules.
// =============================================================================

// strtold_l (numpy's dl_exports[] entries below -- see ../NUMPY.md's
// Phase 1) is a GNU extension musl's own stdlib.h only declares under
// _GNU_SOURCE.
#define _GNU_SOURCE

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
// The next seven are only here for numpy's dl_exports[] entries below
// (see ../NUMPY.md's Phase 1) -- musl already provides all of them,
// nothing else in this file needed them before. Real declarations from
// real headers, not generic ones: unlike the CPython C-API block below,
// these have proper public prototypes readily available, so there's no
// reason to guess at them.
#include <math.h>
#include <complex.h>
#include <fenv.h>
#include <locale.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>

#include "libBareMetal.h"

// Minimal hand-written declarations for the three CPython C-API entries
// dl_exports[] below needs the addresses of -- deliberately not
// #include <Python.h>: this file is compiled into *every* app
// (build-app.sh), Python or not, and Python.h/pyconfig.h assume a build
// setup (an -I onto Python's Include tree, PY_SSIZE_T_CLEAN, etc) a
// generic shim like this one shouldn't force onto unrelated apps. These
// just need to be ABI-compatible enough to take an address from --
// port/python_port/ itself (and any real extension module, e.g.
// ../pyexttest.c) is what actually calls through them against the real
// Python.h declarations.
struct _object;
typedef struct _object PyObject;
struct PyModuleDef;
extern int PyArg_ParseTuple(PyObject *args, const char *format, ...);
// PY_SSIZE_T_CLEAN (the recommended, increasingly default-assumed way to
// write an extension -- see ../pyexttest.c) makes PyArg_ParseTuple a
// macro for this real symbol instead, so both need exporting: which one
// a given module's calls actually resolve to at link time depends on
// whether that module itself defined PY_SSIZE_T_CLEAN before #include
// <Python.h>, not on anything dlfcn_shim.c controls.
extern int _PyArg_ParseTuple_SizeT(PyObject *args, const char *format, ...);
extern PyObject *PyModule_Create2(struct PyModuleDef *module, int module_api_version);
extern PyObject *PyLong_FromLong(long v);

// Bulk CPython C-API additions -- see ../NUMPY.md's Phase 1. Found by
// building numpy 1.26.4's real PyPI wheel elsewhere, extracting
// numpy/core/_multiarray_umath*.so, and running
// `nm -D --undefined-only`/`readelf -sW` on it for the definitive list
// of every external symbol it references, then cross-checking each
// Py*/_Py* name against build/Python-3.12.8/Include's own
// PyAPI_FUNC()/PyAPI_DATA() macros (grep -rhoP
// 'PyAPI_(DATA|FUNC)\([^)]*\)\s+NAME\b') to classify function vs. data
// -- not guessed. Declared generically (DL_FUNC_DECL = extern void
// NAME(void), DL_DATA_DECL = extern char NAME) rather than each one's
// real prototype: correct prototypes for ~300 symbols would be a lot of
// error-prone busywork for no benefit, since nothing in this file ever
// calls through these declarations -- only F()/D() below take their
// address, which doesn't depend on the declared signature at all, only
// the linker-visible *name*. (The three hand-typed ones above stay as
// they are -- already correct, no reason to touch them.)
//
// A ~500-entry wheel's own undefined-symbol list isn't 1:1 with what
// *this port's* build of _multiarray_umath will actually need: the
// wheel links real OpenBLAS (cblas_*/LAPACKE_* symbols, excluded here
// entirely -- NUMPY.md's plan is lapack_lite's bundled portable-C
// fallback, no external BLAS), and several entries are glibc-specific
// artifacts of the wheel's own build that won't appear when compiling
// fresh against musl headers (_IO_getc/fseeko64/ftello64/lseek64 are
// glibc's LFS64/getc-macro internals for plain getc/fseeko/ftello/lseek
// -- already covered below by their ordinary names instead; __ctype_b_loc/
// __ctype_tolower_loc/__cxa_finalize/__gmon_start__/_ITM_*/backtrace/
// dladdr/__popcountdi2 are omitted outright -- the last is a libgcc
// helper build-module.sh's own module link should pull in directly, not
// something to route through this table).
#define DL_FUNC_DECL(name) extern void name(void)
#define DL_DATA_DECL(name) extern char name

DL_FUNC_DECL(PyArg_ParseTupleAndKeywords);
DL_FUNC_DECL(PyArg_UnpackTuple);
DL_FUNC_DECL(PyBool_FromLong);
DL_FUNC_DECL(PyBuffer_Release);
DL_FUNC_DECL(PyBytes_AsString);
DL_FUNC_DECL(PyBytes_AsStringAndSize);
DL_FUNC_DECL(PyBytes_FromString);
DL_FUNC_DECL(PyBytes_FromStringAndSize);
DL_FUNC_DECL(PyBytes_Size);
DL_FUNC_DECL(PyCallable_Check);
DL_FUNC_DECL(PyCapsule_GetContext);
DL_FUNC_DECL(PyCapsule_GetPointer);
DL_FUNC_DECL(PyCapsule_Import);
DL_FUNC_DECL(PyCapsule_IsValid);
DL_FUNC_DECL(PyCapsule_New);
DL_FUNC_DECL(PyCapsule_SetContext);
DL_FUNC_DECL(PyCapsule_SetName);
DL_FUNC_DECL(PyComplex_AsCComplex);
DL_FUNC_DECL(PyComplex_FromCComplex);
DL_FUNC_DECL(PyComplex_FromDoubles);
DL_FUNC_DECL(PyComplex_ImagAsDouble);
DL_FUNC_DECL(PyComplex_RealAsDouble);
DL_FUNC_DECL(PyContextVar_Get);
DL_FUNC_DECL(PyContextVar_New);
DL_FUNC_DECL(PyContextVar_Set);
DL_FUNC_DECL(PyDictProxy_New);
DL_FUNC_DECL(PyDict_Contains);
DL_FUNC_DECL(PyDict_Copy);
DL_FUNC_DECL(PyDict_DelItem);
DL_FUNC_DECL(PyDict_DelItemString);
DL_FUNC_DECL(PyDict_GetItem);
DL_FUNC_DECL(PyDict_GetItemString);
DL_FUNC_DECL(PyDict_GetItemWithError);
DL_FUNC_DECL(PyDict_Merge);
DL_FUNC_DECL(PyDict_New);
DL_FUNC_DECL(PyDict_Next);
DL_FUNC_DECL(PyDict_SetItem);
DL_FUNC_DECL(PyDict_SetItemString);
DL_FUNC_DECL(PyDict_Size);
DL_FUNC_DECL(PyErr_CheckSignals);
DL_FUNC_DECL(PyErr_Clear);
DL_FUNC_DECL(PyErr_ExceptionMatches);
DL_FUNC_DECL(PyErr_Fetch);
DL_FUNC_DECL(PyErr_Format);
DL_FUNC_DECL(PyErr_GivenExceptionMatches);
DL_FUNC_DECL(PyErr_NoMemory);
DL_FUNC_DECL(PyErr_NormalizeException);
DL_FUNC_DECL(PyErr_Occurred);
DL_FUNC_DECL(PyErr_Print);
DL_FUNC_DECL(PyErr_Restore);
DL_FUNC_DECL(PyErr_SetFromErrno);
DL_FUNC_DECL(PyErr_SetNone);
DL_FUNC_DECL(PyErr_SetObject);
DL_FUNC_DECL(PyErr_SetString);
DL_FUNC_DECL(PyErr_WarnEx);
DL_FUNC_DECL(PyErr_WarnFormat);
DL_FUNC_DECL(PyErr_WriteUnraisable);
DL_FUNC_DECL(PyEval_GetBuiltins);
DL_FUNC_DECL(PyEval_RestoreThread);
DL_FUNC_DECL(PyEval_SaveThread);
DL_FUNC_DECL(PyException_SetCause);
DL_FUNC_DECL(PyException_SetContext);
DL_FUNC_DECL(PyException_SetTraceback);
DL_FUNC_DECL(PyFloat_AsDouble);
DL_FUNC_DECL(PyFloat_FromDouble);
DL_FUNC_DECL(PyGILState_Ensure);
DL_FUNC_DECL(PyGILState_Release);
DL_FUNC_DECL(PyImport_Import);
DL_FUNC_DECL(PyImport_ImportModule);
DL_FUNC_DECL(PyIndex_Check);
DL_FUNC_DECL(PyInterpreterState_Main);
DL_FUNC_DECL(PyIter_Check);
DL_FUNC_DECL(PyIter_Next);
DL_FUNC_DECL(PyList_Append);
DL_FUNC_DECL(PyList_AsTuple);
DL_FUNC_DECL(PyList_GetItem);
DL_FUNC_DECL(PyList_New);
DL_FUNC_DECL(PyList_Size);
DL_FUNC_DECL(PyLong_AsLong);
DL_FUNC_DECL(PyLong_AsLongAndOverflow);
DL_FUNC_DECL(PyLong_AsLongLong);
DL_FUNC_DECL(PyLong_AsSsize_t);
DL_FUNC_DECL(PyLong_AsUnsignedLong);
DL_FUNC_DECL(PyLong_AsUnsignedLongLong);
DL_FUNC_DECL(PyLong_AsVoidPtr);
DL_FUNC_DECL(PyLong_FromDouble);
DL_FUNC_DECL(PyLong_FromLongLong);
DL_FUNC_DECL(PyLong_FromSsize_t);
DL_FUNC_DECL(PyLong_FromUnsignedLong);
DL_FUNC_DECL(PyLong_FromUnsignedLongLong);
DL_FUNC_DECL(PyLong_FromVoidPtr);
DL_FUNC_DECL(PyMapping_GetItemString);
DL_FUNC_DECL(PyMem_Calloc);
DL_FUNC_DECL(PyMem_Free);
DL_FUNC_DECL(PyMem_Malloc);
DL_FUNC_DECL(PyMem_RawFree);
DL_FUNC_DECL(PyMem_RawMalloc);
DL_FUNC_DECL(PyMem_RawRealloc);
DL_FUNC_DECL(PyMem_Realloc);
DL_FUNC_DECL(PyMemoryView_FromObject);
DL_FUNC_DECL(PyMethod_New);
DL_FUNC_DECL(PyModule_AddIntConstant);
DL_FUNC_DECL(PyModule_AddObject);
DL_FUNC_DECL(PyModule_AddStringConstant);
DL_FUNC_DECL(PyModule_GetDict);
DL_FUNC_DECL(PyNumber_Absolute);
DL_FUNC_DECL(PyNumber_Add);
DL_FUNC_DECL(PyNumber_And);
DL_FUNC_DECL(PyNumber_AsSsize_t);
DL_FUNC_DECL(PyNumber_Check);
DL_FUNC_DECL(PyNumber_Float);
DL_FUNC_DECL(PyNumber_FloorDivide);
DL_FUNC_DECL(PyNumber_Index);
DL_FUNC_DECL(PyNumber_Invert);
DL_FUNC_DECL(PyNumber_Long);
DL_FUNC_DECL(PyNumber_Lshift);
DL_FUNC_DECL(PyNumber_Multiply);
DL_FUNC_DECL(PyNumber_Negative);
DL_FUNC_DECL(PyNumber_Or);
DL_FUNC_DECL(PyNumber_Positive);
DL_FUNC_DECL(PyNumber_Power);
DL_FUNC_DECL(PyNumber_Remainder);
DL_FUNC_DECL(PyNumber_Rshift);
DL_FUNC_DECL(PyNumber_Subtract);
DL_FUNC_DECL(PyNumber_TrueDivide);
DL_FUNC_DECL(PyNumber_Xor);
DL_FUNC_DECL(PyOS_setsig);
DL_FUNC_DECL(PyOS_snprintf);
DL_FUNC_DECL(PyOS_string_to_double);
DL_FUNC_DECL(PyOS_strtol);
DL_FUNC_DECL(PyOS_strtoul);
DL_FUNC_DECL(PyObject_AsFileDescriptor);
DL_FUNC_DECL(PyObject_Bytes);
DL_FUNC_DECL(PyObject_Call);
DL_FUNC_DECL(PyObject_CallFunctionObjArgs);
DL_FUNC_DECL(PyObject_CallMethodObjArgs);
DL_FUNC_DECL(PyObject_CallObject);
DL_FUNC_DECL(PyObject_Calloc);
DL_FUNC_DECL(PyObject_CheckBuffer);
DL_FUNC_DECL(PyObject_ClearWeakRefs);
DL_FUNC_DECL(PyObject_Format);
DL_FUNC_DECL(PyObject_Free);
DL_FUNC_DECL(PyObject_GC_Del);
DL_FUNC_DECL(PyObject_GC_Track);
DL_FUNC_DECL(PyObject_GC_UnTrack);
DL_FUNC_DECL(PyObject_GenericGetAttr);
DL_FUNC_DECL(PyObject_GenericGetDict);
DL_FUNC_DECL(PyObject_GenericSetAttr);
DL_FUNC_DECL(PyObject_GetAttr);
DL_FUNC_DECL(PyObject_GetAttrString);
DL_FUNC_DECL(PyObject_GetBuffer);
DL_FUNC_DECL(PyObject_GetItem);
DL_FUNC_DECL(PyObject_GetIter);
DL_FUNC_DECL(PyObject_HasAttrString);
DL_FUNC_DECL(PyObject_Hash);
DL_FUNC_DECL(PyObject_Init);
DL_FUNC_DECL(PyObject_InitVar);
DL_FUNC_DECL(PyObject_IsInstance);
DL_FUNC_DECL(PyObject_IsSubclass);
DL_FUNC_DECL(PyObject_IsTrue);
DL_FUNC_DECL(PyObject_LengthHint);
DL_FUNC_DECL(PyObject_Malloc);
DL_FUNC_DECL(PyObject_Not);
DL_FUNC_DECL(PyObject_Print);
DL_FUNC_DECL(PyObject_Realloc);
DL_FUNC_DECL(PyObject_Repr);
DL_FUNC_DECL(PyObject_RichCompare);
DL_FUNC_DECL(PyObject_RichCompareBool);
DL_FUNC_DECL(PyObject_SelfIter);
DL_FUNC_DECL(PyObject_SetAttrString);
DL_FUNC_DECL(PyObject_SetItem);
DL_FUNC_DECL(PyObject_Size);
DL_FUNC_DECL(PyObject_Str);
DL_FUNC_DECL(PyObject_Type);
DL_FUNC_DECL(PyObject_Vectorcall);
DL_FUNC_DECL(PySeqIter_New);
DL_FUNC_DECL(PySequence_Check);
DL_FUNC_DECL(PySequence_Concat);
DL_FUNC_DECL(PySequence_Contains);
DL_FUNC_DECL(PySequence_Fast);
DL_FUNC_DECL(PySequence_GetItem);
DL_FUNC_DECL(PySequence_InPlaceConcat);
DL_FUNC_DECL(PySequence_InPlaceRepeat);
DL_FUNC_DECL(PySequence_Repeat);
DL_FUNC_DECL(PySequence_Size);
DL_FUNC_DECL(PySequence_Tuple);
DL_FUNC_DECL(PySlice_AdjustIndices);
DL_FUNC_DECL(PySlice_New);
DL_FUNC_DECL(PySlice_Unpack);
DL_FUNC_DECL(PyStructSequence_InitType2);
DL_FUNC_DECL(PyStructSequence_New);
DL_FUNC_DECL(PySys_GetObject);
DL_FUNC_DECL(PyThreadState_Get);
DL_FUNC_DECL(PyThreadState_GetDict);
DL_FUNC_DECL(PyTraceMalloc_Track);
DL_FUNC_DECL(PyTraceMalloc_Untrack);
DL_FUNC_DECL(PyTuple_GetItem);
DL_FUNC_DECL(PyTuple_GetSlice);
DL_FUNC_DECL(PyTuple_New);
DL_FUNC_DECL(PyTuple_Pack);
DL_FUNC_DECL(PyTuple_SetItem);
DL_FUNC_DECL(PyTuple_Size);
DL_FUNC_DECL(PyType_GenericNew);
DL_FUNC_DECL(PyType_GetFlags);
DL_FUNC_DECL(PyType_IsSubtype);
DL_FUNC_DECL(PyType_Ready);
DL_FUNC_DECL(PyUnicode_AsASCIIString);
DL_FUNC_DECL(PyUnicode_AsEncodedString);
DL_FUNC_DECL(PyUnicode_AsLatin1String);
DL_FUNC_DECL(PyUnicode_AsUCS4);
DL_FUNC_DECL(PyUnicode_AsUCS4Copy);
DL_FUNC_DECL(PyUnicode_AsUTF8);
DL_FUNC_DECL(PyUnicode_AsUTF8AndSize);
DL_FUNC_DECL(PyUnicode_AsUTF8String);
DL_FUNC_DECL(PyUnicode_Compare);
DL_FUNC_DECL(PyUnicode_CompareWithASCIIString);
DL_FUNC_DECL(PyUnicode_Concat);
DL_FUNC_DECL(PyUnicode_Format);
DL_FUNC_DECL(PyUnicode_FromEncodedObject);
DL_FUNC_DECL(PyUnicode_FromFormat);
DL_FUNC_DECL(PyUnicode_FromKindAndData);
DL_FUNC_DECL(PyUnicode_FromString);
DL_FUNC_DECL(PyUnicode_FromStringAndSize);
DL_FUNC_DECL(PyUnicode_GetLength);
DL_FUNC_DECL(PyUnicode_InternFromString);
DL_FUNC_DECL(PyUnicode_Replace);
DL_FUNC_DECL(PyUnicode_Substring);
DL_FUNC_DECL(PyUnicode_Tailmatch);
DL_FUNC_DECL(PyVectorcall_Call);
DL_FUNC_DECL(Py_BuildValue);
DL_FUNC_DECL(Py_EnterRecursiveCall);
DL_FUNC_DECL(Py_GenericAlias);
DL_FUNC_DECL(Py_IsInitialized);
DL_FUNC_DECL(Py_LeaveRecursiveCall);
DL_FUNC_DECL(_PyArg_ParseTupleAndKeywords_SizeT);
DL_FUNC_DECL(_PyArg_VaParseTupleAndKeywords_SizeT);
DL_FUNC_DECL(_PyDict_GetItemStringWithError);
DL_FUNC_DECL(_PyErr_BadInternalCall);
DL_FUNC_DECL(_PyObject_CallFunction_SizeT);
DL_FUNC_DECL(_PyObject_CallMethod_SizeT);
DL_FUNC_DECL(_PyObject_GC_New);
DL_FUNC_DECL(_PyObject_New);
DL_FUNC_DECL(_PyUnicode_IsWhitespace);
DL_FUNC_DECL(_Py_BuildValue_SizeT);
DL_FUNC_DECL(_Py_Dealloc);
DL_FUNC_DECL(_Py_HashDouble);

DL_DATA_DECL(PyBaseObject_Type);
DL_DATA_DECL(PyBool_Type);
DL_DATA_DECL(PyBytes_Type);
DL_DATA_DECL(PyCFunction_Type);
DL_DATA_DECL(PyCapsule_Type);
DL_DATA_DECL(PyComplex_Type);
DL_DATA_DECL(PyDictProxy_Type);
DL_DATA_DECL(PyDict_Type);
DL_DATA_DECL(PyExc_AttributeError);
DL_DATA_DECL(PyExc_BufferError);
DL_DATA_DECL(PyExc_DeprecationWarning);
DL_DATA_DECL(PyExc_Exception);
DL_DATA_DECL(PyExc_FloatingPointError);
DL_DATA_DECL(PyExc_FutureWarning);
DL_DATA_DECL(PyExc_IOError);
DL_DATA_DECL(PyExc_ImportError);
DL_DATA_DECL(PyExc_ImportWarning);
DL_DATA_DECL(PyExc_IndexError);
DL_DATA_DECL(PyExc_KeyError);
DL_DATA_DECL(PyExc_MemoryError);
DL_DATA_DECL(PyExc_NameError);
DL_DATA_DECL(PyExc_NotImplementedError);
DL_DATA_DECL(PyExc_OSError);
DL_DATA_DECL(PyExc_OverflowError);
DL_DATA_DECL(PyExc_RecursionError);
DL_DATA_DECL(PyExc_RuntimeError);
DL_DATA_DECL(PyExc_RuntimeWarning);
DL_DATA_DECL(PyExc_SystemError);
DL_DATA_DECL(PyExc_TypeError);
DL_DATA_DECL(PyExc_UnicodeDecodeError);
DL_DATA_DECL(PyExc_UserWarning);
DL_DATA_DECL(PyExc_ValueError);
DL_DATA_DECL(PyFloat_Type);
DL_DATA_DECL(PyFrozenSet_Type);
DL_DATA_DECL(PyGetSetDescr_Type);
DL_DATA_DECL(PyList_Type);
DL_DATA_DECL(PyLong_Type);
DL_DATA_DECL(PyMemberDescr_Type);
DL_DATA_DECL(PyMemoryView_Type);
DL_DATA_DECL(PyMethodDescr_Type);
DL_DATA_DECL(PySet_Type);
DL_DATA_DECL(PySlice_Type);
DL_DATA_DECL(PyTuple_Type);
DL_DATA_DECL(PyType_Type);
DL_DATA_DECL(PyUnicode_Type);
DL_DATA_DECL(_Py_EllipsisObject);
DL_DATA_DECL(_Py_FalseStruct);
DL_DATA_DECL(_Py_NoneStruct);
DL_DATA_DECL(_Py_NotImplementedStruct);
DL_DATA_DECL(_Py_TrueStruct);
DL_DATA_DECL(_Py_ascii_whitespace);

// -----------------------------------------------------------------------
// Curated export table -- symbols a loaded module is allowed to bind
// against for anything it doesn't define itself. Add entries as modules
// need more of the host surface; a module referencing something not
// listed here fails dlopen() with that symbol's name in dlerror().
//
// Function vs. data exports are NOT interchangeable here. A function
// export's table entry is just the function's own address -- a call
// through the GOT loads that address and jumps straight to it. A *data*
// symbol (e.g. a Python C-extension module doing `extern PyObject
// *PyExc_ValueError` and reading it) instead needs the table entry to be
// the address *of* the variable -- (void *)&PyExc_ValueError, not
// (void *)PyExc_ValueError -- since compiled PIC code dereferences the
// GOT slot once to get the variable's address, then dereferences again
// to read its current value. Every entry below is a function; there are
// no data exports yet.
// -----------------------------------------------------------------------

struct dl_export {
	const char *name;
	void *addr;
};

static const struct dl_export dl_exports[] = {
	{ "printf",  (void *)printf },
	{ "malloc",  (void *)malloc },
	{ "free",    (void *)free },
	{ "realloc", (void *)realloc },
	{ "calloc",  (void *)calloc },
	{ "memcpy",  (void *)memcpy },
	{ "memset",  (void *)memset },
	{ "memmove", (void *)memmove },
	{ "memcmp",  (void *)memcmp },
	{ "strlen",  (void *)strlen },
	{ "strcmp",  (void *)strcmp },
	{ "strncmp", (void *)strncmp },
	{ "strcpy",  (void *)strcpy },
	{ "strcat",  (void *)strcat },
	{ "b_output", (void *)b_output },
	// CPython C-API entries -- enough for a hand-written extension
	// module (see ../pyexttest.c) to prove Python/dynload_shlib.c's
	// dlopen()/dlsym() path end-to-end. All three are already real
	// symbols in the linked python.app binary (Python/getargs.c,
	// Python/modsupport.c, Objects/longobject.c are all in setup.sh's
	// PYTHON_SRCS). Extend this as more extensions need more of the
	// C API -- see this table's header comment for data vs. function
	// exports before adding a PyExc_*/PyType_Type-style symbol.
	{ "PyArg_ParseTuple", (void *)PyArg_ParseTuple },
	{ "_PyArg_ParseTuple_SizeT", (void *)_PyArg_ParseTuple_SizeT },
	{ "PyModule_Create2", (void *)PyModule_Create2 },
	{ "PyLong_FromLong",  (void *)PyLong_FromLong },

// F()/D() just spell out { "NAME", (void*)NAME } / { "NAME", (void*)&NAME }
// -- shorthand for the sheer volume of the numpy-driven block below (see
// its own declarations' comment above for where this list came from).
// #undef'd right after the table closes; nothing outside this file uses
// them.
#define F(name) { #name, (void *)name },
#define D(name) { #name, (void *)&name },

	// libc/libm -- numpy (see ../NUMPY.md Phase 1)
	F(__errno_location) F(acos) F(acosf) F(acosh) F(acoshf) F(acoshl) F(acosl)
	F(asin) F(asinf) F(asinh) F(asinhf) F(asinhl) F(asinl) F(atan) F(atan2)
	F(atan2f) F(atan2l) F(atanf) F(atanh) F(atanhf) F(atanhl) F(atanl)
	F(cabs) F(cabsf) F(cabsl) F(cbrt) F(cbrtf) F(cbrtl) F(ccos) F(ccosf)
	F(ccosh) F(ccoshf) F(ccoshl) F(ccosl) F(ceil) F(ceilf) F(ceill) F(cexp)
	F(cexpf) F(cexpl) F(clog) F(clogf) F(clogl) F(cos) F(cosf) F(cosh)
	F(coshf) F(coshl) F(cosl) F(cpow) F(cpowf) F(cpowl) F(csin) F(csinf)
	F(csinh) F(csinhf) F(csinhl) F(csinl) F(csqrt) F(csqrtf) F(csqrtl)
	F(ctan) F(ctanf) F(ctanh) F(ctanhf) F(ctanhl) F(ctanl) F(exp) F(exp2)
	F(exp2f) F(exp2l) F(expf) F(expl) F(expm1) F(expm1f) F(expm1l) F(fabs)
	F(fabsf) F(fabsl) F(fclose) F(fdopen) F(feclearexcept) F(feraiseexcept)
	F(fetestexcept) F(fflush) F(fgetc) F(fileno) F(floor) F(floorf) F(floorl)
	F(fmax) F(fmaxf) F(fmaxl) F(fmin) F(fminf) F(fminl) F(fmod) F(fmodf)
	F(fmodl) F(fprintf) F(fread) F(freelocale) F(frexp) F(frexpf) F(frexpl)
	F(fscanf) F(fseeko) F(ftello) F(fwrite) F(getc) F(getenv) F(hypot)
	F(hypotf) F(hypotl) F(ldexp) F(ldexpf) F(ldexpl) F(localeconv)
	F(localtime_r) F(log) F(log10) F(log10f) F(log10l) F(log1p) F(log1pf)
	F(log1pl) F(log2) F(log2f) F(log2l) F(logf) F(logl) F(lseek) F(madvise)
	F(memchr) F(modf) F(modff) F(modfl) F(newlocale) F(nextafter)
	F(nextafterf) F(nextafterl) F(pow) F(powf) F(powl) F(putchar) F(puts)
	F(qsort) F(rint) F(rintf) F(rintl) F(siglongjmp) F(sin) F(sinf) F(sinh)
	F(sinhf) F(sinhl) F(sinl) F(snprintf) F(sqrt) F(sqrtf) F(sqrtl)
	F(strerror) F(strncat) F(strrchr) F(strtok) F(strtol) F(strtold_l)
	F(strtoll) F(strtoull) F(tan) F(tanf) F(tanh) F(tanhf) F(tanhl) F(tanl)
	F(time) F(trunc) F(truncf) F(truncl) F(ungetc)

	// CPython C-API functions -- numpy (see ../NUMPY.md Phase 1)
	F(PyArg_ParseTupleAndKeywords) F(PyArg_UnpackTuple) F(PyBool_FromLong)
	F(PyBuffer_Release) F(PyBytes_AsString) F(PyBytes_AsStringAndSize)
	F(PyBytes_FromString) F(PyBytes_FromStringAndSize) F(PyBytes_Size)
	F(PyCallable_Check) F(PyCapsule_GetContext) F(PyCapsule_GetPointer)
	F(PyCapsule_Import) F(PyCapsule_IsValid) F(PyCapsule_New)
	F(PyCapsule_SetContext) F(PyCapsule_SetName) F(PyComplex_AsCComplex)
	F(PyComplex_FromCComplex) F(PyComplex_FromDoubles)
	F(PyComplex_ImagAsDouble) F(PyComplex_RealAsDouble) F(PyContextVar_Get)
	F(PyContextVar_New) F(PyContextVar_Set) F(PyDictProxy_New)
	F(PyDict_Contains) F(PyDict_Copy) F(PyDict_DelItem)
	F(PyDict_DelItemString) F(PyDict_GetItem) F(PyDict_GetItemString)
	F(PyDict_GetItemWithError) F(PyDict_Merge) F(PyDict_New) F(PyDict_Next)
	F(PyDict_SetItem) F(PyDict_SetItemString) F(PyDict_Size)
	F(PyErr_CheckSignals) F(PyErr_Clear) F(PyErr_ExceptionMatches)
	F(PyErr_Fetch) F(PyErr_Format) F(PyErr_GivenExceptionMatches)
	F(PyErr_NoMemory) F(PyErr_NormalizeException) F(PyErr_Occurred)
	F(PyErr_Print) F(PyErr_Restore) F(PyErr_SetFromErrno) F(PyErr_SetNone)
	F(PyErr_SetObject) F(PyErr_SetString) F(PyErr_WarnEx)
	F(PyErr_WarnFormat) F(PyErr_WriteUnraisable) F(PyEval_GetBuiltins)
	F(PyEval_RestoreThread) F(PyEval_SaveThread) F(PyException_SetCause)
	F(PyException_SetContext) F(PyException_SetTraceback)
	F(PyFloat_AsDouble) F(PyFloat_FromDouble) F(PyGILState_Ensure)
	F(PyGILState_Release) F(PyImport_Import) F(PyImport_ImportModule)
	F(PyIndex_Check) F(PyInterpreterState_Main) F(PyIter_Check)
	F(PyIter_Next) F(PyList_Append) F(PyList_AsTuple) F(PyList_GetItem)
	F(PyList_New) F(PyList_Size) F(PyLong_AsLong)
	F(PyLong_AsLongAndOverflow) F(PyLong_AsLongLong) F(PyLong_AsSsize_t)
	F(PyLong_AsUnsignedLong) F(PyLong_AsUnsignedLongLong)
	F(PyLong_AsVoidPtr) F(PyLong_FromDouble) F(PyLong_FromLongLong)
	F(PyLong_FromSsize_t) F(PyLong_FromUnsignedLong)
	F(PyLong_FromUnsignedLongLong) F(PyLong_FromVoidPtr)
	F(PyMapping_GetItemString) F(PyMem_Calloc) F(PyMem_Free)
	F(PyMem_Malloc) F(PyMem_RawFree) F(PyMem_RawMalloc) F(PyMem_RawRealloc)
	F(PyMem_Realloc) F(PyMemoryView_FromObject) F(PyMethod_New)
	F(PyModule_AddIntConstant) F(PyModule_AddObject)
	F(PyModule_AddStringConstant) F(PyModule_GetDict) F(PyNumber_Absolute)
	F(PyNumber_Add) F(PyNumber_And) F(PyNumber_AsSsize_t) F(PyNumber_Check)
	F(PyNumber_Float) F(PyNumber_FloorDivide) F(PyNumber_Index)
	F(PyNumber_Invert) F(PyNumber_Long) F(PyNumber_Lshift)
	F(PyNumber_Multiply) F(PyNumber_Negative) F(PyNumber_Or)
	F(PyNumber_Positive) F(PyNumber_Power) F(PyNumber_Remainder)
	F(PyNumber_Rshift) F(PyNumber_Subtract) F(PyNumber_TrueDivide)
	F(PyNumber_Xor) F(PyOS_setsig) F(PyOS_snprintf)
	F(PyOS_string_to_double) F(PyOS_strtol) F(PyOS_strtoul)
	F(PyObject_AsFileDescriptor) F(PyObject_Bytes) F(PyObject_Call)
	F(PyObject_CallFunctionObjArgs) F(PyObject_CallMethodObjArgs)
	F(PyObject_CallObject) F(PyObject_Calloc) F(PyObject_CheckBuffer)
	F(PyObject_ClearWeakRefs) F(PyObject_Format) F(PyObject_Free)
	F(PyObject_GC_Del) F(PyObject_GC_Track) F(PyObject_GC_UnTrack)
	F(PyObject_GenericGetAttr) F(PyObject_GenericGetDict)
	F(PyObject_GenericSetAttr) F(PyObject_GetAttr) F(PyObject_GetAttrString)
	F(PyObject_GetBuffer) F(PyObject_GetItem) F(PyObject_GetIter)
	F(PyObject_HasAttrString) F(PyObject_Hash) F(PyObject_Init)
	F(PyObject_InitVar) F(PyObject_IsInstance) F(PyObject_IsSubclass)
	F(PyObject_IsTrue) F(PyObject_LengthHint) F(PyObject_Malloc)
	F(PyObject_Not) F(PyObject_Print) F(PyObject_Realloc) F(PyObject_Repr)
	F(PyObject_RichCompare) F(PyObject_RichCompareBool) F(PyObject_SelfIter)
	F(PyObject_SetAttrString) F(PyObject_SetItem) F(PyObject_Size)
	F(PyObject_Str) F(PyObject_Type) F(PyObject_Vectorcall) F(PySeqIter_New)
	F(PySequence_Check) F(PySequence_Concat) F(PySequence_Contains)
	F(PySequence_Fast) F(PySequence_GetItem) F(PySequence_InPlaceConcat)
	F(PySequence_InPlaceRepeat) F(PySequence_Repeat) F(PySequence_Size)
	F(PySequence_Tuple) F(PySlice_AdjustIndices) F(PySlice_New)
	F(PySlice_Unpack) F(PyStructSequence_InitType2) F(PyStructSequence_New)
	F(PySys_GetObject) F(PyThreadState_Get) F(PyThreadState_GetDict)
	F(PyTraceMalloc_Track) F(PyTraceMalloc_Untrack) F(PyTuple_GetItem)
	F(PyTuple_GetSlice) F(PyTuple_New) F(PyTuple_Pack) F(PyTuple_SetItem)
	F(PyTuple_Size) F(PyType_GenericNew) F(PyType_GetFlags)
	F(PyType_IsSubtype) F(PyType_Ready) F(PyUnicode_AsASCIIString)
	F(PyUnicode_AsEncodedString) F(PyUnicode_AsLatin1String)
	F(PyUnicode_AsUCS4) F(PyUnicode_AsUCS4Copy) F(PyUnicode_AsUTF8)
	F(PyUnicode_AsUTF8AndSize) F(PyUnicode_AsUTF8String)
	F(PyUnicode_Compare) F(PyUnicode_CompareWithASCIIString)
	F(PyUnicode_Concat) F(PyUnicode_Format) F(PyUnicode_FromEncodedObject)
	F(PyUnicode_FromFormat) F(PyUnicode_FromKindAndData)
	F(PyUnicode_FromString) F(PyUnicode_FromStringAndSize)
	F(PyUnicode_GetLength) F(PyUnicode_InternFromString)
	F(PyUnicode_Replace) F(PyUnicode_Substring) F(PyUnicode_Tailmatch)
	F(PyVectorcall_Call) F(Py_BuildValue) F(Py_EnterRecursiveCall)
	F(Py_GenericAlias) F(Py_IsInitialized) F(Py_LeaveRecursiveCall)
	F(_PyArg_ParseTupleAndKeywords_SizeT)
	F(_PyArg_VaParseTupleAndKeywords_SizeT)
	F(_PyDict_GetItemStringWithError) F(_PyErr_BadInternalCall)
	F(_PyObject_CallFunction_SizeT) F(_PyObject_CallMethod_SizeT)
	F(_PyObject_GC_New) F(_PyObject_New) F(_PyUnicode_IsWhitespace)
	F(_Py_BuildValue_SizeT) F(_Py_Dealloc) F(_Py_HashDouble)

	// CPython C-API data -- numpy (see ../NUMPY.md Phase 1). Address-of,
	// not a plain function-pointer entry -- see this table's header
	// comment for why.
	D(PyBaseObject_Type) D(PyBool_Type) D(PyBytes_Type) D(PyCFunction_Type)
	D(PyCapsule_Type) D(PyComplex_Type) D(PyDictProxy_Type) D(PyDict_Type)
	D(PyExc_AttributeError) D(PyExc_BufferError) D(PyExc_DeprecationWarning)
	D(PyExc_Exception) D(PyExc_FloatingPointError) D(PyExc_FutureWarning)
	D(PyExc_IOError) D(PyExc_ImportError) D(PyExc_ImportWarning)
	D(PyExc_IndexError) D(PyExc_KeyError) D(PyExc_MemoryError)
	D(PyExc_NameError) D(PyExc_NotImplementedError) D(PyExc_OSError)
	D(PyExc_OverflowError) D(PyExc_RecursionError) D(PyExc_RuntimeError)
	D(PyExc_RuntimeWarning) D(PyExc_SystemError) D(PyExc_TypeError)
	D(PyExc_UnicodeDecodeError) D(PyExc_UserWarning) D(PyExc_ValueError)
	D(PyFloat_Type) D(PyFrozenSet_Type) D(PyGetSetDescr_Type)
	D(PyList_Type) D(PyLong_Type) D(PyMemberDescr_Type)
	D(PyMemoryView_Type) D(PyMethodDescr_Type) D(PySet_Type)
	D(PySlice_Type) D(PyTuple_Type) D(PyType_Type) D(PyUnicode_Type)
	D(_Py_EllipsisObject) D(_Py_FalseStruct) D(_Py_NoneStruct)
	D(_Py_NotImplementedStruct) D(_Py_TrueStruct) D(_Py_ascii_whitespace)

#undef F
#undef D
};

static void *dl_export_lookup(const char *name)
{
	for (size_t i = 0; i < sizeof(dl_exports) / sizeof(dl_exports[0]); i++) {
		if (strcmp(dl_exports[i].name, name) == 0)
			return dl_exports[i].addr;
	}
	return 0;
}

// -----------------------------------------------------------------------
// dlerror() state -- a single static buffer is enough here: apps run
// single-threaded per the rest of this port (see thread_shim.c), so
// there's no concurrent-dlopen() case to race on it.
// -----------------------------------------------------------------------

static char dl_error_buf[256];
static int dl_error_pending = 0;

// Every dlopen()/dlsym() failure path returns this, so callers can
// write `return dl_fail(...)` directly instead of setting the error and
// returning 0 as two separate statements.
static void *dl_fail(const char *msg)
{
	size_t n = strlen(msg);
	if (n >= sizeof(dl_error_buf))
		n = sizeof(dl_error_buf) - 1;
	memcpy(dl_error_buf, msg, n);
	dl_error_buf[n] = 0;
	dl_error_pending = 1;
	return 0;
}

// Same, for the common "<prefix>: <name>" case (missing file, missing
// symbol) -- avoids pulling in printf's format machinery for what's
// always just two strings joined together.
static void *dl_failf(const char *prefix, const char *name)
{
	size_t pn = strlen(prefix);
	if (pn > sizeof(dl_error_buf) - 3)
		pn = sizeof(dl_error_buf) - 3;
	size_t room = sizeof(dl_error_buf) - 1 - pn - 2;
	size_t nn = strlen(name);
	if (nn > room)
		nn = room;
	memcpy(dl_error_buf, prefix, pn);
	dl_error_buf[pn] = ':';
	dl_error_buf[pn + 1] = ' ';
	memcpy(dl_error_buf + pn + 2, name, nn);
	dl_error_buf[pn + 2 + nn] = 0;
	dl_error_pending = 1;
	return 0;
}

char *dlerror(void)
{
	if (!dl_error_pending)
		return 0;
	dl_error_pending = 0;
	return dl_error_buf;
}

// -----------------------------------------------------------------------
// Loaded-module handle
// -----------------------------------------------------------------------

struct dl_handle {
	char *block;			// malloc()'d backing store -- what dlclose() frees
	char *bias;			// block, rebased so bias+p_vaddr always lands inside it
	const Elf64_Sym *dynsym;
	const char *dynstr;
	size_t dynsym_count;		// from DT_HASH's nchain -- see dlopen() below
};

// Resolves relocs in one Elf64_Rela table (.rela.dyn or .rela.plt --
// same entry format, processed the same way). Every symbolic reference
// is resolved eagerly here rather than lazily through a PLT trampoline
// -- there's no runtime resolver to jump to on first call the way a
// real ld.so provides, so build-module.sh compiles modules -fno-plt to
// route external calls through the GOT directly, and everything is
// bound up front (equivalent to a real loader's -z now/BIND_NOW path).
static int apply_relocs(char *bias, const Elf64_Rela *relocs, size_t bytes,
			 const Elf64_Sym *dynsym, const char *dynstr)
{
	size_t count = bytes / sizeof(Elf64_Rela);

	for (size_t i = 0; i < count; i++) {
		const Elf64_Rela *r = &relocs[i];
		u64 type = ELF64_R_TYPE(r->r_info);
		u64 symidx = ELF64_R_SYM(r->r_info);
		u64 *target = (u64 *)(bias + r->r_offset);

		if (type == R_X86_64_RELATIVE) {
			*target = (u64)(bias + r->r_addend);
			continue;
		}

		if (type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT ||
		    type == R_X86_64_64) {
			const Elf64_Sym *sym = &dynsym[symidx];
			const char *name = dynstr + sym->st_name;
			void *resolved = (sym->st_shndx != SHN_UNDEF)
				? (void *)(bias + sym->st_value)
				: dl_export_lookup(name);
			if (!resolved) {
				dl_failf("dlopen: undefined symbol", name);
				return -1;
			}
			*target = (type == R_X86_64_64)
				? (u64)resolved + r->r_addend
				: (u64)resolved;
			continue;
		}

		dl_fail("dlopen: unsupported relocation type in module");
		return -1;
	}

	return 0;
}

void *dlopen(const char *path, int flags)
{
	(void)flags; // relocations are always processed eagerly -- see apply_relocs()

	if (!path)
		return dl_fail("dlopen: NULL path (dlopen(NULL, ...) -- handle to the host program -- is not supported here)");

	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return dl_failf("dlopen: open failed", path);

	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size <= 0) {
		close(fd);
		return dl_failf("dlopen: fstat failed", path);
	}

	size_t fsize = (size_t)st.st_size;
	char *filebuf = malloc(fsize);
	if (!filebuf) {
		close(fd);
		return dl_fail("dlopen: out of memory reading module file");
	}

	size_t got = 0;
	while (got < fsize) {
		long n = read(fd, filebuf + got, fsize - got);
		if (n <= 0) {
			close(fd);
			free(filebuf);
			return dl_failf("dlopen: short read on", path);
		}
		got += (size_t)n;
	}
	close(fd);

	if (fsize < sizeof(Elf64_Ehdr)) {
		free(filebuf);
		return dl_fail("dlopen: file too small to be an ELF64 module");
	}

	Elf64_Ehdr *eh = (Elf64_Ehdr *)filebuf;
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' ||
	    eh->e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh->e_ident[EI_DATA] != ELFDATA2LSB) {
		free(filebuf);
		return dl_fail("dlopen: not a little-endian ELF64 file");
	}
	if (eh->e_type != ET_DYN || eh->e_machine != EM_X86_64) {
		free(filebuf);
		return dl_fail("dlopen: not an x86-64 ET_DYN shared object (build it with build-module.sh)");
	}

	Elf64_Phdr *phdrs = (Elf64_Phdr *)(filebuf + eh->e_phoff);
	int64_t min_vaddr = -1;
	int64_t max_vaddr = 0;
	Elf64_Phdr *dyn_phdr = 0;

	for (int i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = &phdrs[i];
		if (ph->p_type == PT_LOAD) {
			if (min_vaddr < 0 || (int64_t)ph->p_vaddr < min_vaddr)
				min_vaddr = (int64_t)ph->p_vaddr;
			int64_t end = (int64_t)(ph->p_vaddr + ph->p_memsz);
			if (end > max_vaddr)
				max_vaddr = end;
		} else if (ph->p_type == PT_DYNAMIC) {
			dyn_phdr = ph;
		}
	}
	if (min_vaddr < 0) {
		free(filebuf);
		return dl_fail("dlopen: module has no PT_LOAD segments");
	}
	if (!dyn_phdr) {
		free(filebuf);
		return dl_fail("dlopen: module has no PT_DYNAMIC segment");
	}

	size_t span = (size_t)(max_vaddr - min_vaddr);
	char *block = malloc(span);
	if (!block) {
		free(filebuf);
		return dl_fail("dlopen: out of memory loading module");
	}
	char *bias = block - min_vaddr;

	for (int i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = &phdrs[i];
		if (ph->p_type != PT_LOAD)
			continue;
		char *dst = bias + ph->p_vaddr;
		memcpy(dst, filebuf + ph->p_offset, ph->p_filesz);
		memset(dst + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
	}

	Elf64_Dyn *dyn = (Elf64_Dyn *)(bias + dyn_phdr->p_vaddr);
	const Elf64_Sym *dynsym = 0;
	const char *dynstr = 0;
	const Elf64_Rela *rela = 0; size_t relasz = 0;
	const Elf64_Rela *jmprel = 0; size_t pltrelsz = 0;
	const u64 *init_array = 0; size_t init_arraysz = 0;
	size_t dynsym_count = 0;

	for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
		switch (d->d_tag) {
		case DT_SYMTAB:      dynsym = (const Elf64_Sym *)(bias + d->d_un.d_ptr); break;
		case DT_STRTAB:      dynstr = (const char *)(bias + d->d_un.d_ptr); break;
		case DT_RELA:        rela = (const Elf64_Rela *)(bias + d->d_un.d_ptr); break;
		case DT_RELASZ:      relasz = (size_t)d->d_un.d_val; break;
		case DT_JMPREL:      jmprel = (const Elf64_Rela *)(bias + d->d_un.d_ptr); break;
		case DT_PLTRELSZ:    pltrelsz = (size_t)d->d_un.d_val; break;
		case DT_INIT_ARRAY:  init_array = (const u64 *)(bias + d->d_un.d_ptr); break;
		case DT_INIT_ARRAYSZ: init_arraysz = (size_t)d->d_un.d_val; break;
		case DT_HASH: {
			// hash[0] = nbucket, hash[1] = nchain; the ELF spec
			// guarantees nchain == the symbol table's entry
			// count. build-module.sh passes --hash-style=both
			// so this section always exists to read it from,
			// even though nothing here uses the hash itself.
			const Elf64_Word *hash = (const Elf64_Word *)(bias + d->d_un.d_ptr);
			dynsym_count = hash[1];
			break;
		}
		default: break;
		}
	}

	if (!dynsym || !dynstr || !dynsym_count) {
		free(block);
		free(filebuf);
		return dl_fail("dlopen: module missing .dynsym/.dynstr/.hash");
	}

	if (rela && relasz && apply_relocs(bias, rela, relasz, dynsym, dynstr) != 0) {
		free(block);
		free(filebuf);
		return 0; // apply_relocs() already set dlerror()
	}
	if (jmprel && pltrelsz && apply_relocs(bias, jmprel, pltrelsz, dynsym, dynstr) != 0) {
		free(block);
		free(filebuf);
		return 0;
	}

	if (init_array) {
		size_t n = init_arraysz / sizeof(u64);
		for (size_t i = 0; i < n; i++) {
			void (*ctor)(void) = (void (*)(void))init_array[i];
			if (ctor)
				ctor();
		}
	}

	free(filebuf); // module now lives entirely in `block`

	struct dl_handle *h = malloc(sizeof(struct dl_handle));
	if (!h) {
		free(block);
		return dl_fail("dlopen: out of memory allocating handle");
	}
	h->block = block;
	h->bias = bias;
	h->dynsym = dynsym;
	h->dynstr = dynstr;
	h->dynsym_count = dynsym_count;

	return h;
}

void *dlsym(void *handle, const char *name)
{
	struct dl_handle *h = handle;
	if (!h)
		return dl_fail("dlsym: NULL handle");

	for (size_t i = 0; i < h->dynsym_count; i++) {
		const Elf64_Sym *sym = &h->dynsym[i];
		if (sym->st_shndx == SHN_UNDEF || !sym->st_name)
			continue;
		if (strcmp(h->dynstr + sym->st_name, name) == 0)
			return h->bias + sym->st_value;
	}

	return dl_failf("dlsym: symbol not found", name);
}

int dlclose(void *handle)
{
	struct dl_handle *h = handle;
	if (!h)
		return 0;
	free(h->block);
	free(h);
	return 0;
}
