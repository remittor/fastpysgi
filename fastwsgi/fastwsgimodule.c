#include <Python.h>
#include "server.h"

static PyMethodDef FastPySgiFunctions[] = {
    { "get_version", (PyCFunction)get_version, METH_NOARGS, NULL },
    { "init_server", init_server, METH_O, "" },
    { "change_setting", change_setting, METH_VARARGS, NULL },
    { "run_server", run_server, METH_O, NULL },
    { "run_nowait", run_nowait, METH_O, NULL },
    { "close_server", close_server, METH_O, NULL },
    { NULL }
};

static struct PyModuleDef module = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "fastpysgi",
    .m_doc  = "FastPySGI Python module",
    .m_size = -1,
    .m_methods = FastPySgiFunctions,
    .m_slots = NULL,
    .m_traverse = NULL,
    .m_clear = NULL,
    .m_free = NULL,
};

PyMODINIT_FUNC PyInit__fastpysgi(void)
{
    return PyModule_Create(&module);
}
