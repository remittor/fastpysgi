#include "common.h"
#include "llhttp.h"
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

void logrepr(int level, PyObject* obj)
{
    PyObject* repr = PyObject_Repr(obj);
    PyObject* str = PyUnicode_AsEncodedString(repr, "utf-8", "~E~");
    const char* bytes = PyBytes_AS_STRING(str);
    LOGX(level, "REPR: %s", bytes);
    Py_XDECREF(repr);
    Py_XDECREF(str);
}

const char * get_http_status_name(int status)
{
#define HTTP_STATUS_GEN(NUM, NAME, STRING) case HTTP_STATUS_##NAME: return #STRING;
    switch (status) {
        HTTP_STATUS_MAP(HTTP_STATUS_GEN)
    default: return NULL;
    }
#undef HTTP_STATUS_GEN
    return NULL;
}

int64_t get_env_int(const char * name)
{
    int64_t v;
    char buf[128];
    size_t len = sizeof(buf) - 1;
    int rv = uv_os_getenv(name, buf, &len);
    if (rv == UV_ENOENT)
        return -1;  // env not found
    if (rv != 0 || len == 0)
        return -2;
    if (len == 1 && buf[0] == '0')
        return 0;
    if (len > 2 && buf[0] == '0' && buf[1] == 'x') {
        v = strtoll(buf + 2, NULL, 16);
    } else {
        v = strtoll(buf, NULL, 10);
    }
    if (v <= 0 || v == LLONG_MAX)
        return -3;
    return v;
}

int64_t get_obj_attr_int(PyObject * obj, const char * name)
{
    int64_t hr = 0;
    PyObject * attr = PyObject_GetAttrString(obj, name);
    FIN_IF(!attr, LLONG_MIN);  // error
    FIN_IF(attr == Py_True, 1);
    FIN_IF(attr == Py_False, 0);
    FIN_IF(!PyLong_CheckExact(attr), LLONG_MIN);
    hr = PyLong_AsSsize_t(attr);
fin:
    Py_XDECREF(attr);
    return hr;
}

const char * get_obj_attr_str(PyObject * obj, const char * name)
{
    const char * res;
    PyObject * attr = PyObject_GetAttrString(obj, name);
    if (!attr || !PyUnicode_CheckExact(attr)) {
        Py_XDECREF(attr);
        return NULL;
    }
    res = PyUnicode_AsUTF8(attr);
    Py_XDECREF(attr);
    return res;
}

int get_obj_attr_list_tup(PyObject * obj, const char * name, int idx, PyObject ** buf, int bufsize)
{
    int hr = -1;
    PyObject * list = PyObject_GetAttrString(obj, name);
    FIN_IF(!list, -1);
    FIN_IF(!PyList_Check(list), -2);
    int size = (int)PyList_GET_SIZE(list);
    FIN_IF(size < 1, -3);
    FIN_IF(idx < 0, size);    // only return lenght of list
    FIN_IF(idx >= size, -4);  // incorrect idx
    PyObject * tup = PyList_GET_ITEM(list, idx);
    FIN_IF(!tup, -5);
    FIN_IF(!PyTuple_Check(tup), -6);
    size = (int)PyTuple_GET_SIZE(tup);
    FIN_IF(size <= 0, -7);
    FIN_IF(bufsize <= 0, -8);
    FIN_IF(!buf, -9);
    hr = 0;
    for (idx = 0; idx < bufsize; idx++) {
        buf[idx] = NULL;
        if (idx < size) {
            buf[idx] = PyTuple_GET_ITEM(tup, idx);
            hr++;
        }
    }
fin:
    Py_XDECREF(list);
    return hr;
}

int get_obj_attr_bindlist(PyObject * obj, const char * name, int idx, const char ** host, int * port)
{
    int hr = -1;
    PyObject * buf[2];
    int size = get_obj_attr_list_tup(obj, name, -1, NULL, 0);
    FIN_IF(size < 1, -3);
    FIN_IF(idx < 0, size);
    *host = NULL;
    *port = 0;
    hr = get_obj_attr_list_tup(obj, name, idx, buf, COUNTOF(buf));
    if (hr >= COUNTOF(buf)) {
        *host = (buf[0] == NULL) ? NULL :   PyUnicode_AsUTF8(buf[0]);
        *port = (buf[1] == NULL) ? 0 : (int)PyLong_AsSsize_t(buf[1]);
    }
fin:
    return hr;
}

static const char weekDays[7][4] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char monthList[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

uint32_t g_actual_time = 0;
char g_actual_asctime[32] = { 0 };
int g_actual_asctime_len = 0;

int get_asctime(char ** asc_time)
{
    uint32_t curr_ticks;
#ifdef _WIN32
    curr_ticks = GetTickCount() / 1000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    curr_ticks = (uint32_t)ts.tv_sec;
#endif
    if (curr_ticks == g_actual_time && g_actual_asctime_len) {
        *asc_time = g_actual_asctime;
        return g_actual_asctime_len;
    }
    time_t curr_time = time(NULL);
    struct tm tv;
#ifdef _WIN32
    gmtime_s(&tv, &curr_time);
#else
    gmtime_r(&curr_time, &tv);
#endif
    char buf[64];
    int len = sprintf(buf, "%s, %02d %s %04d %02d:%02d:%02d GMT",
        weekDays[tv.tm_wday], tv.tm_mday, monthList[tv.tm_mon],
        1900 + tv.tm_year, tv.tm_hour, tv.tm_min, tv.tm_sec);
    if (len > 0 && len < 32) {
        g_actual_time = curr_ticks;
        g_actual_asctime_len = len;
        memcpy(g_actual_asctime, buf, 32);
        *asc_time = g_actual_asctime;
        return len;
    }
    *asc_time = "";
    return 0;
}

PyObject * get_function(PyObject * object)
{
    if (PyFunction_Check(object)) {
        Py_INCREF(object);
        return object;
    }
    if (PyMethod_Check(object)) {
        PyObject * met = PyMethod_GET_FUNCTION(object);
        Py_INCREF(met);
        return met;
    }
    PyObject * call = PyObject_GetAttrString(object, "__call__");
    if (call) {
        if (PyFunction_Check(call))
            return call;

        if (PyMethod_Check(call)) {
            PyObject * met = PyMethod_GET_FUNCTION(call);
            if (PyFunction_Check(met)) {
                Py_INCREF(met);
                Py_DECREF(call);
                return met;
            }
        }
        /*
        if (PyCFunction_Check(call))
            return call;

        const char * type_name = Py_TYPE(call)->tp_name;
        
        if (strcmp(type_name, "method-wrapper") == 0)
            return call;
        
        if (strcmp(type_name, "builtin_function_or_method") == 0)
            return call;
        */
        Py_DECREF(call);
    }
    return NULL;
}

int get_func_sig_arg_count(PyObject * func)
{
    int hr = -1;
    PyObject * params = NULL;
    PyObject * sig = NULL;
    PyObject * inspect = PyImport_ImportModule("inspect");
    FIN_IF(!inspect, -2);
    sig = PyObject_CallMethod(inspect, "signature", "O", func);
    if (!sig) PyErr_Clear();
    FIN_IF(!sig, -3);
    params = PyObject_GetAttrString(sig, "parameters");
    FIN_IF(!params, -4);
    hr = (int)PyMapping_Size(params);
fin:
    Py_XDECREF(params);
    Py_XDECREF(sig);
    Py_XDECREF(inspect);
    return hr;
}

bool is_coroutine_function(PyObject * func)
{
    int hr = -1;
    PyObject * call_attr = NULL;
    PyObject * ret = NULL;
    PyObject * asyncio = NULL;
    asyncio = PyImport_ImportModule("asyncio");
    FIN_IF(!asyncio, -3);
    ret = PyObject_CallMethod(asyncio, "iscoroutinefunction", "O", func);
    if (!ret) PyErr_Clear();
    FIN_IF(!ret, -4);
    hr = PyObject_IsTrue(ret) ? 0 : -1;
    if (hr) {
        Py_XDECREF(ret);
        call_attr = PyObject_GetAttrString(func, "__call__");
        FIN_IF(!call_attr, -5);
        ret = PyObject_CallMethod(asyncio, "iscoroutinefunction", "O", call_attr);
        if (!ret) PyErr_Clear();
        FIN_IF(!ret, -6);
        hr = PyObject_IsTrue(ret) ? 0 : -1;
    }
fin:
    Py_XDECREF(ret);
    Py_XDECREF(call_attr);
    Py_XDECREF(asyncio);
    return (hr == 0) ? true : false;
}
