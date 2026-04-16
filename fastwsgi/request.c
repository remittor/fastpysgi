#include "server.h"
#include "request.h"
#include "llhttp.h"
#include "constants.h"
#include "start_response.h"
#include "pyhacks.h"


static
INLINE
int xbuf_add_HDR(xbuf_t * buf, const void * data, Py_ssize_t size)
{
    int beg = buf->size;
    if (size <= 0) {
        return beg;
    }
    int end = xbuf_add(buf, data, (size_t)size);
    if (g_srv.resp_hdr_lower && end > beg) {
        for (int pos = beg; pos < end; pos++) {
            buf->data[pos] = ASCII_TO_LOWER(buf->data[pos]);
        }
    }
    return end;
}

static
INLINE
int xbuf_add_hdr(xbuf_t * buf, const char * str)
{
    return xbuf_add_HDR(buf, str, strlen(str));
}

static
int set_header(client_t * client, PyObject * key, const char * value, ssize_t length, int flags)
{
    int hr = 0;
    ssize_t vlen = (length >= 0) ? length : (ssize_t)strlen(value);
    LOGi("set_header: %s = '%.*s'", PyBytes_Check(key) ? PyBytes_AS_STRING(key) : PyUnicode_AsUTF8(key), (int)vlen, value);
    PyObject * dict = NULL;
    PyObject * kname = NULL;
    PyObject * val = NULL;
    PyObject * val_aux = NULL;   // persent decoded UTF-8 string
    PyObject * asgi_scope = (client->asgi != NULL) ? client->asgi->scope : NULL;
    if (key == g_cv.PATH_INFO) {
        void * pbackslash = (vlen > 0) ? memchr(value, '\\', vlen) : NULL;
        FIN_IF(pbackslash, -41);  // not valid character
        void * penc = (vlen > 0) ? memchr(value, '%', vlen) : NULL;
        if (penc) {
            char * buf = (char *)value;  // tiny hack
            int skip2F = 1;
            ssize_t new_len = uri_percent_decode_inplace(buf, vlen, skip2F, 100);
            FIN_IF(new_len < 0, -45);
            vlen = new_len;
        }
        if (asgi_scope) {
            val = PyBytes_FromStringAndSize(value, vlen);
            FIN_if(!val, -40, PyErr_Clear());
            if (penc == NULL) {
                val_aux = PyUnicode_DecodeLatin1(value, vlen, "strict");  // without unicode symbols
            } else {
                val_aux = PyUnicode_DecodeUTF8(value, vlen, "strict");  // maybe with unicode symbols
            }
            FIN_if(!val_aux, -55, PyErr_Clear());
            hr = PyDict_SetItem(asgi_scope, g_cv.path, val_aux);
            FIN_if(hr, -56, PyErr_Clear());
        } else {
            dict = client->request.headers;
            server_t * server = client->server;
            if (server->root_path.len > 0) {
                FIN_IF(vlen < (ssize_t)server->root_path.len, HTTP_STATUS_NOT_FOUND); // 404
                int rc = memcmp(value, server->root_path.str, vlen);
                FIN_IF(rc != 0, HTTP_STATUS_NOT_FOUND); // 404
                if (vlen == (ssize_t)server->root_path.len) {
                    hr = PyDict_SetItem(dict, g_cv.PATH_INFO, g_cv.slash);
                    FIN_if(hr, -66, PyErr_Clear());
                    FIN(0);
                }
                FIN_IF(value[server->root_path.len] != '/', HTTP_STATUS_NOT_FOUND); // 404
                value += server->root_path.len;
                vlen -= server->root_path.len;
            }
            val = PyUnicode_DecodeUTF8(value, vlen, "strict");  // maybe with unicode symbols
            FIN_if(!val, -68, PyErr_Clear());
        }
    }
    if (asgi_scope) {
        dict = asgi_scope;
        if (key == g_cv.QUERY_STRING) {
            kname = g_cv.query_string;
        }
        else if (key == g_cv.REQUEST_METHOD) {
            kname = g_cv.method;
            val = PyUnicode_FromStringAndSize(value, vlen);  // as UTF-8
        }
        else if (key == g_cv.REMOTE_ADDR) {
            kname = g_cv.REMOTE_ADDR;  // FIXME: set "client"
        }
        if (!val) {
            val = PyBytes_FromStringAndSize(value, vlen);
            FIN_IF(!val, -78);
        }
        if (!kname) {
            // only for "scope.headers"
            if (key == g_cv.CONTENT_LENGTH) {
                key = g_cv.content_length;
            }
            PyObject * scope_headers = PyDict_GetItem(asgi_scope, g_cv.headers);
            if (!scope_headers) {
                scope_headers = PyList_New(0);
                PyDict_SetItem(asgi_scope, g_cv.headers, scope_headers);
                Py_DECREF(scope_headers);
            }
            if (!PyBytes_Check(key)) {
                const char * keystr = PyUnicode_AsUTF8(key);
                FIN_if(!keystr, -91, PyErr_Clear());
                kname = PyBytes_FromString(keystr);
                FIN_if(!kname, -92, PyErr_Clear());
            }
            PyObject * tup = PyTuple_Pack(2, (kname != NULL) ? kname : key, val);
            FIN_if(!tup, -93, PyErr_Clear());
            Py_XDECREF(kname);
            PyList_Append(scope_headers, tup);
            Py_DECREF(tup);
            FIN(0);
        }
    } else {
        dict = client->request.headers;
        kname = key;
        if (key == g_cv.PATH_INFO) {
            FIN_IF(!val, -95);
        }
        else if (key == g_cv.QUERY_STRING) {
            val = PyUnicode_DecodeLatin1(value, vlen, "strict");
            FIN_if(!val, -96, PyErr_Clear());
        }
        if (!val) {
            val = PyUnicode_DecodeUTF8(value, vlen, "ignore");  // as UTF-8
            FIN_if(!val, -97, PyErr_Clear());
        }
    }
    FIN_IF(!dict, -3);
    FIN_IF(!kname, -4);
    FIN_IF(!val, -5);
    hr = PyDict_SetItem(dict, kname, val);
    FIN_if(hr, -6, PyErr_Clear());
fin:
    Py_XDECREF(val);
    Py_XDECREF(val_aux);
    return hr;
}

static 
int set_header_v(client_t * client, const char * key, const char * value, ssize_t length, int flags)
{
    if (!key)
        return -11;
    size_t klen = strlen(key);
    if (klen == 0)
        return -12;
    PyObject * pkey;
    if (client->asgi) {
        pkey = PyBytes_FromStringAndSize(key, klen);
    } else {
        pkey = PyUnicode_DecodeLatin1(key, klen, "ignore");
    }
    int retval = set_header(client, pkey, value, length, flags);
    Py_DECREF(pkey);
    return retval;
}

void close_iterator(PyObject * iterator)
{
    if (iterator != NULL && PyIter_Check(iterator)) {
        PyObject * close = PyObject_GetAttrString(iterator, "close");
        if (close != NULL) {
            PyObject * close_result = PyObject_CallObject(close, NULL);
            Py_XDECREF(close_result);
            Py_XDECREF(close);
        }
    }
}

void reset_head_buffer(client_t * client)
{
    client->request.current_key_len = 0;
    client->request.current_val_len = 0;
    xbuf_reset(&client->head);
    client->response.headers_size = 0;
}

static
int reset_wsgi_input(client_t * client)
{
    if (client->request.wsgi_input_size > 1*1024*1024) {
        // Always free huge buffers for incoming data
        Py_CLEAR(client->request.wsgi_input);
    }
    client->request.wsgi_input_size = 0;
    return 0;
}

void free_start_response(client_t * client)
{
    Py_CLEAR(client->start_response);
}

void reset_response_preload(client_t * client)
{
    size_t chunks = client->response.body_chunk_num;
    if (chunks || client->response.wsgi_body) {
        LOGt("%s: chunks = %d, wsgi_body = %p", __func__, (int)chunks, client->response.wsgi_body);
    }
    for (size_t i = 0; i < chunks; i++) {
        Py_XDECREF(client->response.body[i]);
    }
    client->response.body_chunk_num = 0;
    client->response.body_preloaded_size = 0;
}

void reset_response_body(client_t * client)
{
    reset_response_preload(client);
    client->response.body_total_size = 0;

    if (client->response.wsgi_body) {
        if (client->response.body_iterator == client->response.wsgi_body) {
            // ClosingIterator.close() or FileWrapper.close()
            close_iterator(client->response.wsgi_body);
        } else {
            Py_CLEAR(client->response.body_iterator);
        }
    }
    Py_CLEAR(client->response.wsgi_body);
    client->response.body_iterator = NULL;
    client->response.body_total_written = 0;
    client->response.chunked = 0;
}

// ============== request processing ==================================================

int on_message_begin(llhttp_t * parser)
{
    LOGi("on_message_begin: ------------------------------");
    client_t * client = (client_t *)parser->data;
    client->request.load_state = LS_MSG_BEGIN;
    if (client->head.data == NULL)
        xbuf_init2(&client->head, client->buf_head_prealloc, sizeof(client->buf_head_prealloc));
    //client->request.keep_alive = 0;
    client->error = 0;
    if (client->state == CS_RESP_SEND) {
        client->error = 1;
        LOGc("Received new HTTP request while sending response! Disconnect client!");
        return -1;
    }
    if (!g_srv.asgi_app) {
        Py_CLEAR(client->request.headers);  // wsgi_input: refcnt 2 -> 1
        // Sets up base request dict for new incoming requests
        // https://www.python.org/dev/peps/pep-3333/#specification-details
        client->request.headers = PyDict_Copy(client->server->def_env);
    }
    client->request.http_content_length = -1; // not specified
    client->request.chunked = 0;
    client->request.expect_continue = 0;
    client->request.headers_num = 0;
    Py_CLEAR(client->request.host);
    reset_wsgi_input(client);
    reset_head_buffer(client);
    free_start_response(client);
    reset_response_body(client);
    client->response.wsgi_content_length = -1;
    if (g_srv.asgi_app) {
        asgi_init(client);
    }
    return 0;
}

int on_url(llhttp_t * parser, const char * data, size_t length)
{
    LOGd("%s: (len = %d)", __func__, (int)length);
    if (length > 0) {
        client_t * client = (client_t *)parser->data;
        if (client->head.size + length >= 16*1024) {
            client->error = HTTP_STATUS_URI_TOO_LONG;
            return -1;  // 414 URI Too Long
        }
        xbuf_add(&client->head, data, length);
    }
    return 0;
}

int on_url_complete(llhttp_t * parser)
{
    int hr = 0;
    client_t * client = (client_t *)parser->data;
    client->request.load_state = LS_MSG_URL;
    xbuf_t * buf = &client->head;
    LOGi("%s: \"%s\"", __func__, buf->data);
    char * path = buf->data;
    size_t path_len = (size_t)buf->size;
    FIN_if(memchr(path, '#', path_len), -1, client->error = HTTP_STATUS_BAD_REQUEST);
    char * query = (char *)memchr(path, '?', path_len);
    size_t query_len = 0;
    if (query) {
        path_len = (size_t)query - (size_t)path;
        query[0] = 0;  // change '?' to '\0'
        query++;       // fix ptr for QUERY_STRING
        query_len = (size_t)buf->size - path_len - 1;
    }
    if (path_len == 0) {
        LOGe("%s: empty PATH", __func__);
        client->error = HTTP_STATUS_BAD_REQUEST; // 400
        FIN(-1);
    } else {
        FIN_if(parser->method != HTTP_OPTIONS && memchr(path, '*', path_len), -1, client->error = HTTP_STATUS_BAD_REQUEST);
        hr = set_header(client, g_cv.PATH_INFO, path, path_len, 0);
        LOGc_IF(hr, "%s: malformed PATH = \"%s\"", __func__, path);
        FIN_if(hr, -1, client->error = (hr < 0) ? HTTP_STATUS_BAD_REQUEST : hr);
    }
    if (query_len > 0) {
        hr = set_header(client, g_cv.QUERY_STRING, query, query_len, 0);
        LOGc_IF(hr, "%s: malformed QUERY = \"%s\"", __func__, query);
        FIN_if(hr, -1, client->error = (hr < 0) ? HTTP_STATUS_BAD_REQUEST : hr);
    }
fin:
    reset_head_buffer(client);
    return hr;
}

int on_header_field(llhttp_t * parser, const char * data, size_t length)
{
    LOGd_IF(length == 0, "%s: <empty>", __func__);
    if (length == 0)
        return 0;

    client_t * client = (client_t *)parser->data;
    client->request.headers_num++;
    if (parser->http_major != 1 || parser->http_minor != 1) {
        LOGe("%s: recived protocol version = %d.%d (expected: v1.1)", __func__, parser->http_major, parser->http_minor);
        client->error = HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED;
        return -1;  // 505 HTTP Version Not Supported
    }
    LOGd("%s: '%.*s'", __func__, (int)length, data);
    if (client->request.headers_num >= 10000) {
        client->error = HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE;
        return -1;  // 431 Request Header Fields Too Large
    }
    if (client->head.size + length >= 32*1024) {
        client->error = HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE;
        return -1;  // 431 Request Header Fields Too Large
    }
    if (client->request.current_key_len == 0 && !client->asgi) {
        xbuf_add(&client->head, "HTTP_", 5);
        client->request.current_key_len = 5;
    }
    xbuf_add(&client->head, data, length);
    client->request.current_key_len += length;
    client->request.current_val_len = 0;
    return 0;
}

int on_header_field_complete(llhttp_t * parser)
{
    client_t * client = (client_t *)parser->data;
    xbuf_t * buf = &client->head;
    char * data = buf->data;
    ssize_t size = buf->size;
    ssize_t prefix_len = (client->asgi) ? 0 : 5;  // prefix "HTTP_"
    if (size <= prefix_len) {
        LOGi("%s: Unnamed field!", __func__);        
        goto fin;
    }
    LOGi("%s: %s", __func__, data + prefix_len);
    for (ssize_t i = prefix_len; i < size; i++) {
        const char symbol = data[i];
        if (symbol == '_') {  // CVE-2015-0219 
            xbuf_reset(buf);
            client->request.current_key_len = 0;
            client->request.current_val_len = 0;
            return 0;  // skip incorrect header
        }
        if ((unsigned char)symbol >= 127) {
            continue;
        }
        if (client->asgi) {
            // ASGI
            if (g_srv.aio.req_hdr_lower) {
                data[i] = ASCII_TO_LOWER(symbol);
            }
        } else {
            // WSGI
            if (symbol == '-') {
                data[i] = '_';
                continue;
            }
            data[i] = ASCII_TO_UPPER(symbol);
        }
    }
fin:
    xbuf_add(buf, "\0", 1);  // add empty value
    client->request.current_val_len = 0;
    return 0;
}

int on_header_value(llhttp_t * parser, const char * data, size_t length)
{
    LOGd_IF(length == 0, "%s: <empty>", __func__);
    if (length == 0)
        return 0;

    LOGd("%s: '%.*s'", __func__, (int)length, data);
    client_t * client = (client_t *)parser->data;
    xbuf_t * buf = &client->head;
    if (client->request.current_val_len == 0) {
        if (client->request.current_key_len == 0) {
            return 0;  // skip incorrect header
        }
        if (client->request.current_key_len + 1 != (size_t)buf->size) {
            client->error = 1;
            LOGc("%s: internal error (1)", __func__);
            return -1;  // critical error
        }
    }
    if (buf->size + length >= 32*1024) {
        client->error = HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE;
        return -1;  // 431 Request Header Fields Too Large
    }
    xbuf_add(buf, data, length);
    client->request.current_val_len += length;
    return 0;
}

typedef enum {
    HN_UNKNOWN           = 0,
    HN_CONTENT_LENGTH    = 1,
    HN_CONTENT_TYPE      = 2,
    HN_TRANSFER_ENCODING = 3,
    HN_EXPECT            = 4,
    HN_HOST              = 5,
    HN__MAX
} header_name_t;

int on_header_value_complete(llhttp_t * parser)
{
    int hr = -1;
    client_t * client = (client_t *)parser->data;
    xbuf_t * buf = &client->head;
    size_t key_len = client->request.current_key_len;
    size_t val_len = client->request.current_val_len;
    char * key = buf->data;
    char * val = buf->data + key_len + 1;
    LOGi("%s: '%s'", __func__, val);
    size_t prefix_len = (client->asgi) ? 0 : 5;
    if (key_len <= prefix_len) {
        LOGw("%s: Headers has an unnamed value!", __func__);        
        return 0;  // skip incorrect header
    }
    header_name_t hname = HN_UNKNOWN;
    if (client->asgi) {
        if (key_len == 14 && strcasecmp(key, "CONTENT-LENGTH") == 0)
            hname = HN_CONTENT_LENGTH;
        else if (key_len == 12 && strcasecmp(key, "CONTENT-TYPE") == 0)
            hname = HN_CONTENT_TYPE;
        else if (key_len == 17 && strcasecmp(key, "TRANSFER-ENCODING") == 0)
            hname = HN_TRANSFER_ENCODING;
        else if (key_len == 6 && strcasecmp(key, "EXPECT") == 0)
            hname = HN_EXPECT;
        else if (key_len == 4 && strcasecmp(key, "HOST") == 0)
            hname = HN_HOST;
    } else {
        if (key_len == 19 && strncmp(key, "HTTP_CONTENT_LENGTH", 19) == 0)
            hname = HN_CONTENT_LENGTH;
        else if (key_len == 17 && strncmp(key, "HTTP_CONTENT_TYPE", 17) == 0)
            hname = HN_CONTENT_TYPE;
        else if (key_len == 22 && strncmp(key, "HTTP_TRANSFER_ENCODING", 22) == 0)
            hname = HN_TRANSFER_ENCODING;
        else if (key_len == 11 && strncmp(key, "HTTP_EXPECT", 11) == 0)
            hname = HN_EXPECT;
        else if (key_len == 9 && strncmp(key, "HTTP_HOST", 9) == 0)
            hname = HN_HOST;
    }
    if (hname == HN_UNKNOWN) {
        // nothing
    }
    else if (hname == HN_CONTENT_LENGTH) {
        client->request.http_content_length = 0; // field "Content-Length" present
        key += prefix_len;  // exclude prefix "HTTP_"
    }
    else if (hname == HN_CONTENT_TYPE) {
        key += prefix_len;  // exclude prefix "HTTP_"
    }
    else if (hname == HN_TRANSFER_ENCODING) {
        if (val_len == 7 && strncmp(val, "chunked", 7) == 0) {
            client->request.chunked = 1;
        } else {
            client->error = 1;
            LOGc("%s: Header \"Transfer-Encoding\" contain unsupported value = '%s'", __func__, val);
            // https://peps.python.org/pep-3333/#other-http-features
            LOGc("%s: FastPySGI cannot decompress content!", __func__);
            return -1;  // critical error
        }
        key = NULL;  // skip chunked flag
        // Let's skip the chunks flag, since the FastPySGI server completely downloads the body into memory
        // and immediately gives body to the WSGI application.
    }
    else if (hname == HN_EXPECT) {
        if (val_len != 12 || strncmp(val, "100-continue", 12) != 0) {
            client->error = HTTP_STATUS_EXPECTATION_FAILED;  // 417
            LOGc("%s: Header \"Expect\" contain unsupported value = '%s'", __func__, val);
            // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Expect
            return -1;  // critical error
        }
        client->request.expect_continue = 1;
        key = NULL;  // hide Expect header
    }
    if (key) {
        hr = set_header_v(client, key, val, val_len, 0);
        LOGc_IF(hr, "%s: malformed header \"%s\" = \"%.*s\"", __func__, key, (int)val_len, val);
        FIN_if(hr, -1, client->error = (hr < 0) ? HTTP_STATUS_BAD_REQUEST : hr);
        if (hname == HN_HOST) {
            FIN_if(client->request.host, -1, client->error = HTTP_STATUS_BAD_REQUEST); // DUPLICATE-HOST
            FIN_if(val_len == 0, -1, client->error = HTTP_STATUS_BAD_REQUEST);  // empty value
            FIN_if(memchr(val, ',', val_len), -1, client->error = HTTP_STATUS_BAD_REQUEST);  // detect list
            FIN_if(memchr(val, '@', val_len), -1, client->error = HTTP_STATUS_BAD_REQUEST);  // detect username
            FIN_if(memchr(val, '/', val_len), -1, client->error = HTTP_STATUS_BAD_REQUEST);  // detect slash
            client->request.host = PyUnicode_DecodeUTF8(val, val_len, "ignore");  // as UTF-8
            FIN_if(!client->request.host, -999, PyErr_Clear());
        }
    }
    reset_head_buffer(client);
    hr = 0;
fin:
    return hr;
}

int on_headers_complete(llhttp_t * parser)
{
    client_t * client = (client_t *)parser->data;
    client->request.load_state = LS_MSG_HEADERS;
    if (!client->request.host) {
        LOGe("%s: header HOST not present! => 400", __func__);
        client->error = HTTP_STATUS_BAD_REQUEST;
        return -1;  // 400
    }
    uint64_t clen = parser->content_length;
    LOGi("%s: %s", __func__, (client->request.chunked) ? "(chunked)" : "");
    reset_head_buffer(client);
    if (clen > g_srv.max_content_length) {
        LOGc("Received HTTP headers with \"Content-Length\" = %llu (expected <= %llu)", clen, g_srv.max_content_length);
        if (client->request.expect_continue) {
            client->error = HTTP_STATUS_EXPECTATION_FAILED;
            return 1; // Assume that request has no body, and proceed to parsing the next message!
        }
        client->error = 1;
        return -1;  // error
    }
    if (client->request.http_content_length >= 0) {
        client->request.http_content_length = clen;
    }
    if (client->request.expect_continue) {
        int rc = x_send_status(client, HTTP_STATUS_CONTINUE);
        if (rc < 0) {
            client->error = 1;
            return -1;  // abort parsing
        }
        client->request.expect_continue = 0;
    }
    return 0;
}

int on_body(llhttp_t * parser, const char * body, size_t length)
{
    LOGi("%s: len = %d", __func__, (int)length);
    client_t * client = (client_t *)parser->data;
    client->request.load_state = LS_MSG_BODY;
    if (length == 0)
        return 0;

    PyObject* wsgi_input = client->request.wsgi_input;
    if (wsgi_input == NULL) {
        wsgi_input = PyObject_CallMethodObjArgs(g_cv.module_io, g_cv.BytesIO, NULL);
        client->request.wsgi_input = wsgi_input;  // object cached
        client->request.wsgi_input_size = 0;
    }
    else if (client->request.wsgi_input_size == 0) {
        PyObject* result = PyObject_CallMethodObjArgs(wsgi_input, g_cv.seek, g_cv.i0, NULL);
        Py_XDECREF(result);
    }
    uint64_t clen = (uint64_t)client->request.wsgi_input_size + length;
    if (clen > g_srv.max_content_length) {
        client->error = 1;
        LOGc("Received too large body of HTTP request: size = %llu (expected <= %llu)", clen, g_srv.max_content_length);
        return -1;  // critical error
    }
    
    bytesio_t * bio = get_bytesio_object(wsgi_input);
    if (bio) {
        Py_ssize_t rv = io_BytesIO_write_bytes(bio, body, length);
        if (rv < 0 || rv != (Py_ssize_t)length) {
            client->error = 1;
            LOGf("Failed write bytes directly to wsgi_input Stream! (ret = %d, expected = %d)", (int)rv, (int)length);
        }
        goto fin;
    }

    PyObject* body_content = PyBytes_FromStringAndSize(body, length);
    if (client->request.wsgi_input_size == 0) {
        LOGREPR(LL_TRACE, body_content);  // output only first chunk
    }
    PyObject* result = PyObject_CallMethodObjArgs(wsgi_input, g_cv.write, body_content, NULL);
    if (!result || !PyLong_CheckExact(result)) {
        client->error = 1;
        LOGf("Cannot write PyBytes to wsgi_input stream! (ret = None)");
    } else {
        size_t rv = PyLong_AsSize_t(result);
        if (rv != length) {
            client->error = 1;
            LOGf("Failed write PyBytes to wsgi_input stream! (ret = %d, expected = %d)", (int)rv, (int)length);
        }
    }
    Py_XDECREF(result);
    Py_XDECREF(body_content);

fin:
    client->request.wsgi_input_size += length;
    return (client->error) ? -1 : 0;
}

int on_message_complete(llhttp_t * parser)
{
    int hr = -1;
    LOGi("%s", __func__);
    client_t * client = (client_t *)parser->data;
    client->request.load_state = LS_MSG_END;

    if (llhttp_should_keep_alive(parser)) {
        client->request.keep_alive = 1;
    } else {
        client->request.keep_alive = 0;
    }
    if (client->error) {
        if (client->request.expect_continue && client->error == HTTP_STATUS_EXPECTATION_FAILED)
            return HPE_PAUSED;
        return -1;
    }

    if (client->request.http_content_length >= 0) {
        const int64_t clen = client->request.http_content_length;
        const int64_t ilen = client->request.wsgi_input_size;
        if (ilen != clen) {
            client->error = 1;
            LOGc("Received body with size %lld not equal specified 'Content-Length' = %lld", (long long)ilen, (long long)clen);
            return -1;
        }
    }

    PyObject * wsgi_input = NULL;
    if (client->request.wsgi_input_size > 0) {
        PyObject* result;
        wsgi_input = client->request.wsgi_input;
        // Truncate input byte stream to real request content length
        result = PyObject_CallMethodObjArgs(wsgi_input, g_cv.truncate, NULL);
        Py_XDECREF(result);
        // Sets the input byte stream position back to 0
        result = PyObject_CallMethodObjArgs(wsgi_input, g_cv.seek, g_cv.i0, NULL);
        Py_XDECREF(result);
    } else {
        client->request.wsgi_input_size = 0;
        if (client->request.wsgi_input_empty == NULL) {
            wsgi_input = PyObject_CallMethodObjArgs(g_cv.module_io, g_cv.BytesIO, NULL);
            client->request.wsgi_input_empty = wsgi_input;  // object cached
        } else { 
            wsgi_input = client->request.wsgi_input_empty;
        }
    }
    if (client->request.headers) {
        PyDict_SetItem(client->request.headers, g_cv.wsgi_input, wsgi_input); // wsgi_input: refcnt 1 -> 2
    }
    //LOGd("build_wsgi_environ");
    char buf[128];
    
    const char* method = llhttp_method_name(parser->method);
    set_header(client, g_cv.REQUEST_METHOD, method, -1, 0);
    FIN_if(parser->method == HTTP_CONNECT, -1, client->error = HTTP_STATUS_NOT_IMPLEMENTED);  // not supported => 501

    if (client->remote_addr[0])
        set_header(client, g_cv.REMOTE_ADDR, client->remote_addr, -1, 0);

    if (client->request.chunked && client->request.http_content_length < 0) {
        sprintf(buf, "%lld", (long long)client->request.wsgi_input_size);
        set_header(client, g_cv.CONTENT_LENGTH, buf, -1, 0);
        // Insertion of this header is allowed because the header "Transfer-Encoding" FastPySGI server removes!
        // https://peps.python.org/pep-3333/#other-http-features
    }

    client->request.load_state = LS_OK;
    hr = HPE_PAUSED;
fin:
    return hr;
}

int on_reset(llhttp_t * parser)
{
    // This callback called only on HTTP pipelining
    //LOGi("%s: Detect HTTP pipelining!", __func__);
    return 0;
}

// =================== call WSGI app =============================================

int call_wsgi_app(client_t * client)
{
    PyObject * headers = client->request.headers;
    
    StartResponse * start_response = create_start_response();
    client->start_response = start_response;

    LOGi("calling wsgi application...");
    PyObject * wsgi_body = PyObject_CallFunctionObjArgs(g_srv.wsgi_app, headers, start_response, NULL);
    LOGi("wsgi app return object: '%s'", (wsgi_body) ? Py_TYPE(wsgi_body)->tp_name : "");
    client->response.wsgi_body = wsgi_body;

    if (PyErr_Occurred() || wsgi_body == NULL) {
        return HTTP_STATUS_INTERNAL_SERVER_ERROR;
    }
    return 0;
}

// =================== build response ============================================

int get_info_from_wsgi_response(client_t * client);
int wsgi_body_pleload(client_t * client, PyObject * wsgi_body);

int process_wsgi_response(client_t * client)
{
    int err = 0;
    client->response.wsgi_content_length = -1;
    PyObject * wsgi_body = client->response.wsgi_body;

    const char* body_type = Py_TYPE(wsgi_body)->tp_name;
    if (body_type == NULL)
        body_type = "<unknown_type_name>";

    if (PyBytes_CheckExact(wsgi_body)) {
        LOGd("wsgi_body: is PyBytes (size = %d)", (int)PyBytes_GET_SIZE(wsgi_body));
    } else {
        if (PyGen_Check(wsgi_body)) {
            LOGd("wsgi_body: is GENERATOR '%s'", body_type);
            /*
            * If the application we called was a generator, we have to call .next() on
            * it before we do anything else because that may execute code that
            * invokes `start_response` (which might not have been invoked yet).
            * Think of the following scenario:
            *
            *     def app(environ, start_response):
            *         start_response('200 Ok', ...)
            *         yield 'Hello World'
            */
            client->response.body_iterator = wsgi_body;
            PyObject * item = wsgi_iterator_get_next_chunk(client, 1);
            if (!item && client->error) {
                err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
                goto fin;
            }                
            Py_ssize_t size = (item == NULL) ? 0 : PyBytes_GET_SIZE(item);
            client->response.body[0] = item;
            client->response.body_chunk_num = (item == NULL) ? 0 : 1;
            client->response.body_total_size = -111;  // generator
            client->response.body_preloaded_size = size;
        }
        else if (PyIter_Check(wsgi_body)) {
            LOGd("wsgi_body: is ITERATOR '%s'", body_type);
            client->response.body_iterator = wsgi_body;
        } else {
            LOGd("wsgi_body: type = '%s'", body_type);
            PyObject * iter = PyObject_GetIter(wsgi_body);
            if (iter == NULL) {
                PYTYPE_ERR("wsgi_body: has not iterable type = '%s'", body_type);
                err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
                goto fin;
            }
            client->response.body_iterator = iter;
        }
    }
    err = get_info_from_wsgi_response(client);
    if (err) {
        LOGc("response header 'Content-Length' contain incorrect value!");
        PYTYPE_ERR("response headers contain incorrect value!");
        err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        goto fin;
    }
    err = wsgi_body_pleload(client, wsgi_body);
    if (err < 0) {
        LOGc("wsgi_body_pleload return error = %d", err);
        err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        goto fin;
    }
    err = 0;

fin:
    if (err) {
        reset_head_buffer(client);
        reset_response_body(client);
        free_start_response(client);
        Py_CLEAR(client->request.headers);
        return err;
    }
    return 0;
}

int create_response(client_t * client)
{
    int err = 0;
    LOGi("%s", __func__);
    int flags = RF_HEADERS_WSGI;
    if (client->request.keep_alive)
        flags |= RF_SET_KEEP_ALIVE;

    int len = build_response(client, flags, 0, client->start_response, NULL, -1);
    if (len <= 0) {
        err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
    }
    if (err) {
        reset_head_buffer(client);
        reset_response_body(client);
    }
    free_start_response(client);
    Py_CLEAR(client->request.headers);
    return err;
}

int build_response(client_t * client, int flags, int status, const void * headers, const void * body_data, int _body_size)
{
    int hr = 0;
    xbuf_t * head = &client->head;
    reset_head_buffer(client);
    PyObject** body = client->response.body;
    StartResponse * response = NULL;
    PyObject * asgi_dict = NULL;
    int64_t body_size = _body_size;
    bool resp_date_present = false;
    bool resp_server_present = false;

    if (flags & RF_HEADERS_WSGI) {
        response = (StartResponse *)headers;
        headers = NULL;
        char scode[4];
        Py_ssize_t status_len = 0;
        const char * status_code = PyUnicode_AsUTF8AndSize(response->status, &status_len);
        FIN_IF(status_len < 3, -2);
        memcpy(scode, status_code, 4);
        scode[3] = 0;
        status = atoi(scode);
    }
    else if (flags & RF_HEADERS_ASGI) {
        asgi_dict = (PyObject *)headers;
        headers = NULL;
        if (status == 0) {
            PyObject * _status = PyDict_GetItem(asgi_dict, g_cv.status);
            FIN_IF(!_status, -2);
            status = (int)PyLong_AsLong(_status);
        }
    }
    if (status == 204 || status == 304) {
        body_size = 0;
        reset_response_body(client);  // forced reset body buffers
    }

    const char * status_name = get_http_status_name(status);
    FIN_IF(!status_name, -3);

    char * buf = xbuf_expand(head, 128);
    head->size += sprintf(buf, "HTTP/1.1 %d %s\r\n", status, status_name);

    if (response || asgi_dict) {
        PyObject * asgi_headers = NULL;
        PyObject * iterator = NULL;
        Py_ssize_t hsize = 0;
        if (response) {
            FIN_IF(!response->headers, -4);
            hsize = PyList_GET_SIZE(response->headers);
        } else {
            asgi_headers = PyDict_GetItem(asgi_dict, g_cv.headers);
            FIN_IF(!asgi_headers, -4);
            iterator = PyObject_GetIter(asgi_headers);
            FIN_IF(!iterator, -5);
        }
        PyObject * item = NULL;
        for (Py_ssize_t i = 0; /* nothing */ ; i++) {
            const char * key = NULL;
            Py_ssize_t key_len = 0;
            const char * val = NULL;
            Py_ssize_t val_len = 0;
            if (asgi_dict) {
                Py_XDECREF(item);
                item = PyIter_Next(iterator);
                if (!item)
                    break;

                key_len = asgi_get_data_from_header(item, 0, &key);
                val_len = asgi_get_data_from_header(item, 1, &val);
                if (key_len < 0 || val_len < 0) {
                    hr = -59;
                    break;
                }
            } else {
                if (i >= hsize)
                    break;

                PyObject * tuple = PyList_GET_ITEM(response->headers, i);
                key = PyUnicode_AsUTF8AndSize(PyTuple_GET_ITEM(tuple, 0), &key_len);
                val = PyUnicode_AsUTF8AndSize(PyTuple_GET_ITEM(tuple, 1), &val_len);
            }

            if (key_len == 14 && key[7] == '-')
                if (strcasecmp(key, "Content-Length") == 0)
                    continue;  // skip "Content-Length" header

            if (key_len == 10)
                if (strcasecmp(key, "Connection") == 0)
                    continue;  // skip "Connection" header

            if (key_len == 4)
                if (strcasecmp(key, "Date") == 0)
                    resp_date_present = true;

            bool is_header_server = false;
            if (key_len == 6)
                if (strcasecmp(key, "Server") == 0) {
                    is_header_server = true;
                    resp_server_present = true;
                }

            xbuf_add_HDR(head, key, key_len);
            xbuf_add(head, ": ", 2);
            if (is_header_server && g_srv.add_header_server > 0) {
                xbuf_add(head, g_srv.header_server, g_srv.add_header_server);
                xbuf_add(head, " ", 1);
            }
            xbuf_add(head, val, val_len);
            xbuf_add(head, "\r\n", 2);

            LOGi("added header '%s: %s'", key, val);
        }
        Py_XDECREF(item);
        Py_XDECREF(iterator);
        FIN_IF(hr, hr);
    }
    else if (headers) {
        xbuf_add_str(head, (const char *)headers);
    }

    if (!resp_date_present && g_srv.add_header_date) {
        char * date_str;
        int date_len = get_asctime(&date_str);
        xbuf_add_HDR(head, "Date: ", 6);
        xbuf_add(head, date_str, date_len);
        xbuf_add(head, "\r\n", 2);
    }
    if (!resp_server_present && g_srv.add_header_server > 0) {
        xbuf_add_HDR(head, "Server: ", 8);
        xbuf_add(head, g_srv.header_server, g_srv.add_header_server);
        xbuf_add(head, "\r\n", 2);
    }

    if ((flags & RF_SET_KEEP_ALIVE) != 0 && g_srv.allow_keepalive) {
        xbuf_add_hdr(head, "Connection: keep-alive\r\n");
    } else {
        xbuf_add_hdr(head, "Connection: close\r\n");
    }

    if (client->request.parser.method == HTTP_HEAD) {
        // The HEAD response does not contain a body! But may contain "Content-Length"
        reset_response_body(client);
        body_size = client->response.wsgi_content_length;
        goto end;
    }

    if (client->response.chunked) {
        xbuf_add_hdr(head, "Transfer-Encoding: chunked\r\n");
        LOGi("Added Header 'Transfer-Encoding: chunked'");
        xbuf_add(head, "\r\n", 2);  // end of headers
        if (client->response.body_preloaded_size >= INT_MAX)
            return -7;  // critical error
        char * buf = xbuf_expand(head, 48);
        head->size += sprintf(buf, "%X\r\n", (int)client->response.body_preloaded_size);
        FIN(0);
    }

    if (body_size == 0) {
        reset_response_body(client);
    }
    if (body_data && body_size > 0) {
        reset_response_body(client);
        PyObject * buf = PyBytes_FromStringAndSize((const char *)body_data, (Py_ssize_t)body_size);
        body[0] = buf;
        client->response.body_chunk_num = 1;
        client->response.body_preloaded_size = body_size;
        client->response.body_total_size = body_size;
    }
    body_size = client->response.body_total_size;

end:
    if (body_size == 0) {
        xbuf_add_hdr(head, "Content-Length: 0\r\n");
        LOGi("Added Header 'Content-Length: 0'");
    }
    else if (body_size > 0) {
        char contlen[48]; 
        int len = snprintf(contlen, sizeof(contlen)-1, "Content-Length: %lld\r\n", (long long)body_size);
        xbuf_add_HDR(head, contlen, len);
        LOGi("Added Header 'Content-Length: %lld'", (long long)body_size);
    }
    xbuf_add(head, "\r\n", 2);  // end of headers
    hr = 0;

fin:
    if (hr < 0) {
        return hr;
    }
    if (g_log_level >= LL_TRACE && head->size >= 2) {
        int hsize = head->size - 2;
        const char * data = (body_size > 0 && client->response.body_chunk_num) ? PyBytes_AS_STRING(body[0]) : NULL;
        Py_ssize_t data_size = (body_size > 0 && client->response.body_chunk_num) ? PyBytes_GET_SIZE(body[0]) : 0;
        int dsize = (data_size <= 128) ? (int)data_size : 128;
        if (data && dsize > 0) {
            LOGt("HTTP RESPONSE RAW DATA [%d+%d]:\n%.*s\n%.*s", head->size, (int)body_size, hsize, head->data, dsize, data);
        } else {
            LOGt("HTTP RESPONSE RAW DATA [%d+%d]:\n%.*s", head->size, (int)body_size, hsize, head->data);
        }
    }
    client->response.headers_size = head->size;
    return head->size;
}

// ===================== aux functions ===================================================

int get_info_from_wsgi_response(client_t * client)
{
    StartResponse * response = client->start_response;
    client->response.wsgi_content_length = -1;  // unknown
    Py_ssize_t hsize = PyList_GET_SIZE(response->headers);
    for (Py_ssize_t i = 0; i < hsize; i++) {
        PyObject* tuple = PyList_GET_ITEM(response->headers, i);
        Py_ssize_t key_len = 0;
        const char * key = PyUnicode_AsUTF8AndSize(PyTuple_GET_ITEM(tuple, 0), &key_len);
        Py_ssize_t value_len = 0;
        const char * value = PyUnicode_AsUTF8AndSize(PyTuple_GET_ITEM(tuple, 1), &value_len);
        if (key_len == 14 && key[7] == '-' && strcasecmp(key, "Content-Length") == 0) {
            if (value_len == 0)
                return -2;  // error
            int64_t clen;
            if (value_len == 1 && value[0] == '0') {
                clen = 0;
            } else {
                clen = strtoll(value, NULL, 10);
                if (clen <= 0 || clen == LLONG_MAX)
                    return -3;  // error
            }
            LOGi("wsgi response: content-length = %lld", (long long)clen);
            client->response.wsgi_content_length = clen;
        }
    }
    return 0; // without parsing error
}

PyObject* wsgi_iterator_get_next_chunk(client_t * client, int outpyerr)
{
    if (client->response.body_iterator == NULL)
        return NULL;

    if (client->response.body_total_size == -111) {  // generator
        client->response.body_total_size = 0;
        int64_t size = client->response.body_preloaded_size;
        client->response.body_preloaded_size = 0;
        if (client->response.body_chunk_num > 0) {
            client->response.body_chunk_num = 0;
            PyObject * item = client->response.body[0];
            if (item && size == 0) {
                Py_DECREF(item); // skip empty items
                return NULL;
            }
            return item;
        }
        return NULL;
    }
    PyObject* item;
    while ( (item = PyIter_Next(client->response.body_iterator)) != NULL ) {
        if (!PyBytes_Check(item)) {
            client->error = 11;
            if (outpyerr)
                PYTYPE_ERR("wsgi_body: ITERATOR: contain item type = '%s' (expected bytes)", Py_TYPE(item)->tp_name);
            Py_DECREF(item);
            return NULL;
        }
        Py_ssize_t size = PyBytes_GET_SIZE(item);
        if (size > 0) {
            LOGd("wsgi_body: ITERATOR: get bytes size = %d", (int)size);
            return item;
        }
        Py_DECREF(item); // skip empty items
    }
    if (PyErr_Occurred()) {
        client->error = 11;
        if (outpyerr) {
            PyErr_Print();
            PyErr_Clear();
        }
    }
    return NULL;
}

int wsgi_body_pleload(client_t * client, PyObject * wsgi_body)
{
    PyObject** body = client->response.body;
    int64_t wsgi_content_length = client->response.wsgi_content_length;

    if (PyBytes_CheckExact(wsgi_body)) {
        ssize_t body_size = PyBytes_GET_SIZE(wsgi_body);
        if (body_size == 0) {
            client->response.body_total_size = 0;
            client->response.body_preloaded_size = 0;
            return 0;  // response without body
        }
        body[0] = wsgi_body;
        Py_INCREF(wsgi_body);  // Reasone: wsgi_body inserted into body chunks array
        client->response.body_chunk_num = 1;
        client->response.body_total_size = body_size;
        client->response.body_preloaded_size = body_size;
        return 1;  // response has body
    }
    if (wsgi_content_length == 0) {
        client->response.body_total_size = 0;
        client->response.body_preloaded_size = 0;
        return 0;  // response without body
    }
    PyObject* iterator = client->response.body_iterator;
    if (!iterator)
        return -1;

    const char* body_type = Py_TYPE(wsgi_body)->tp_name;
    // wsgi_body types:
    // Flask: ClosingIterator, FileWrapper (buffer_size)
    // Falcon: CloseableStreamIterator (_block_size)
    if (body_type && body_type[0] == 'F' && strcmp(body_type, "FileWrapper") == 0) {
        // This hack increases the speed of reading a file from a Flask
        PyObject * orig_buffer_size = PyObject_GetAttr(wsgi_body, g_cv.buffer_size); // refcnt: 1 -> 2
        if (orig_buffer_size != NULL) {
            Py_ssize_t refcnt = Py_REFCNT(orig_buffer_size);  // refcnt = 2
            PyObject * buf_size = PyLong_FromLong(g_srv.max_chunk_size);
            int error = PyObject_SetAttr(wsgi_body, g_cv.buffer_size, buf_size); // if OK then refcnt: 2 -> 1
            if (error) {
                LOGw("wsgi_body: failed to change file read buffer size (Flask.FileWrapper)");
                if (refcnt == Py_REFCNT(orig_buffer_size)) {  // if (refcnt == 2)
                    Py_DECREF(orig_buffer_size);  // refcnt: 2 -> 1
                }
            } else {
                Py_DECREF(orig_buffer_size); // destroy object
            }
            Py_DECREF(buf_size); // refcnt: 2 -> 1
        }
    }
    PyObject* item = NULL;
    int is_filelike = 0;
    int fully_loaded = 1;
    ssize_t prev_size = 0;
    size_t chunks = 0;
    while ( (item = wsgi_iterator_get_next_chunk(client, 1)) != NULL ) {
        ssize_t item_size = PyBytes_GET_SIZE(item);
        body[chunks++] = item;
        client->response.body_chunk_num = chunks;
        client->response.body_preloaded_size += item_size;
        if (chunks == 1 && Py_REFCNT(item) == 1) {
            LOGd("wsgi_body: detect reading from filelike object!");
            is_filelike = 1;
        }
        if (chunks == max_preloaded_body_chunks + 1) {
            fully_loaded = 0;
            break;
        }
        if (is_filelike) {
            // Only filelike data source check for max chunk size
            if (client->response.body_preloaded_size + prev_size >= (int64_t)g_srv.max_chunk_size) {
                LOGd("wsgi_body: overflow chunk buffer! Preloaded size = %d", client->response.body_preloaded_size);
                fully_loaded = 0;
                break;
            }
        }
        prev_size = item_size;
    }
    if (client->error) {
        // wsgi_body: incorrect content
        return -13;
    }
    if (fully_loaded) {
        if (wsgi_content_length > 0 && client->response.body_preloaded_size > wsgi_content_length) {
            LOGc("wsgi_body: real size of the response body exceeds the specified Content-Length");
            return -14;
        }
        LOGi("wsgi_body: response body fully loaded! (size = %lld)", (long long)client->response.body_preloaded_size);
        client->response.body_total_size = client->response.body_preloaded_size;
        return 1;
    }
    if (client->response.body_preloaded_size == wsgi_content_length) {
        LOGi("wsgi_body: response body fully loaded! (SIZE = %lld)", (long long)wsgi_content_length);
        client->response.body_total_size = wsgi_content_length;
        return 1;
    }
    if (wsgi_content_length > 0) {
        LOGd("wsgi_body: large content transfer begins. body_total_size = %lld", (long long)wsgi_content_length);
        client->response.body_total_size = wsgi_content_length;
        return 1;
    }
    client->response.chunked = 1;  // wsgi_content_length == -1
    LOGi("wsgi_body: chunked content transfer begins (unknown size of body)");
    return 2;
}

void init_request_def_env()
{
    for (int idx = 0; idx < g_srv.servers_num; idx++) {
        server_t * server = SERVER(idx);
        if (server->def_env)
            continue;
        char buf[32];
        sprintf(buf, "%d", server->port);
        PyObject * port = PyUnicode_FromString(buf);
        PyObject * host = PyUnicode_FromString(server->host);
        // only constant values!!!
        PyObject * env = PyDict_New();
        PyDict_SetItem(env, g_cv.SERVER_NAME, host);
        PyDict_SetItem(env, g_cv.SERVER_PORT, port);
        //PyDict_SetItem(env, g_cv.wsgi_input, io_BytesIO);   // not const!!!
        PyDict_SetItem(env, g_cv.wsgi_version, g_cv.wsgi_ver_1_0);
        PyDict_SetItem(env, g_cv.wsgi_url_scheme, (server->tls.enabled) ? g_cv.https : g_cv.http);
        PyDict_SetItem(env, g_cv.wsgi_errors, PySys_GetObject("stderr"));
        PyDict_SetItem(env, g_cv.wsgi_run_once, Py_False);
        PyDict_SetItem(env, g_cv.wsgi_multithread, Py_False);
        PyDict_SetItem(env, g_cv.wsgi_multiprocess, Py_True);
        Py_DECREF(port);
        Py_DECREF(host);
        // semi-const values
        PyDict_SetItem(env, g_cv.SERVER_PROTOCOL, g_cv.HTTP_1_1);
        PyDict_SetItem(env, g_cv.SCRIPT_NAME, (server->root_path.len > 0) ? server->root_path.obj : g_cv.empty_string);
        // non constant values!!!
        PyDict_SetItem(env, g_cv.PATH_INFO, g_cv.empty_string);
        PyDict_SetItem(env, g_cv.QUERY_STRING, g_cv.empty_string);
        server->def_env = env;
    }
}

void configure_parser_settings(llhttp_settings_t * ps)
{
    llhttp_settings_init(ps);
    ps->on_message_begin = on_message_begin;
    ps->on_url = on_url;
    ps->on_url_complete = on_url_complete;
    ps->on_header_field = on_header_field;
    ps->on_header_field_complete = on_header_field_complete;
    ps->on_header_value = on_header_value;
    ps->on_header_value_complete = on_header_value_complete;
    ps->on_headers_complete = on_headers_complete;
    ps->on_body = on_body;
    ps->on_message_complete = on_message_complete;
    ps->on_reset = on_reset;
}
