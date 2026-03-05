#include "lifespan.h"
#include "server.h"
#include "constants.h"

/* -------------------------------------------------------------------------
 * Python string constants local to this module.
 * We reuse g_cv where possible; the lifespan-specific ones are static here.
 * ---------------------------------------------------------------------- */

static PyObject * g_ls_type_startup           = NULL;
static PyObject * g_ls_type_startup_complete  = NULL;
static PyObject * g_ls_type_startup_failed    = NULL;
static PyObject * g_ls_type_shutdown          = NULL;
static PyObject * g_ls_type_shutdown_complete = NULL;
static PyObject * g_ls_type_shutdown_failed   = NULL;
static PyObject * g_ls_str_lifespan           = NULL;
static PyObject * g_ls_str_message            = NULL;
static PyObject * g_ls_str_exception          = NULL;

static int g_ls_constants_inited = 0;

static int ls_init_constants(void)
{
    if (!g_ls_constants_inited) {
        g_ls_type_startup           = PyUnicode_FromString("lifespan.startup");
        g_ls_type_startup_complete  = PyUnicode_FromString("lifespan.startup.complete");
        g_ls_type_startup_failed    = PyUnicode_FromString("lifespan.startup.failed");
        g_ls_type_shutdown          = PyUnicode_FromString("lifespan.shutdown");
        g_ls_type_shutdown_complete = PyUnicode_FromString("lifespan.shutdown.complete");
        g_ls_type_shutdown_failed   = PyUnicode_FromString("lifespan.shutdown.failed");
        g_ls_str_lifespan           = PyUnicode_FromString("lifespan");
        g_ls_str_message            = PyUnicode_FromString("message");
        g_ls_str_exception          = PyUnicode_FromString("exception");
        g_ls_constants_inited = 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Forward declarations for the ASGI-method objects
 * ---------------------------------------------------------------------- */

static PyObject * lifespan_recv(PyObject * self, PyObject * notused);
static PyObject * lifespan_send(PyObject * self, PyObject * dict);
static PyObject * lifespan_done(PyObject * self, PyObject * task);

static PyMethodDef lifespan_methods[] = {
    { "receive", lifespan_recv, METH_NOARGS, 0 },
    { "send",    lifespan_send, METH_O,      0 },
    { "done",    lifespan_done, METH_O,      0 },
    { NULL,      NULL,          0,           0 }
};

/* The lifespan object is a thin Python object that carries a pointer back
 * to the server's lifespan_t so the receive/send callbacks can update state.
 * It also acts as an awaitable so asyncio can call it as a coroutine arg. */

typedef struct {
    PyObject   ob_base;
    lifespan_t * ls;   // weak ref — owned by g_srv
} ls_obj_t;

static void ls_obj_dealloc(ls_obj_t * self)
{
    PyObject_Del(self);
}

static PyObject * ls_obj_await(PyObject * self)
{
    Py_INCREF(self);
    return self;
}

static PyObject * ls_obj_iternext(PyObject * self)
{
    return NULL;
}

static PyAsyncMethods ls_async_methods = {
    .am_await = ls_obj_await
};

static PyTypeObject LS_Obj_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "LifespanHandler",
    .tp_basicsize = sizeof(ls_obj_t),
    .tp_dealloc   = (destructor) ls_obj_dealloc,
    .tp_as_async  = &ls_async_methods,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_iter      = ls_obj_iternext,
    .tp_iternext  = ls_obj_iternext,
    .tp_methods   = lifespan_methods,
};

/* -------------------------------------------------------------------------
 * Helper: create a new asyncio.Future
 * ---------------------------------------------------------------------- */
static
INLINE
PyObject * ls_create_future(void)
{
    return PyObject_CallObject(g_srv.aio.loop.create_future, NULL);
}

/* -------------------------------------------------------------------------
 * Helper: resolve a future with a result (call_soon so it's safe from C)
 * ---------------------------------------------------------------------- */
static
int ls_future_set_result(PyObject * future, PyObject * result)
{
    int hr = -1;
    PyObject * done = NULL;
    PyObject * set_result = NULL;
    PyObject * ret = NULL;
    
    if (!future)
        return -1;

    // Check it hasn't already been resolved
    done = PyObject_CallMethodObjArgs(future, g_cv.done, NULL);
    FIN_IF(!done, -2);
    FIN_IF(done == Py_True, 0);  // already done

    set_result = PyObject_GetAttr(future, g_cv.set_result);
    FIN_IF(!set_result, -3);

    ret = PyObject_CallFunctionObjArgs(g_srv.aio.loop.call_soon, set_result, result, NULL);
    FIN_IF(!ret, -4);

    hr = 0;
fin:
    Py_XDECREF(ret);
    Py_XDECREF(set_result);
    Py_XDECREF(done);
    return hr;
}

/* -------------------------------------------------------------------------
 * lifespan.receive()
 *
 * Called by the app coroutine when it awaits the next lifespan event.
 * We return a future; the server resolves it with the next event dict
 * (lifespan.startup or lifespan.shutdown).
 * ---------------------------------------------------------------------- */
static
PyObject * lifespan_recv(PyObject * self, PyObject * notused)
{
    ls_obj_t  * obj = (ls_obj_t *)self;
    lifespan_t * ls = obj->ls;

    LOGd("%s: state = %d", __func__, (int)ls->state);

    PyObject * future = ls_create_future();
    if (!future)
        return NULL;

    // Replace any previously stored recv_future
    Py_XDECREF(ls->recv_future);
    ls->recv_future = future;
    Py_INCREF(future);   // one ref owned by ls->recv_future, one returned

    // If we are in STARTING state the server is waiting for the app to
    // call receive() — resolve immediately with the startup event so the coroutine can proceed.
    if (ls->state == LS_STATE_STARTING) {
        PyObject * event = PyDict_New();
        PyDict_SetItem(event, g_cv.type, g_ls_type_startup);
        ls_future_set_result(future, event);
        Py_DECREF(event);
    }
    else if (ls->state == LS_STATE_STOPPING) {
        PyObject * event = PyDict_New();
        PyDict_SetItem(event, g_cv.type, g_ls_type_shutdown);
        ls_future_set_result(future, event);
        Py_DECREF(event);
    }
    // Otherwise the future just waits — the server will resolve it later

    return future;  // caller owns this ref
}

/* -------------------------------------------------------------------------
 * lifespan.send()
 *
 * Called by the app coroutine to report startup.complete / startup.failed /
 * shutdown.complete / shutdown.failed.
 * ---------------------------------------------------------------------- */
static
PyObject * lifespan_send(PyObject * self, PyObject * dict)
{
    ls_obj_t  * obj = (ls_obj_t *)self;
    lifespan_t * ls = obj->ls;

    if (!PyDict_Check(dict)) {
        PyErr_SetString(PyExc_TypeError, "lifespan send() expects a dict");
        return NULL;
    }
    PyObject * type_obj = PyDict_GetItem(dict, g_cv.type);
    if (!type_obj || !PyUnicode_Check(type_obj)) {
        PyErr_SetString(PyExc_ValueError, "lifespan send() dict missing 'type'");
        return NULL;
    }
    const char * event = PyUnicode_AsUTF8(type_obj);
    LOGi("%s: event = \"%s\"", __func__, event);

    if (strcmp(event, "lifespan.startup.complete") == 0) {
        ls->state = LS_STATE_READY;
        if (ls->startup_future) {
            ls_future_set_result(ls->startup_future, Py_True);
        }
    }
    else if (strcmp(event, "lifespan.startup.failed") == 0) {
        ls->state = LS_STATE_FAILED;
        snprintf(ls->fail_message, sizeof(ls->fail_message), "%s (no message)", event);
        PyObject * msg_obj = PyDict_GetItem(dict, g_ls_str_message);
        if (msg_obj && PyUnicode_Check(msg_obj)) {
            const char * msg = PyUnicode_AsUTF8(msg_obj);
            if (msg)
                snprintf(ls->fail_message, sizeof(ls->fail_message), "%s", msg);
        }
        LOGe("%s: lifespan startup failed: %s", __func__, ls->fail_message);
        if (ls->startup_future) {
            ls_future_set_result(ls->startup_future, Py_False);
        }
    }
    else if (strcmp(event, "lifespan.shutdown.complete") == 0) {
        ls->state = LS_STATE_DONE;
        if (ls->shutdown_future) {
            ls_future_set_result(ls->shutdown_future, Py_True);
        }
    }
    else if (strcmp(event, "lifespan.shutdown.failed") == 0) {
        ls->state = LS_STATE_DONE;  // treat as done — we're shutting down anyway
        PyObject * msg_obj = PyDict_GetItem(dict, g_ls_str_message);
        if (msg_obj && PyUnicode_Check(msg_obj)) {
            const char * msg = PyUnicode_AsUTF8(msg_obj);
            if (msg)
                snprintf(ls->fail_message, sizeof(ls->fail_message), "%s", msg);
        }
        LOGe("%s: lifespan shutdown failed: %s", __func__, ls->fail_message);
        if (ls->shutdown_future) {
            ls_future_set_result(ls->shutdown_future, Py_True);
        }
    }
    else {
        LOGw("%s: unknown event type '%s'", __func__, event);
    }

    // Return a resolved future so the app's `await send(...)` completes
    PyObject * future = ls_create_future();
    if (!future)
        return NULL;

    // Store for possible cancellation
    Py_XDECREF(ls->send_future);
    ls->send_future = future;
    Py_INCREF(future);

    ls_future_set_result(future, Py_None);
    return future;   // caller owns this ref
}

/* -------------------------------------------------------------------------
 * lifespan.done()  — asyncio task done-callback
 *
 * Called when the lifespan coroutine task finishes (normally or with
 * exception).  We use this to detect unsupported lifespan (TypeError raised
 * by the app) and to clean up.
 * ---------------------------------------------------------------------- */
static
PyObject * lifespan_done(PyObject * self, PyObject * task)
{
    ls_obj_t  * obj = (ls_obj_t *)self;
    lifespan_t * ls = obj->ls;

    LOGd("%s: state = %d", __func__, (int)ls->state);

    // Retrieve task result / exception
    PyObject * exc = PyObject_CallMethodObjArgs(task, g_ls_str_exception, NULL);
    if (exc && exc != Py_None) {
        // Check if it's a TypeError — app doesn't handle lifespan scope
        if (PyErr_GivenExceptionMatches(exc, PyExc_TypeError) ||
            PyErr_GivenExceptionMatches(exc, PyExc_NotImplementedError)) {
            if (ls->mode == LS_MODE_AUTO) {
                LOGn("%s: app does not support lifespan scope -> ignoring (auto mode)", __func__);
                ls->state = LS_STATE_UNSUPPORTED;
            } else {
                LOGe("%s: app raised TypeError -> lifespan not supported", __func__);
                ls->state = LS_STATE_FAILED;
                snprintf(ls->fail_message, sizeof(ls->fail_message), "App does not support lifespan scope");
            }
        } else {
            // Some other exception
            PyObject * repr = PyObject_Str(exc);
            const char * repr_str = repr ? PyUnicode_AsUTF8(repr) : "<unknown>";
            LOGe("%s: lifespan task raised exception: %s", __func__, repr_str);
            Py_XDECREF(repr);
            if (ls->state == LS_STATE_STARTING || ls->state == LS_STATE_IDLE) {
                ls->state = LS_STATE_FAILED;
                snprintf(ls->fail_message, sizeof(ls->fail_message), "Lifespan coroutine raised an exception");
            } else {
                // Exception during shutdown -> log and mark done
                ls->state = LS_STATE_DONE;
            }
        }
    } else {
        // Normal exit without exception — if we were still STARTING that's
        // a bug in the app (it returned without sending startup.complete),
        // but we treat it as unsupported in auto mode.
        if (ls->state == LS_STATE_STARTING) {
            if (ls->mode == LS_MODE_AUTO) {
                ls->state = LS_STATE_UNSUPPORTED;
            } else {
                ls->state = LS_STATE_FAILED;
                snprintf(ls->fail_message, sizeof(ls->fail_message), "Lifespan coroutine exited before sending startup.complete");
            }
        }
    }
    PyErr_Clear();
    Py_XDECREF(exc);

    // If startup_future is still pending, resolve it so lifespan_startup() unblocks.
    if (ls->startup_future) {
        PyObject * done = PyObject_CallMethodObjArgs(ls->startup_future, g_cv.done, NULL);
        if (done && done != Py_True) {
            PyObject * result = (ls->state == LS_STATE_READY) ? Py_True : Py_False;
            ls_future_set_result(ls->startup_future, result);
        }
        Py_XDECREF(done);
    }
    // Same for shutdown_future
    if (ls->shutdown_future) {
        PyObject * done = PyObject_CallMethodObjArgs(ls->shutdown_future, g_cv.done, NULL);
        if (done && done != Py_True) {
            ls_future_set_result(ls->shutdown_future, Py_True);
        }
        Py_XDECREF(done);
    }

    Py_RETURN_NONE;
}

// ----------------------------------------------------------------------
// --------------------- Public API -------------------------------------
// ----------------------------------------------------------------------

int lifespan_init(lifespan_t * ls)
{
    ls->state = LS_STATE_IDLE;

    if (ls->mode == LS_MODE_OFF)
        return 0;

    if (ls_init_constants() != 0) {
        LOGe("%s: failed to create Python string constants", __func__);
        return -1;
    }
    if (PyType_Ready(&LS_Obj_Type) < 0) {
        LOGe("%s: PyType_Ready(LS_Obj_Type) failed", __func__);
        return -2;
    }

    // Build the lifespan scope dict (ASGI spec 2.0)
    ls->scope = PyDict_New();
    if (!ls->scope)
        return -3;

    PyObject * scope_asgi = PyDict_New();
    PyDict_SetItem(scope_asgi, g_cv.version,      g_cv.v3_0);
    PyDict_SetItem(scope_asgi, g_cv.spec_version, g_cv.v2_0);

    PyDict_SetItem(ls->scope, g_cv.type, g_ls_str_lifespan);
    PyDict_SetItem(ls->scope, g_cv.asgi, scope_asgi);
    Py_DECREF(scope_asgi);

    LOGd("%s: mode = %d, fail_on_startup_error = %d", __func__, (int)ls->mode, ls->fail_on_startup_error);
    return 0;
}

void lifespan_free(lifespan_t * ls)
{
    if (ls) {
        Py_CLEAR(ls->startup_future);
        Py_CLEAR(ls->shutdown_future);
        Py_CLEAR(ls->recv_future);
        Py_CLEAR(ls->send_future);
        Py_CLEAR(ls->task);
        Py_CLEAR(ls->scope);
    }
}

/* -------------------------------------------------------------------------
 * lifespan_startup()
 *
 * Creates the lifespan coroutine task and drives the asyncio loop until
 * startup.complete (or failure / unsupported) is received.
 *
 * We run the loop in small ticks (call_soon + run_until_complete on a
 * dedicated future) instead of run_forever so we block only during startup.
 * ---------------------------------------------------------------------- */
int lifespan_startup(lifespan_t * ls)
{
    int hr = -1;
    ls_obj_t * handler = NULL;
    PyObject * recv = NULL;
    PyObject * send = NULL;
    PyObject * done = NULL;
    PyObject * coroutine = NULL;

    FIN_IF(!ls || ls->mode == LS_MODE_OFF, 0);
    LOGw_IF(!g_srv.asgi_app, "%s: no ASGI app configured", __func__);
    FIN_IF(!g_srv.asgi_app, 0);

    // Create the ls_obj that is passed to the app as the handler object
    handler = PyObject_New(ls_obj_t, &LS_Obj_Type);
    LOGe_IF(!handler, "%s: cannot create LS_Obj_Type instance", __func__);
    FIN_IF(!handler, -2);
    handler->ls = ls;

    // Create the startup_future — we'll wait on this
    ls->startup_future = ls_create_future();
    FIN_IF(!ls->startup_future, -3);

    // Get receive / send / done method objects from the handler
    recv = PyObject_GetAttrString((PyObject *)handler, "receive");
    send = PyObject_GetAttrString((PyObject *)handler, "send");
    done = PyObject_GetAttrString((PyObject *)handler, "done");
    LOGe_IF(!recv || !send || !done, "%s: cannot get handler methods", __func__);
    FIN_IF(!recv, -4);
    FIN_IF(!send, -5);
    FIN_IF(!done, -6);

    // Call the ASGI app with the lifespan scope.
    // app(scope, receive, send) returns a coroutine.
    ls->state = LS_STATE_STARTING;

    coroutine = PyObject_CallFunctionObjArgs(g_srv.asgi_app, ls->scope, recv, send, NULL);
    if (!coroutine) {
        // App raised an exception immediately (not a coroutine)
        if (PyErr_ExceptionMatches(PyExc_TypeError) && ls->mode == LS_MODE_AUTO) {
            LOGn("%s: app raised TypeError synchronously -> assuming no lifespan support", __func__);
            PyErr_Clear();
            ls->state = LS_STATE_UNSUPPORTED;
            FIN(0);
        }
        LOGe("%s: app() call failed", __func__);
        PyErr_Print();
        PyErr_Clear();
        FIN(ls->fail_on_startup_error ? -101 : 0);
    }
    if (!PyCoro_CheckExact(coroutine)) {
        LOGe("%s: app() did not return a coroutine", __func__);
        FIN(ls->fail_on_startup_error ? -102 : 0);
    }

    // Wrap coroutine in an asyncio.Task
    PyObject * task = PyObject_CallFunctionObjArgs(g_srv.aio.loop.create_task, coroutine, NULL);
    Py_CLEAR(coroutine);
    LOGe_IF(!task, "%s: create_task() failed", __func__);
    FIN_IF(!task, 21);
    Py_INCREF(task);
    ls->task = task;

    // Register done-callback so lifespan_done() is called when task finishes
    PyObject * ret = PyObject_CallMethodObjArgs(task, g_cv.add_done_callback, done, NULL);
    Py_XDECREF(ret);

    // Schedule the first receive() trigger via call_soon so the coroutine starts running before we block on the startup_future.
    PyObject * noop = PyObject_CallFunctionObjArgs(g_srv.aio.loop.call_soon, g_srv.aio.uni_loop, NULL);
    Py_XDECREF(noop);

    // Drive the asyncio event loop until startup_future is resolved.
    // run_until_complete() blocks the Python thread but libuv is not involved yet at this point — we haven't called uv_listen yet.
    PyObject * result = PyObject_CallFunctionObjArgs(g_srv.aio.loop.run_until_complete, ls->startup_future, NULL);
    Py_XDECREF(result);

    hr = 0;
    // Evaluate outcome
    switch (ls->state) {
        case LS_STATE_READY:
            LOGn("%s: startup.complete received -> server is ready", __func__);
            FIN(0);

        case LS_STATE_UNSUPPORTED:
            LOGn("%s: app does not support lifespan -> continuing", __func__);
            FIN(0);

        case LS_STATE_FAILED:
            LOGe("%s: startup FAILED: %s", __func__, ls->fail_message);
            LOGw_IF(!ls->fail_on_startup_error, "%s: fail_on_startup_error = 0, continuing despite failure", __func__);
            FIN(ls->fail_on_startup_error ? -105 : 0);

        default:
            LOGe("%s: unexpected state after startup: %d", __func__, (int)ls->state);
            FIN(ls->fail_on_startup_error ? -106 : 0);
    }
fin:
    Py_XDECREF(coroutine);
    Py_XDECREF(recv);
    Py_XDECREF(send);
    Py_XDECREF(done);
    Py_XDECREF(handler);
    return hr;
}

/* -------------------------------------------------------------------------
 * lifespan_shutdown()
 *
 * Sends the shutdown event and waits for shutdown.complete.
 * Called from close_server() while the asyncio loop is still alive.
 * ---------------------------------------------------------------------- */
int lifespan_shutdown(lifespan_t * ls)
{
    int hr = -1;

    FIN_IF(!ls || ls->mode == LS_MODE_OFF, 0);
    LOGw_IF(!g_srv.asgi_app, "%s: no ASGI app configured", __func__);
    FIN_IF(!g_srv.asgi_app, 0);
    
    FIN_IF(ls->state != LS_STATE_READY, 0); // startup never completed -> nothing to shut down

    LOGd("%s: sending shutdown event ...", __func__);
    ls->state = LS_STATE_STOPPING;

    // Create the shutdown_future
    ls->shutdown_future = ls_create_future();
    LOGe_IF(!ls->shutdown_future, "%s: cannot create shutdown_future", __func__);
    FIN_IF(!ls->shutdown_future, -2);

    // Resolve the pending recv_future (the app is suspended on receive())
    // with the shutdown event dict — this unblocks the coroutine.
    if (ls->recv_future) {
        PyObject * event = PyDict_New();
        PyDict_SetItem(event, g_cv.type, g_ls_type_shutdown);
        ls_future_set_result(ls->recv_future, event);
        Py_DECREF(event);
        Py_CLEAR(ls->recv_future);
    }
    // Drive the loop until shutdown_future resolves
    LOGi("%s: waiting for shutdown.complete ...", __func__);
    PyObject * result = PyObject_CallFunctionObjArgs(g_srv.aio.loop.run_until_complete, ls->shutdown_future, NULL);
    Py_XDECREF(result);

    LOGi("%s: complete with state = %d", __func__, (int)ls->state);
    hr = 0;
fin:
    return hr;
}
