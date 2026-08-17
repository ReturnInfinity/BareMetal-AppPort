// A minimal real C extension module -- proves Python/dynload_shlib.c's
// dlopen()/dlsym() path (port/dlfcn_shim.c) end-to-end: a genuine
// PyInit_pyexttest, found and run by the embedded interpreter's normal
// `import pyexttest`, not anything baked into the static/frozen module
// table. Uses only the public Python.h API -- no Py_BUILD_CORE, no
// Include/internal -- same as any real extension author would write.
//
// Build with:
//   ./build-module.sh -I build/Python-3.14.7/Include -I port/python_port \
//       -isystem "$(gcc -print-file-name=include)" \
//       pyexttest.c -o build/pyexttest.so
// (port/python_port is needed too -- Python.h's #include "pyconfig.h" is
// this port's own, not one CPython ships in Include/) then install at
// /pylib/pyexttest.so (scripts/install-file.sh) so it's on sys.path.

#define PY_SSIZE_T_CLEAN
#include <Python.h>

static PyObject *py_add(PyObject *self, PyObject *args)
{
	long a, b;
	if (!PyArg_ParseTuple(args, "ll", &a, &b))
		return NULL;
	return PyLong_FromLong(a + b);
}

static PyMethodDef methods[] = {
	{ "add", py_add, METH_VARARGS, "Add two integers." },
	{ NULL, NULL, 0, NULL },
};

static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT, "pyexttest", NULL, -1, methods,
};

PyMODINIT_FUNC PyInit_pyexttest(void)
{
	return PyModule_Create(&moduledef);
}
