#include <stdbool.h>
#include "start_response.h"

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 9
#include <traceback.h>
#endif

static
bool is_valid_status(PyObject * status)
{
    if (PyUnicode_Check(status))
        if (PyUnicode_GET_LENGTH(status) >= 3)
            return true;

    LOGc("start_response: argument 1 (status) must be a 3-digit string");
    return false;
}

static
bool is_valid_header_tuple(PyObject * tuple)
{
    if (PyTuple_Check(tuple))
        if (PyTuple_GET_SIZE(tuple) == 2)
            if (PyUnicode_Check(PyTuple_GET_ITEM(tuple, 0)))
                if (PyUnicode_Check(PyTuple_GET_ITEM(tuple, 1)))
                    return true;

    LOGc("start_response: argument 2 (headers) expects a list of 2-tuples (str, str)");
    return false;
}

static
bool is_valid_headers(PyObject * headers)
{
    if (PyList_Check(headers)) {
        Py_ssize_t len = PyList_GET_SIZE(headers);
        for (Py_ssize_t i = 0; i < len; i++) {
            if (!is_valid_header_tuple(PyList_GET_ITEM(headers, i)))
                return false;
        }
        return true;
    }
    LOGc("start_response: argument 2 (headers) expects a list of 2-tuples, got '%s' instead.", Py_TYPE(headers)->tp_name);
    return false;
}

static
void log_exc_info(PyObject * exc_info)
{
    if (exc_info && PyTuple_Check(exc_info)) {
        PyObject * type_str  = PyObject_Str(PyTuple_GET_ITEM(exc_info, 0));
        PyObject * value_str = PyObject_Str(PyTuple_GET_ITEM(exc_info, 1));
        LOGe("start_response: Exception: %s: %s", type_str ? PyUnicode_AsUTF8(type_str) : "<unknown>", value_str ? PyUnicode_AsUTF8(value_str) : "<unknown>");
        Py_XDECREF(type_str);
        Py_XDECREF(value_str);
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 9
        PyObject * tb_obj = PyTuple_GET_ITEM(exc_info, 2);
        if (tb_obj && PyTraceBack_Check(tb_obj)) {
            PyTracebackObject * tb = (PyTracebackObject *)tb_obj;
            PyFrameObject * frame = tb->tb_frame;
            if (frame) {
                int lineno = PyFrame_GetLineNumber(frame);
                PyCodeObject * code = PyFrame_GetCode(frame);
                if (code) {
                    PyObject * co_filename = PyObject_GetAttrString((PyObject *)code, "co_filename");
                    PyObject * co_name     = PyObject_GetAttrString((PyObject *)code, "co_name");
                    const char * filename  = co_filename ? PyUnicode_AsUTF8(co_filename) : NULL;
                    const char * funcname  = co_name     ? PyUnicode_AsUTF8(co_name)     : NULL;
                    LOGe("start_response: Exception at %s:%d in %s()", filename ? filename : "<unknown>", lineno, funcname ? funcname : "<unknown>");
                    Py_XDECREF(co_filename);
                    Py_XDECREF(co_name);
                }
                Py_XDECREF(code);
            }
        }
#endif
    }
}

static
bool is_valid_exc_info(StartResponse * sr)
{
    if (sr->exc_info && sr->exc_info != Py_None) {
        if (PyTuple_Check(sr->exc_info)) {
            if (PyTuple_GET_SIZE(sr->exc_info) == 3) {
                log_exc_info(sr->exc_info);
                return true;
            }
        }
        LOGc("start_response: argument 3 (exc_info) expects a 3-tuple, got '%s' instead.", Py_TYPE(sr->exc_info)->tp_name);
        return false;
    }
    if (sr->called == 1) {
        LOGc("start_response: argument 3 (exc_info) is required in the second call of start_response");
        return false;
    }
    return true;
}

static
PyObject * start_response_call(StartResponse * sr, PyObject * args, PyObject * kwargs)
{
    if (sr->called == 1) {
        Py_CLEAR(sr->status);
        Py_CLEAR(sr->headers);
    }
    sr->exc_info = NULL;

    int rc = PyArg_UnpackTuple(args, "start_response", 2, 3, &sr->status, &sr->headers, &sr->exc_info);
    if (rc == (int)false)
        return NULL;

    if (!is_valid_status(sr->status))
        return NULL;

    if (!is_valid_headers(sr->headers))
        return NULL;

    if (!is_valid_exc_info(sr))
        return NULL;

    sr->called = 1;

    Py_XINCREF(sr->status);
    Py_XINCREF(sr->headers);
    Py_XINCREF(sr->exc_info);

    Py_RETURN_NONE;
}

static
void start_response_dealloc(StartResponse * self)
{
    Py_CLEAR(self->status);
    Py_CLEAR(self->headers);
    Py_CLEAR(self->exc_info);
    PyObject_Del(self);
}

PyTypeObject StartResponse_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "start_response",
    .tp_basicsize = sizeof(StartResponse),
    .tp_itemsize  = 0,
    .tp_dealloc   = (destructor) start_response_dealloc,
    .tp_call      = (ternaryfunc) start_response_call,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_finalize  = NULL
};
