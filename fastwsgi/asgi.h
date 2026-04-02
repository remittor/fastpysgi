#ifndef _FASTWSGI_ASGI_
#define _FASTWSGI_ASGI_

#include "common.h"
#include "request.h"
#include "lifespan.h"


typedef struct {
    PyObject * asyncio;  // module
    PyObject * uni_loop; // united loop
    PyObject * uni_loop_periodic;  // united loop for call_later calls
    int        periodic_armed;     // 1 = call_later already in aio queue
    int        idle_num;
    int        loop_timeout;  // millisec
    int        req_hdr_lower;   // 0 = not change case for header names, 1 = force lowercase
    struct {
        int borrowed;
        PyObject * self;
        PyObject * run_forever;
        PyObject * run_until_complete;
        PyObject * call_soon;
        PyObject * call_later;
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

int  asgi_future_set_result(client_t * client, PyObject ** ptr_future, PyObject * result);
int  asgi_future_set_exception(client_t * client, PyObject ** ptr_future, const char * fmt, ...);


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
