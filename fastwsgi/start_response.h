#ifndef START_RESPONSE_H_
#define START_RESPONSE_H_

#include "common.h"

typedef struct {
    PyObject   ob_base;
    PyObject * status;
    PyObject * headers;
    PyObject * exc_info;
    int        called;
} StartResponse;

extern PyTypeObject StartResponse_Type;

INLINE
static StartResponse * create_start_response(void)
{
    StartResponse * sr = PyObject_NEW(StartResponse, &StartResponse_Type);
    if (sr) {
        sr->status = NULL;
        sr->headers = NULL;
        sr->exc_info = NULL;
        sr->called = 0;
    }
    return sr;
}

#endif
