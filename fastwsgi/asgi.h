#ifndef _FASTWSGI_ASGI_
#define _FASTWSGI_ASGI_

#include "common.h"
#include "request.h"
#include "lifespan.h"


typedef enum {
    UL_DISABLED = 0,
    UL_CALL_SOON,        // inited via call_soon
    UL_CALL_LATER,       // inited via call_later
    UL_RUNNING,          // busy or not active
} uni_loop_state_t;

typedef struct {
    PyObject * asyncio;  // module
    PyObject * uni_loop; // united loop
    uni_loop_state_t uni_loop_state;
    int        idle_num;
    int        allow_call_soon;
    int        loop_timeout_us; // timeout for call_later into uni_loop (microsec)
    int        req_hdr_lower;   // 0 = not change case for header names, 1 = force lowercase
    struct {
        int is_stock;           // 1 = used stock asyncio event loop object
        int borrowed;
        PyObject * self;
        PyObject * run_forever;
        PyObject * run_until_complete;
        PyObject * call_soon;
        PyObject * call_later;
        PyObject * call_at;
        PyObject * create_future;
        PyObject * create_task;
        PyObject * add_reader;
        PyObject * remove_reader;
        PyObject * relax_timeout;  // sec (float)
    } loop;
    struct {
        PyObject * self;
        PyObject * set_result;
    } future;
    lifespan_t     lifespan;    // ASGI lifespan state
} asyncio_t;

int asyncio_init(asyncio_t * aio);
int asyncio_free(asyncio_t * aio, bool free_self);

int asyncio_load_cfg(asyncio_t * aio);

int aio_loop_run(asyncio_t * aio);
int aio_loop_shutdown(asyncio_t * aio);
int aio_loop_call(asyncio_t * aio, PyObject * func_cb, int timeout_us, PyObject * arg);


typedef struct {
    PyObject   ob_base;
    client_t * client;
    PyObject * task;   // task for coroutine
    PyObject * scope;  // PyDict
    struct {
        PyObject * future;
        bool       completed;
    } recv;
    struct {
        PyObject * future;
        int        status;   // response status
        PyObject * start_response;  // PyDict
        int        num_body;
        int64_t    body_size;
        bool       latest_chunk;
    } send;
} asgi_t;

extern PyTypeObject ASGI_Type;

static
INLINE
PyObject * create_asgi(client_t * client)
{
    asgi_t * asgi = PyObject_New(asgi_t, &ASGI_Type);
    if (asgi) {
        size_t prefix = offsetof(asgi_t, client);
        memset((char *)asgi + prefix, 0, sizeof(asgi_t) - prefix);
        asgi->client = client;
    }
    return (PyObject *)asgi;
}


bool asgi_app_check(PyObject * app);
int  asgi_init(client_t * client);
int  asgi_free(client_t * client);
int  asgi_call_app(client_t * client);

int  asgi_future_set_result_soon(client_t * client, PyObject * future, bool check, PyObject * result);
int  asgi_exec_send_future(client_t * client);

int  asgi_future_set_exception_soon(client_t * client, PyObject * future, const char * fmt, ...);
int  asgi_exec_send_future_as_exception(client_t * client, const char * fmt, ...);


static
INLINE
Py_ssize_t asgi_get_data_from_header(PyObject * object, size_t index, const char ** data)
{
    PyObject * item = NULL;

    if (PyList_Check(object)) {
        if (PyList_GET_SIZE(object) >= 2)
            item = PyList_GET_ITEM(object, index);
    }
    else if (PyTuple_Check(object)) {
        if (PyTuple_GET_SIZE(object) >= 2)
            item = PyTuple_GET_ITEM(object, index);
    }
    else {
        return -39001;  // error
    }
    if (item) {
        if (PyBytes_Check(item)) {
            *data = PyBytes_AS_STRING(item);
            return PyBytes_GET_SIZE(item);
        }
        if (PyUnicode_Check(item)) {
            *data = PyUnicode_DATA(item);
            return PyUnicode_GET_LENGTH(item);
        }
    }
    return -39004;
}


#endif
