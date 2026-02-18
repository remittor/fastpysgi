#include <Python.h>
#include "server.h"

static PyMethodDef FastPySgiFunctions[] = {
    { "init_server", init_server, METH_O, "" },
    { "change_setting", change_setting, METH_VARARGS, "" },
    { "run_server", run_server, METH_O, "" },
    { "run_nowait", run_nowait, METH_O, "" },
    { "close_server", close_server, METH_O, "" },
    { NULL, NULL, 0, NULL}
};

static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "fastpysgi",
    "fastpysgi Python module",
    -1,
    FastPySgiFunctions,
};

PyMODINIT_FUNC PyInit__fastpysgi(void)
{
    return PyModule_Create(&module);
}
