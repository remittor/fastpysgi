#include "asgi.h"
#include "server.h"
#include "lifespan.h"
#include "constants.h"
#include "pyhacks.h"


bool asgi_app_check(PyObject * app)
{
    return is_coroutine_function(app);
}

bool asgi_app_check2(PyObject * app)
{
    int argcnt = get_func_sig_arg_count(app);
    // ASGI: (scope, receive, send) = 3 arguments
    // WSGI: (environ, start_response) = 2 arguments
    return (argcnt == 3) ? true : false;
}

#define PTR_SWAP(a, b)  do { void * tmp = (void*)(a); a = b; b = tmp; } while(0)

PyObject * uni_loop(PyObject * self, PyObject * not_used)
{
    int rc = 0;
    PyObject * res = NULL;
    uv_metrics_t uv_metrics_list[2];
    uv_metrics_t * uv_metrics_before = &uv_metrics_list[0];
    uv_metrics_t * uv_metrics_after  = &uv_metrics_list[1];

    if (g_srv.exit_code != 0) {
        LOGd("%s: exit_code = %d, stopping asyncio loop", __func__, g_srv.exit_code);
        g_srv.aio.uni_loop_state = UL_DISABLED;
        res = PyObject_CallMethod(g_srv.aio.loop.self, "stop", NULL);
        Py_XDECREF(res);
        Py_RETURN_NONE;
    }
    if (g_srv.aio.uni_loop_state == UL_DISABLED) {
        Py_RETURN_NONE;
    }
    g_srv.aio.uni_loop_state = UL_RUNNING;
    g_srv.num_loop_cb = 0;  // reset cb counter

    int uv_run_count = 0;
    uv_metrics_info(g_srv.loop, uv_metrics_before);
    while (1) {
        uv_run(g_srv.loop, UV_RUN_NOWAIT);
        uv_run_count++;
        uv_metrics_info(g_srv.loop, uv_metrics_after);
        if (uv_run_count > 150 || uv_metrics_after->events - uv_metrics_before->events == 0) {
            break;
        }
        PTR_SWAP(uv_metrics_before, uv_metrics_after);
    }
    if (g_srv.num_loop_cb == 0 && g_srv.num_writes == 0) {
        if (g_srv.aio.idle_num < LONG_MAX)
            g_srv.aio.idle_num++;
    } else {
        g_srv.aio.idle_num = 0;
    }
    if (g_srv.aio.idle_num <= 50) {
        rc = aio_loop_call(&g_srv.aio, (PyObject *)UL_CALL_SOON, -1, NULL);
    } else {
        int timeout_ms = uv_backend_timeout(g_srv.loop);
        if (timeout_ms == 0 || g_srv.aio.loop.relax_timeout == NULL) {
            rc = aio_loop_call(&g_srv.aio, (PyObject *)UL_CALL_SOON, -1, NULL);
        } else {
            rc = aio_loop_call(&g_srv.aio, (PyObject *)UL_CALL_LATER, -1, NULL);
        }
    }
    if (rc < 0) {
        const char * method = (g_srv.aio.uni_loop_state == UL_CALL_SOON) ? "call_soon" : "call_later";
        LOGf("%s: method %s cannot insert 'uni_loop' into aio.loop !!!", __func__, method);
        abort();
    }
    Py_RETURN_NONE;
}

static PyMethodDef uni_loop_method = {
    "uni_loop", uni_loop, METH_NOARGS, ""
};

int asyncio_init(asyncio_t * aio)
{
    int hr = 0;
    PyObject * set_event_loop = NULL;
    PyObject * new_event_loop = NULL;
    PyObject * module = NULL;
    PyObject * res = NULL;

    aio->asyncio = PyImport_ImportModule("asyncio");
    FIN_IF(!aio->asyncio, -4500010);

    PyObject * aio_loop = PyObject_GetAttrString(g_srv.pysrv, "loop");
    if (!aio_loop) PyErr_Clear();
    if (aio_loop && aio_loop == Py_None) {
        aio_loop = NULL;
    }
    if (!aio_loop) {
        aio->loop.borrowed = 0;
        set_event_loop = PyObject_GetAttrString(aio->asyncio, "set_event_loop");
        FIN_IF(!set_event_loop, -4500017);
        new_event_loop = PyObject_GetAttrString(aio->asyncio, "new_event_loop");
        FIN_IF(!new_event_loop, -4500018);
        aio->loop.self = PyObject_CallObject(new_event_loop, NULL);
        FIN_IF(!aio->loop.self, -4500020);
        res = PyObject_CallFunctionObjArgs(set_event_loop, aio->loop.self, NULL);
        FIN_IF(!res, -4500021);
    } else {
        aio->loop.self = aio_loop;
        aio->loop.borrowed = 1;
        LOGt("%s: loop.borrowed = %d", __func__, aio->loop.borrowed);
    }
    module = PyObject_GetAttrString((PyObject *)Py_TYPE(aio->loop.self), "__module__");
    FIN_IF(!module, -4500024);
    const char * mod_name = PyUnicode_AsUTF8(module);
    LOGt("%s: aio.loop type module: \"%s\" ", __func__, mod_name);
    if (strcmp(mod_name, "asyncio.unix_events") == 0 || strcmp(mod_name, "asyncio.windows_events") == 0) {
        aio->loop.is_stock = 1;
    }

    aio->loop.run_forever = PyObject_GetAttrString(aio->loop.self, "run_forever");
    FIN_IF(!aio->loop.run_forever, -4500027);
    FIN_IF(!PyCallable_Check(aio->loop.run_forever), -4500028);

    aio->loop.run_until_complete = PyObject_GetAttrString(aio->loop.self, "run_until_complete");
    FIN_IF(!aio->loop.run_until_complete, -4500031);
    FIN_IF(!PyCallable_Check(aio->loop.run_until_complete), -4500032);

    aio->loop.call_soon = PyObject_GetAttrString(aio->loop.self, "call_soon");
    FIN_IF(!aio->loop.call_soon, -4500041);
    FIN_IF(!PyCallable_Check(aio->loop.call_soon), -4500042);

    aio->loop.call_later = PyObject_GetAttrString(aio->loop.self, "call_later");
    FIN_IF(!aio->loop.call_later, -4500045);
    FIN_IF(!PyCallable_Check(aio->loop.call_later), -4500046);

    aio->loop.call_at = PyObject_GetAttrString(aio->loop.self, "call_at");
    if (aio->loop.call_at) {
        FIN_IF(!PyCallable_Check(aio->loop.call_at), -4500048);
    } else {
        aio->loop.is_stock = 0;
    }

    aio->loop.create_future = PyObject_GetAttrString(aio->loop.self, "create_future");
    FIN_IF(!aio->loop.create_future, -4500051);
    FIN_IF(!PyCallable_Check(aio->loop.create_future), -4500052);

    aio->loop.create_task = PyObject_GetAttrString(aio->loop.self, "create_task");
    FIN_IF(!aio->loop.create_task, -4500061);
    FIN_IF(!PyCallable_Check(aio->loop.create_task), -4500062);

    aio->loop.add_reader = PyObject_GetAttrString(aio->loop.self, "add_reader");
    FIN_IF(!aio->loop.add_reader, -4500071);
    FIN_IF(!PyCallable_Check(aio->loop.add_reader), -4500072);

    aio->loop.remove_reader = PyObject_GetAttrString(aio->loop.self, "remove_reader");
    FIN_IF(!aio->loop.remove_reader, -4500081);
    FIN_IF(!PyCallable_Check(aio->loop.remove_reader), -4500082);

    aio->future.self = PyObject_CallObject(aio->loop.create_future, NULL);
    FIN_IF(!aio->future.self, -4500111);

    aio->future.set_result = PyObject_GetAttrString(aio->future.self, "set_result");
    FIN_IF(!aio->future.set_result, -4500113);
    FIN_IF(!PyCallable_Check(aio->future.set_result), -4500115);

    aio->uni_loop = PyCFunction_New(&uni_loop_method, NULL);
    FIN_IF(!aio->uni_loop, -4500213);
    FIN_IF(!PyCallable_Check(aio->uni_loop), -4500214);
    
    aio->loop.relax_timeout = (aio->loop_timeout_us >= 0) ? PyFloat_FromDouble((double)aio->loop_timeout_us / 1000000.0) : NULL;
    aio->uni_loop_state = UL_DISABLED;
    aio->idle_num = 0;

    hr = lifespan_init(&aio->lifespan);
fin:
    Py_XDECREF(set_event_loop);
    Py_XDECREF(new_event_loop);
    Py_XDECREF(module);
    Py_XDECREF(res);
    if (hr) {
        asyncio_free(aio, false);
    }
    return hr;
}

int asyncio_free(asyncio_t * aio, bool free_self)
{
    if (aio) {
        lifespan_free(&aio->lifespan);
        Py_CLEAR(aio->uni_loop);
        Py_CLEAR(aio->future.set_result);
        Py_CLEAR(aio->future.self);
        Py_CLEAR(aio->loop.relax_timeout);
        Py_CLEAR(aio->loop.remove_reader);
        Py_CLEAR(aio->loop.add_reader);
        Py_CLEAR(aio->loop.create_task);
        Py_CLEAR(aio->loop.create_future);
        Py_CLEAR(aio->loop.call_at);
        Py_CLEAR(aio->loop.call_later);
        Py_CLEAR(aio->loop.call_soon);
        Py_CLEAR(aio->loop.run_until_complete);
        Py_CLEAR(aio->loop.run_forever);
        Py_CLEAR(aio->loop.self);
        Py_CLEAR(aio->asyncio);
        if (free_self)
            free(aio);
    }
    return 0;
}

// -----------------------------------------------------------------------------------

int asyncio_load_cfg(asyncio_t * aio)
{
    int64_t rv;
    rv = get_obj_attr_int(g_srv.pysrv, "loop_timeout");  // -1 = call_soon, 0 = 0ms, 3000 = 3ms (default)
    if (rv < 0) {
        aio->loop_timeout_us = (rv == LLONG_MIN) ? 3000 : -1;  // -1 => use call_soon always into uni_loop
    } else {
        aio->loop_timeout_us = (rv >= 0 && rv <= 1000000) ? (int)rv : 1000000;
    }
    rv = get_obj_attr_int(g_srv.pysrv, "loop_call_soon");  // 0 = disabled, 1 = allowed
    aio->allow_call_soon = (rv >= 1) ? 1 : 0;  // 0 is default

    rv = get_obj_attr_int(g_srv.pysrv, "lifespan");  // 0 = off, 1 = on, 2 = auto (default)
    aio->lifespan.mode = (rv >= 0 && rv <= 2) ? (int)rv : (int)LS_MODE_AUTO;

    rv = get_obj_attr_int(g_srv.pysrv, "lifespan_fose");
    aio->lifespan.fail_on_startup_error = (rv == 1) ? 1 : 0;

    rv = get_obj_attr_int(g_srv.pysrv, "req_hdr_lower");
    aio->req_hdr_lower = (rv == 0) ? 0 : 1;

    return 0;
}

// -----------------------------------------------------------------------------------

int aio_loop_run(asyncio_t * _aio)
{
    PyObject * res = NULL;
    asyncio_t * aio = (_aio == NULL) ? &g_srv.aio : _aio;
    g_srv.aio.uni_loop_state = UL_DISABLED;
    int hr = lifespan_startup(&aio->lifespan);
    if (hr) {
        LOGc("%s: lifespan startup failed (rc = %d) -> aborting", __func__, hr);
        g_srv.exit_code = 17;
        return 17;
    }
    if (g_srv.aio.uni_loop_state == UL_DISABLED) {
        aio_loop_call(aio, (PyObject *)UL_CALL_LATER, 0, NULL);
    }
    res = PyObject_CallFunctionObjArgs(aio->loop.run_forever, NULL);
    Py_XDECREF(res);
    LOGd("%s: stopped", __func__);
    return 0;
}

int aio_loop_shutdown(asyncio_t * _aio)
{
    asyncio_t * aio = (_aio == NULL) ? &g_srv.aio : _aio;
    aio->uni_loop_state = UL_DISABLED;
    lifespan_shutdown(&aio->lifespan);
    return 0;
}

int aio_loop_call(asyncio_t * aio, PyObject * func_cb, int timeout_us, PyObject * arg)
{
    PyObject * res = NULL;
    bool uni_loop = false;
    uni_loop_state_t method;
    PyObject * fn_cb = NULL;
    if ((size_t)func_cb == (size_t)UL_CALL_SOON || (size_t)func_cb == (size_t)UL_CALL_LATER) {
        uni_loop = true;
        fn_cb = aio->uni_loop;
        method = (uni_loop_state_t)(size_t)func_cb;
        if (timeout_us < 0 && method == UL_CALL_LATER) {
            timeout_us = aio->loop_timeout_us;
            if (timeout_us < 0) {
                method = UL_CALL_SOON;
            }
        }
    } else {
        fn_cb = func_cb;
        method = (timeout_us < 0) ? UL_CALL_SOON : UL_CALL_LATER;
    }
    if (method == UL_CALL_SOON && !aio->allow_call_soon) {
        method = UL_CALL_LATER;
        timeout_us = 0;
    }
    if (uni_loop) {
        aio->uni_loop_state = method;
    }
    if (method == UL_CALL_SOON) {
        res = PyObject_CallFunctionObjArgs(aio->loop.call_soon, fn_cb, arg, NULL);
    }
    else if (aio->loop.is_stock) {
        uint64_t now = py_time_monotonic_ns();
        uint64_t call_time_ns = (timeout_us > 0) ? now + timeout_us * 1000 : now;
        PyObject * call_time = PyFloat_FromDouble((double)call_time_ns / 1000000000.0);
        res = PyObject_CallFunctionObjArgs(aio->loop.call_at, call_time, fn_cb, arg, NULL);
        Py_XDECREF(call_time);
    }
    else {
        PyObject * v_timeout = NULL;
        PyObject * timeout;
        if (timeout_us <= 0) {
            timeout = g_cv.f0;
        }
        else if (timeout_us == aio->loop_timeout_us && aio->loop.relax_timeout) {
            timeout = aio->loop.relax_timeout;
        }
        else {
            v_timeout = PyFloat_FromDouble((double)timeout_us / 1000000.0);
            timeout = v_timeout;
        }
        res = PyObject_CallFunctionObjArgs(aio->loop.call_later, timeout, fn_cb, arg, NULL);
        Py_XDECREF(v_timeout);
    }
    Py_XDECREF(res);
    return res ? 0 : -1;
}

// -----------------------------------------------------------------------------------

static
void create_asgi_def_scope(void)
{
    for (int idx = 0; idx < g_srv.servers_num; idx++) {
        server_t * server = SERVER(idx);
        if (server->def_scope)
            continue;
        char buf[32];
        sprintf(buf, "%d", server->port);
        PyObject * port = PyUnicode_FromString(buf);
        PyObject * host = PyUnicode_FromString(server->host);
        // only constant values!!!
        PyObject * scope_asgi = PyDict_New();
        PyDict_SetItem(scope_asgi, g_cv.version, g_cv.v3_0);
        PyDict_SetItem(scope_asgi, g_cv.spec_version, g_cv.v2_0);
        PyObject * g_scope = PyDict_New();
        PyDict_SetItem(g_scope, g_cv.type, g_cv.http);
        PyDict_SetItem(g_scope, g_cv.scheme, (server->tls.enabled) ? g_cv.https : g_cv.http);
        PyDict_SetItem(g_scope, g_cv.asgi, scope_asgi);
        Py_DECREF(scope_asgi);
        PyObject * hplist = PyList_New(0);
        PyList_Append(hplist, host);
        PyList_Append(hplist, port);
        PyDict_SetItem(g_scope, g_cv.server, hplist);
        Py_XDECREF(hplist);
        Py_XDECREF(port);
        Py_XDECREF(host);
        // semi-const values
        PyDict_SetItem(g_scope, g_cv.http_version, g_cv.v1_1);
        if (server->root_path.len > 0) {
            PyDict_SetItem(g_scope, g_cv.root_path, server->root_path.obj);
        }
        // non constant values!!!
        PyDict_SetItem(g_scope, g_cv.path, g_cv.empty_string);
        PyDict_SetItem(g_scope, g_cv.raw_path, g_cv.empty_bytes);
        PyDict_SetItem(g_scope, g_cv.query_string, g_cv.empty_bytes);
        server->def_scope = g_scope;
    }
}

int asgi_init(client_t * client)
{
    int hr = 0;
    FIN_IF(!g_srv.asgi_app, 0);
    asgi_free(client);
    PyObject * asgi = create_asgi(client);
    FIN_IF(!asgi, -4510001);
    client->asgi = (asgi_t *)asgi;
    create_asgi_def_scope();
    PyObject * scope = PyDict_Copy(client->server->def_scope);
    FIN_IF(!scope, -4510002);
    PyObject * headers = PyList_New(0);
    PyDict_SetItem(scope, g_cv.headers, headers);
    Py_XDECREF(headers);
    client->asgi->scope = scope;
    hr = 0;
    LOGt("%s: asgi = %p ", __func__, asgi);
fin:
    return hr;
}

int asgi_free(client_t * client)
{
    asgi_t * asgi = client->asgi;
    if (asgi) {
        asgi->client = NULL;
        LOGd("%s: RefCnt(asgi) = %d, RefCnt(task) = %d", __func__, (int)Py_REFCNT(asgi), asgi->task ? (int)Py_REFCNT(asgi->task) : -333);
        Py_DECREF(asgi);
    }
    client->asgi = NULL;
    return 0;
}

int asgi_call_app(client_t * client)
{
    int hr = 0;
    asgi_t * asgi = client->asgi;
    PyObject * coroutine = NULL;
    PyObject * task = NULL;
    PyObject * result = NULL;

    LOGd("%s: ....", __func__);
    PyObject * receive = PyObject_GetAttrString((PyObject *)asgi, "receive");
    PyObject * send = PyObject_GetAttrString((PyObject *)asgi, "send");
    PyObject * done = PyObject_GetAttrString((PyObject *)asgi, "done");
    FIN_IF(!receive || !send || !done, -4502011);

    // call ASGI 3.0 app
    coroutine = PyObject_CallFunctionObjArgs(g_srv.asgi_app, asgi->scope, receive, send, NULL);
    LOGc_IF(!coroutine, "%s: cannot call ASGI 3.0 app", __func__);
    FIN_IF(!coroutine, -4502031);
    FIN_IF(!PyCoro_CheckExact(coroutine), -4502033);

    task = PyObject_CallFunctionObjArgs(g_srv.aio.loop.create_task, coroutine, NULL);
    FIN_IF(!task, -4502041);

    result = PyObject_CallMethodObjArgs(task, g_cv.add_done_callback, done, NULL);
    LOGe_IF(!result, "%s: error on task.add_done_callback", __func__);
    FIN_IF(!result, -4502051);

    LOGi("%s: ASGI TASK created", __func__);
    asgi->task = task;
    Py_INCREF(task);
    hr = 0;
fin:
    LOGe_IF(hr, "%s: FIN with error = %d", __func__, hr);
    Py_XDECREF(receive);
    Py_XDECREF(send);
    Py_XDECREF(done);
    Py_XDECREF(coroutine);
    Py_XDECREF(task);
    Py_XDECREF(result);
    return hr;
}

// -----------------------------------------------------------------------------------

int asgi_get_info_from_response(client_t * client, PyObject * dict)
{
    int hr = 0;
    PyObject * iterator = NULL;
    PyObject * item = NULL;

    client->response.wsgi_content_length = -1;  // unknown
    PyObject * headers = PyDict_GetItem(dict, g_cv.headers);
    FIN_if(!headers, -4, PyErr_Clear());
    iterator = PyObject_GetIter(headers);
    FIN_if(!iterator, -5, PyErr_Clear());

    for (size_t i = 0; /* nothing */ ; i++) {
        Py_XDECREF(item);
        item = PyIter_Next(iterator);
        if (!item)
            break;

        const char * key;
        Py_ssize_t key_len = asgi_get_data_from_header(item, 0, &key);
        FIN_IF(key_len < 0, -10);

        const char * val;
        Py_ssize_t val_len = asgi_get_data_from_header(item, 1, &val);
        FIN_IF(val_len < 0, -11);

        if (key_len == 14 && key[7] == '-' && strcasecmp(key, "Content-Length") == 0) {
            FIN_IF(val_len == 0, -22);  // error
            int64_t clen;
            if (val_len == 1 && val[0] == '0') {
                clen = 0;
            } else {
                clen = strtoll(val, NULL, 10);
                FIN_IF(clen <= 0 || clen == LLONG_MAX, -33);  // error
            }
            LOGi("asgi response: content-length = %lld", (long long)clen);
            client->response.wsgi_content_length = clen;
        }
    }
    hr = 0;
fin:
    LOGc_IF(hr, "%s: error = %d", __func__, hr);
    Py_XDECREF(item);
    Py_XDECREF(iterator);
    return hr;
}

int asgi_build_response(client_t * client)
{
    int hr = 0;
    asgi_t * asgi = client->asgi;
    int status = asgi->send.status;
    PyObject * start_response = asgi->send.start_response;

    int flags = (client->request.keep_alive) ? RF_SET_KEEP_ALIVE : 0;
    int len = build_response(client, flags | RF_HEADERS_ASGI, status, start_response, NULL, -1);
    if (len <= 0) {
        LOGe("%s: error = %d", __func__, len);
        //err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        reset_head_buffer(client);
        reset_response_body(client);
        FIN(len);
    }
    hr = 0;
fin:
    Py_CLEAR(asgi->send.start_response);
    return hr;
}

// -----------------------------------------------------------------------------------

int asgi_future_set_result_soon(client_t * client, PyObject * future, bool check, PyObject * result)
{
    int hr = 0;
    PyObject * set_result = NULL;
    PyObject * done = NULL;

    FIN_IF(!future, -4530914);

    set_result = PyObject_GetAttr(future, g_cv.set_result);
    FIN_if(!set_result, -4530916, PyErr_Clear());
    FIN_IF(!PyCallable_Check(set_result), -4530917);
    if (check) {
        done = PyObject_CallMethodObjArgs(future, g_cv.done, NULL);
        FIN_if(!done, -4530918, PyErr_Clear());
        FIN_IF(done == Py_True, -4530919);  // already completed
    }
    int rc = aio_loop_call(&g_srv.aio, set_result, 0, result);  // call_later
    FIN_IF(rc < 0, -4530921);
    hr = 0;
fin:
    Py_XDECREF(done);
    Py_XDECREF(set_result);
    return hr;
}

int asgi_exec_send_future(client_t * client)
{
    PyObject * send_future = client->asgi->send.future;
    if (!send_future) {
        LOGe("%s: send.future is NULL => closing connection", __func__);
        return -4530931;
    }
    client->asgi->send.future = NULL;
    int err = asgi_future_set_result_soon(client, send_future, true, Py_True);
    Py_DECREF(send_future);
    if (err) {
        LOGe("%s: asgi_future_set_result_soon failed: err = %d => closing connection", __func__, err);
        return -4530931;
    }
    return 0;
}

static
int asgi_v_future_set_exception_soon(client_t * client, PyObject * future, const char * fmt, va_list args)
{
    int hr = 0;
    char text[1024];
    PyObject * exc_text = NULL;
    PyObject * exception = NULL;
    PyObject * set_exception = NULL;

    FIN_IF(!future, -4530851);
    vsnprintf(text, sizeof(text), fmt, args);

    exc_text = PyUnicode_FromString(text);
    FIN_IF(!exc_text, -4530852);

    exception = PyObject_CallFunctionObjArgs(PyExc_RuntimeError, exc_text, NULL);
    FIN_if(!exception, -4530853, PyErr_Clear());

    set_exception = PyObject_GetAttr(future, g_cv.set_exception);
    FIN_if(!set_exception, -4530854, PyErr_Clear());

    int rc = aio_loop_call(&g_srv.aio, set_exception, 0, exception);  // call_later
    FIN_IF(rc < 0, -4530855);
    hr = 0;
fin:
    Py_XDECREF(set_exception);
    Py_XDECREF(exception);
    Py_XDECREF(exc_text);
    return hr;
}

int asgi_future_set_exception_soon(client_t * client, PyObject * future, const char * fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int rc = asgi_v_future_set_exception_soon(client, future, fmt, args);
    va_end(args);
    return rc;
}

int asgi_exec_send_future_as_exception(client_t * client, const char * fmt, ...)
{
    PyObject * send_future = client->asgi->send.future;
    if (send_future) {
        client->asgi->send.future = NULL;
        va_list args;
        va_start(args, fmt);
        asgi_v_future_set_exception_soon(client, send_future, fmt, args);
        va_end(args);
        Py_DECREF(send_future);
    }
    return 0;
}

// -----------------------------------------------------------------------------------

// ASGI coro "receive"
PyObject * asgi_receive(PyObject * self, PyObject * notused)
{
    int hr = 0;
    asgi_t * asgi = (asgi_t *)self;
    client_t * client = asgi->client;
    PyObject * future = NULL;
    PyObject * dict = NULL;
    PyObject * input_body = NULL;
    int64_t input_size = -1;
    
    update_log_prefix(client);
    LOGt("%s: ....", __func__);
    if (asgi->recv.completed) {
        future = PyObject_CallObject(g_srv.aio.loop.create_future, NULL);
        asgi->recv.future = future;
        Py_INCREF(future);
        FIN(0);
    }
    dict = PyDict_New();
    int rc = PyDict_SetItem(dict, g_cv.type, g_cv.http_request);
    FIN_IF(rc, -4560715);

    input_size = client->request.wsgi_input_size;
    if (input_size <= 0) {
        input_size = 0;
        input_body = g_cv.empty_bytes;
        Py_INCREF(input_body);
    } else {
        input_body = PyObject_CallMethodObjArgs(client->request.wsgi_input, g_cv.getbuffer, NULL);
    }
    rc = PyDict_SetItem(dict, g_cv.body, input_body);
    if (rc == 0) {
        Py_DECREF(input_body);
        input_body = NULL;
    }

    future = PyObject_CallObject(g_srv.aio.loop.create_future, NULL);

    int err = asgi_future_set_result_soon(client, future, true, dict);
    FIN_IF(err, -4560775);
    asgi->recv.completed = true;

    LOGd("%s: recv size = %d", __func__, input_size);
    hr = 0;
fin:
    if (hr) {
        Py_CLEAR(future);
        future = NULL;
    }
    Py_XDECREF(dict);
    Py_XDECREF(input_body);
    return future;
}

// ASGI coro "send"
PyObject * asgi_send(PyObject * self, PyObject * dict)
{
    int hr = 0;
    asgi_t * asgi = (asgi_t *)self;
    client_t * client = asgi->client;
    PyObject * type = NULL;
    PyObject * status = NULL;
    PyObject * body = NULL;
    PyObject * future = NULL;

    update_log_prefix(client);
    LOGt("%s: .... client = %p", __func__, client->asgi);
    FIN_IF(!PyDict_Check(dict), -4570005);
    type = PyDict_GetItem(dict, g_cv.type);
    FIN_IF(!type, -4570011);
    FIN_IF(!PyUnicode_Check(type), -4570012);
    const char * evt_type = PyUnicode_AsUTF8(type);
    LOGi("%s: event type = '%s' ", __func__, evt_type);

    if (asgi->send.status == 0) {
        int rc = strcmp(evt_type, "http.response.start");
        FIN_IF(rc != 0, -4570021);
        asgi->send.status = -1;  // error
        status = PyDict_GetItem(dict, g_cv.status);
        FIN_IF(!status, -4570031);
        FIN_IF(!PyLong_Check(status), -4570032);
        int _status = (int)PyLong_AS_LONG(status);
        FIN_IF(_status < 100, -4570037);
        FIN_IF(_status > 999, -4570038);
        asgi->send.status = _status;
        int err = asgi_get_info_from_response(client, dict);
        if (err) {
            LOGc("response header 'Content-Length' contain incorrect value!");
            //err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            FIN(-4570047);
        }
        asgi->send.start_response = dict;
        Py_INCREF(dict);
        FIN(0);
    }
    if (strcmp(evt_type, "http.response.body") == 0) {
        if (asgi->send.latest_chunk) {
            LOGe("%s: body already readed from APP", __func__);
            FIN(0);
        }
        asgi->send.num_body++;
        SET_CSTATE(CS_RESP_BILD);

        body = PyDict_GetItem(dict, g_cv.body);
        if (!body)
            body = g_cv.empty_bytes;
        
        if (!PyBytes_CheckExact(body)) {
            LOGe("%s: body is not a bytes!!!", __func__);
            FIN(-4570111);
        }
        int64_t body_size = (int64_t)PyBytes_GET_SIZE(body);
        asgi->send.body_size += body_size;

        PyObject * more_body = PyDict_GetItem(dict, g_cv.more_body);
        bool latest_body = (more_body == Py_True) ? false : true;

        if (latest_body)
            asgi->send.latest_chunk = true;

        LOGi("%s: body size = %d, latest_body = %d, chunked = %d", __func__, (int)body_size, (int)latest_body, client->response.chunked);

        if (asgi->send.num_body == 1) {
            if (latest_body) {
                if (body_size == 0) {
                    // response without body
                    client->response.body_total_size = 0;
                    client->response.body_preloaded_size = 0;
                } else {
                    Py_INCREF(body);  // Reasone: "body" inserted into body chunks array
                    client->response.body[0] = body;
                    client->response.body_chunk_num = 1;
                    client->response.body_total_size = body_size;
                    client->response.body_preloaded_size = body_size;
                }
            } else {
                client->response.chunked = 1;
                LOGi("%s: chunked content transfer begins (unknown size of body)", __func__);
                client->response.body_chunk_num = 0;
            }
        }
        if (client->response.chunked) {
            if (body_size == 0 && !latest_body) {
                LOGd("%s: skip empty chunk", __func__);
                FIN(0);
            }
            if (body_size > 0) {
                if (client->response.body_chunk_num >= max_preloaded_body_chunks) {
                    LOGe("%s: too many ASGI body chunks (max = %d), closing connection", __func__, (int)max_preloaded_body_chunks);
                    FIN(-4570495);
                }
                Py_INCREF(body);  // Reason: "body" inserted into body chunks array
                client->response.body[client->response.body_chunk_num++] = body;
                client->response.body_preloaded_size += body_size;
                LOGd("%s: added chunk, size = %d", __func__, (int)body_size);
            }
            if (latest_body) {
                client->response.chunked = 2;
                if (body_size > 0) {
                    if (client->response.body_chunk_num >= max_preloaded_body_chunks) {
                        LOGe("%s: no room for footer chunk (max = %d)", __func__, (int)max_preloaded_body_chunks);
                        FIN(-4570496);
                    }
                    client->response.body[client->response.body_chunk_num++] = g_cv.footer_last_chunk;
                    Py_INCREF(g_cv.footer_last_chunk);  // Reason: "footer_last_chunk" inserted into body chunks array
                    //client->response.body_preloaded_size += PyBytes_GET_SIZE(g_cv.footer_last_chunk);
                }
            }
        }
        if (asgi->send.start_response) {
            int err = asgi_build_response(client);
            LOGe_IF(err, "%s: asgi_build_response return error = %d", __func__, err);
            FIN_IF(err, err);
            LOGi("Response created! (len = %d+%lld)", client->head.size, (long long)client->response.body_preloaded_size);
        }
        else if (client->response.chunked) {
            int csize = (int)client->response.body_preloaded_size;
            xbuf_reset(&client->head);
            char * buf = xbuf_expand(&client->head, 48);
            if (csize > 0) {
                client->head.size += sprintf(buf, "%X\r\n", csize);
            } else {
                client->head.size += sprintf(buf, "0\r\n\r\n");
            }
            client->response.headers_size = client->head.size;
            LOGd("%s: added chunk prefix, size = %d ", __func__, csize);
        }        
        int act = stream_write(client);
        if (act != CA_OK || client->state == CS_DESTROY) {
            FIN(-4570555);
        }
        future = PyObject_CallObject(g_srv.aio.loop.create_future, NULL);
        FIN_IF(!future, -4570601);
        asgi->send.future = future;
        Py_INCREF(future);
        FIN(0);
    }
    LOGe("%s: unsupported event type: '%s' ", __func__, evt_type);
    hr = -4570901;
fin:
    if (hr) {
        LOGe("%s: FIN WITH error = %d", __func__, hr);
        PyObject * error = PyErr_Format(PyExc_RuntimeError, "%s: error = %d", __func__, hr);
        if (client->state != CS_DESTROY) {
            SET_CSTATE(CS_DESTROY);
        }
        start_read_timer(client, 1);  // lazy call read_timer_cb
        return error;
    }
    return (future != NULL) ? future : self;
}

// ASGI callback "done"
PyObject * asgi_done(PyObject * self, PyObject * future)
{
    int hr = 0;
    asgi_t * asgi = (asgi_t *)self;
    client_t * client = asgi->client;
    PyObject * res = NULL;
    update_log_prefix(client);

    LOGt_IF(client, "%s: CURR STATE: rx.status = %s, cstate = %s, load_state = %d", __func__, get_rxstatus(client->rx.status), get_cstate(client->state), client->request.load_state);
    res = PyObject_CallMethodObjArgs(future, g_cv.result, NULL);
    if (res == NULL) {
        LOGe("%s: Error or Exception detected", __func__);
        PyErr_Clear();
    } else {
        LOGd("%s: result type = %s", __func__, Py_TYPE(res)->tp_name);
    }
    Py_XDECREF(res);  // release BEFORE stream_read_start
    LOGd("%s: RefCnt(asgi) = %d", __func__, (int)Py_REFCNT(self));
    hr = 0;
//fin:
    if (client) {
        client->asgi = NULL;  // equ asgi_free (end of request and response)
        if (res == NULL) {
            // Error or Exception detected
            SET_CSTATE(CS_DESTROY);
        }
        if (client->rx.status == RXS_FREEZED || client->state == CS_DESTROY) {
            start_read_timer(client, 1);  // lazy call read_rxbuf_after_send
            Py_RETURN_NONE;
        } else {
            stream_read_start(client);  // continue reading from TCP socket (next request from client)
        }
    }
    Py_RETURN_NONE;
}

PyObject * asgi_await(PyObject * self)
{
    Py_INCREF(self);
    return self;
}

void asgi_dealloc(asgi_t * self)
{
    asgi_t * asgi = (asgi_t *)self;
    LOGd("%s: RefCnt(asgi) = %d, RefCnt(task) = %d,", __func__, (int)Py_REFCNT(self), self->task ? (int)Py_REFCNT(self->task) : -999);
    Py_CLEAR(self->scope);
    Py_CLEAR(self->recv.future);
    Py_CLEAR(self->send.future);
    Py_CLEAR(self->send.start_response);
    Py_CLEAR(self->task);
    PyObject_Del(self);
}

PyObject * asgi_iter(PyObject * self)
{
    Py_INCREF(self);
    return self;
}

PyObject * asgi_next(PyObject * self)
{
    return NULL;
}

// -----------------------------------------------------------------------------------

static PyMethodDef asgi_methods[] = {
    { "receive", asgi_receive, METH_NOARGS, 0 },
    { "send",    asgi_send,    METH_O,      0 },
    { "done",    asgi_done,    METH_O,      0 },
    { NULL,      NULL,         0,           0 }
};

static PyAsyncMethods asgi_async_methods = {
    .am_await = asgi_await
};

PyTypeObject ASGI_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "ASGI",
    .tp_basicsize = sizeof(asgi_t),
    .tp_dealloc   = (destructor) asgi_dealloc,
    .tp_as_async  = &asgi_async_methods,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_iter      = asgi_iter,
    .tp_iternext  = asgi_next,
    .tp_methods   = asgi_methods,
    .tp_finalize  = NULL
};

