#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv.h"
#include "uv-common.h"
#include "llhttp.h"

#include "server.h"
#include "request.h"
#include "constants.h"

srv_t g_srv;
static int g_srv_inited = 0;

#define MAGIC_CLIENT ((void *)0xFFAB4321)

void alloc_cb(uv_handle_t * handle, size_t suggested_size, uv_buf_t * buf);
void read_cb(uv_stream_t * handle, ssize_t nread, const uv_buf_t * _buf);
int stream_write(client_t * client);

const char * get_rxstatus(int status)
{
    switch((rx_status_t)status) {
        case RXS_RESTING   : return "RXS_RESTING";
        case RXS_READING   : return "RXS_READING";
        case RXS_FREEZED   : return "RXS_FREEZED";
        case RXS_FAIL      : return "RXS_FAIL";
        case RXS_DONE      : return "RXS_DONE";
    }
    return "RXS_?????";
}

const char * get_cstate(int state)
{
    switch((client_state_t)state) {
        case CS_UNKNOWN    : return "CS_UNKNOWN";
        case CS_ACCEPT     : return "CS_ACCEPT";
        case CS_REQ_READ   : return "CS_REQ_READ";
        case CS_REQ_PARSE  : return "CS_REQ_PARSE";
        case CS_APP_CALL   : return "CS_APP_CALL";
        case CS_RESP_BILD  : return "CS_RESP_BILD";
        case CS_RESP_SEND  : return "CS_RESP_SEND";
        case CS_RESP_SENDED: return "CS_RESP_SENDED";
        case CS_RESP_END   : return "CS_RESP_END";
        case CS_DESTROY    : return "CS_DESTROY";
    }
    return "CS_?????";
}

static
void free_read_buffer(client_t * client)
{
    //LOGt("%s: free buffer = %p size = %d cap = %d", __func__, rbuf->data, rbuf->size, rbuf->capacity);
}

static
void reset_read_buffer(client_t * client, int opt)
{
    (void)opt;
    xbuf_t * buf = &client->rx.buf;
    LOGt("%s: reset buffer = %p size = %d cap = %d", __func__, buf->data, buf->size, buf->capacity);
    buf->size = 0;
}

int stream_read_start_ex(client_t * client, const char * func)
{
    int hr = 0;
    if (!func) func = __func__;
    FIN_IF(!client, -1);
    FIN_IF(client->rx.status == RXS_FREEZED, -2);
    FIN_IF(client->state == CS_RESP_SEND, -3);     // uv_write is active
    SET_CSTATE_FN(CS_REQ_READ, func);
    int rc = uv_read_start((uv_stream_t *)client, alloc_cb, read_cb);
    if (rc != 0 && rc != UV_EALREADY) {
        LOGe("%s: uv_read_start() return error = %d", func, rc);
        SET_CSTATE_FN(CS_DESTROY, func);  // maybe UV_ENOTCONN
        close_connection(client);
        return -7;
    }
    LOGt_IF(rc == UV_EALREADY, "%s: (UV_EALREADY)", func);
fin:
    LOGd_IF(hr, "%s: uv_read not activated (err = %d)", func, hr);
    return hr;
}

int stream_read_stop_ex(client_t * client, const char * func)
{
    if (!func) func = __func__;
    uv_read_stop((uv_stream_t *)client);
    LOGd("%s: uv_read stop (current state = %s)", func, get_cstate(client->state));
    return 0;
}

void close_cb(uv_handle_t * handle)
{
    client_t * client = (client_t *)handle;
    before_loop_callback(client);
    update_log_prefix(client);
    LOGn("disconnected =================================");
    Py_XDECREF(client->request.headers);
    Py_XDECREF(client->request.host);
    Py_XDECREF(client->request.wsgi_input_empty);
    Py_XDECREF(client->request.wsgi_input);
    xbuf_free(&client->head);
    free_start_response(client);
    reset_response_body(client);
    client->rx.status = RXS_RESTING;
    free_read_buffer(client);
    asgi_free(client);
    tls_client_free(client);
    free_read_timer(client);
    free(client);
    update_log_prefix(NULL);
}

void close_connection(client_t * client)
{
    SET_CSTATE(CS_DESTROY);
    if (!uv_is_closing((uv_handle_t*)client))
        uv_close((uv_handle_t*)client, close_cb);
}

void shutdown_cb(uv_shutdown_t * req, int status)
{
    client_t * client = (client_t *)req->handle;
    before_loop_callback(client);
    update_log_prefix(client);
    LOGt("%s: status = %d", __func__, status);
    close_connection(client);
    free(req);
}

void shutdown_connection(client_t * client)
{
    SET_CSTATE(CS_DESTROY);
    bool enotconn = is_stream_notconn((uv_stream_t *)client);
    if (!enotconn) {
        uv_shutdown_t* shutdown = malloc(sizeof(uv_shutdown_t));
        if (shutdown) {
            int rc = uv_shutdown(shutdown, (uv_stream_t *)client, shutdown_cb);
            if (rc == 0)
                return;
            free(shutdown); // uv_shutdown returned UV_ENOTCONN
        }
    }
    close_connection(client);
}

typedef struct {
    uv_write_t req;
    uv_buf_t   buf;
    client_t * client;
    char       data[1];
} x_write_req_t;

void x_write_cb(uv_write_t * req, int status)
{
    x_write_req_t * wreq = (x_write_req_t*)req;
    client_t * client = (client_t *)wreq->client;
    g_srv.num_writes--;
    update_log_prefix(client);
    free(req);
    if (status != 0) {
        LOGe("%s: write error: %s", __func__, uv_strerror(status));
        close_connection(client);
        return;
    }
    stream_read_start(client);
}

int x_send_status(client_t * client, int status)
{
    int hr = -1;
    const char * status_name = get_http_status_name(status);
    if (!status_name)
        status_name = "_unknown_";

    char hdr_buf[256];
    int len = 0;
    len += sprintf(hdr_buf + len, "HTTP/1.1 %d %s\r\n", status, status_name);
    if (status >= 200 && status != 204 && status != 304) {
        len += sprintf(hdr_buf + len, "Content-Length: 0\r\n");
    }
    len += sprintf(hdr_buf + len, "\r\n");
    LOGt("%s: HTTP RESPONSE RAW DATA [%d]:\n%s", __func__, len, hdr_buf);
    x_write_req_t * wreq = NULL;
    if (client->tls.enabled) {
        uv_buf_t buf;
        buf.base = hdr_buf;
        buf.len = len;
        int rc = tls_data_encode(client, &buf, 1, len);
        LOGe_IF(rc <= 0, "%s: cannot encrypt output data (rc = %d)", __func__, rc);
        FIN_IF(rc <= 0, -2);  // critical error
        size_t buf_size = sizeof(x_write_req_t) + client->tls.enc_out.size + 16;
        wreq = (x_write_req_t *)malloc(buf_size);
        memcpy(wreq->data, client->tls.enc_out.data, client->tls.enc_out.size);
        xbuf_reset(&client->tls.enc_out);
        wreq->buf.len = client->tls.enc_out.size;
        wreq->buf.base = wreq->data;
    } else {
        size_t buf_size = sizeof(x_write_req_t) + len + 16;
        wreq = (x_write_req_t *)malloc(buf_size);
        memcpy(wreq->data, hdr_buf, len);
        wreq->buf.len = len;
        wreq->buf.base = wreq->data;
    }
    wreq->client = client;
    g_srv.num_writes++;
    int rc = uv_write((uv_write_t*)wreq, (uv_stream_t*)client, &wreq->buf, 1, x_write_cb);
    if (rc != 0) {
        g_srv.num_writes--;
        LOGe("%s: uv_write returned error = %d (%s)", __func__, rc, uv_strerror(rc));
        SET_CSTATE(CS_DESTROY);  // critical error
        free(wreq);
        FIN(-6);
    }
    hr = 0;
fin:    
    return hr;
}

void write_cb(uv_write_t * req, int status)
{
    int hr = -1;
    int close_conn = 0;
    write_req_t * wreq = (write_req_t*)req;
    client_t * client = (client_t *)wreq->client;
    g_srv.num_writes--;
    before_loop_callback(client);
    update_log_prefix(client);
    if (status != 0) {
        LOGe("%s: Write error: %s", __func__, uv_strerror(status));
        reset_response_preload(client);
        goto fin;
    }
    SET_CSTATE(CS_RESP_SENDED);
    client->response.body_total_written += client->response.body_preloaded_size;
    reset_response_preload(client);
    if (client->response.chunked == 2) {
        LOGd("%s: last chunk sended!", __func__);
        client->response.last_chunk_time = UV_NOW;
        goto fin;
    }
    if (client->response.chunked == 0) {
        int64_t body_total_size = client->response.body_total_size;
        if (client->response.body_total_written > body_total_size) {
            LOGc("%s: ERROR: body_total_written > body_total_size", __func__);
            status = -1; // critical error -> close_connection
            goto fin;
        }
        if (client->response.body_total_written == body_total_size) {
            LOGd("%s: Response body is completely streamed. body_total_size = %lld", __func__, (long long)body_total_size);
            client->response.last_chunk_time = UV_NOW;
            goto fin;
        }
    }
    if (client->asgi) {
        goto fin;
    }
    SET_CSTATE(CS_RESP_BILD);
    client->error = 0;
    PyObject * chunk = wsgi_iterator_get_next_chunk(client, 0);
    if (!chunk) {
        if (client->error) {
            LOGe("%s: ERROR on read response body", __func__);
            status = -1; // error -> close_connection
            goto fin;
        }
        if (client->response.chunked == 1) {
            xbuf_reset(&client->head);
            xbuf_add_str(&client->head, "0\r\n\r\n");
            client->response.headers_size = client->head.size;
            client->response.chunked = 2;
            hr = stream_write(client);  // CS_RESP_SEND
            FIN_if(hr, -1, status = -1);
            LOGd("%s: end of iterable response body (chunked)", __func__);
            return;
        }
        LOGd("%s: end of iterable response body", __func__);
        goto fin;
    }
    Py_ssize_t csize = PyBytes_GET_SIZE(chunk);
    xbuf_reset(&client->head);
    if (client->response.chunked == 0) {
        client->response.headers_size = 0; // data without header
    } else {
        char * buf = xbuf_expand(&client->head, 48);
        client->head.size += sprintf(buf, "%X\r\n", (int)csize);
        client->response.headers_size = client->head.size;
    }
    client->response.body[0] = chunk;
    client->response.body_chunk_num = 1;
    client->response.body_preloaded_size = csize;
    hr = stream_write(client);  // CS_RESP_SEND
    FIN_if(hr, -1, status = -1);
    return;

fin:
    if (status < 0) {
        close_conn = 1;
    }
    if (!client->request.keep_alive || !g_srv.allow_keepalive) {
        close_conn = 1;
    }
    if (client->asgi) {
        SET_CSTATE(CS_RESP_BILD);
        reset_head_buffer(client);
        if (status < 0) {
            // cancel await current app.send()
            asgi_exec_send_future_as_exception(client, "Write error: %d", status);
            goto error;
        }
        // complete await current app.send()
        int err = asgi_exec_send_future(client);
        if (err) {
            LOGe("%s: asgi_exec_send_future failed: %d => closing connection", __func__, err);
            goto error;
        }
        if (client->asgi->send.latest_chunk) {
            LOGd("%s: ASGI last chunk sended!", __func__);
            client->response.last_chunk_time = UV_NOW;
            SET_CSTATE(CS_RESP_END);
        } else {
            SET_CSTATE(CS_RESP_SEND);
            return;  // continue sending chunks 
        }
    }
    if (!close_conn) {
        reset_response_body(client);
        SET_CSTATE(CS_RESP_END);
        if (client->asgi) {
            // ASGI hasn't completed yet (asgi_done hasn't been called)
            // stream_read_start will call asgi_done when it's time
            // Do nothing - asgi_done will take over
        } else {
            // WSGI output stream is already completed
            if (client->rx.status == RXS_FREEZED) {
                read_rxbuf_after_send(client, __func__);
                return;
            } else {
                stream_read_start(client);
            }
        }
    }
    if (close_conn) {
error:
        close_connection(client);
    }
}

int stream_write(client_t * client)
{
    int hr = CA_OK;
    write_req_t * wreq = &client->response.write_req;
    uv_buf_t * buf = wreq->bufs;
    int total_len = 0;
    int nbufs = 0;
    if (client->state == CS_RESP_SEND) {
        LOGe("%s: [UB] client state cannot equ CS_RESP_SEND !!!", __func__);
        SET_CSTATE(CS_DESTROY);
        FIN(CA_SHUTDOWN);
    }
    if (client->response.headers_size > 0) {
        if (client->response.headers_size != client->head.size) {
            LOGe("%s: [UB] headers_size != head.size", __func__);
            return CA_OK; // error ???
        }
        buf->base = client->head.data;
        buf->len = client->head.size;
        buf++;
        nbufs++;
        total_len += client->head.size;
    }
    if (client->response.body_preloaded_size > 0) {
        for (size_t i = 0; i < client->response.body_chunk_num; i++) {
            Py_ssize_t size = PyBytes_GET_SIZE(client->response.body[i]);
            char * data = PyBytes_AS_STRING(client->response.body[i]);
            buf->base = data;
            buf->len = (unsigned int)size;
            buf++;
            nbufs++;
            total_len += (int)size;
        }
    }
    if (client->response.chunked == 1) {
        buf->base = "\r\n";
        buf->len = 2;
        buf++;
        nbufs++;
        total_len += 2;
    }
    if (client->tls.enabled) {
        int rc = tls_data_encode(client, wreq->bufs, nbufs, total_len);
        if (rc <= 0) {
            LOGe("%s: cannot encrypt output data (rc = %d)", __func__, rc);
            SET_CSTATE(CS_DESTROY);
            FIN(CA_SHUTDOWN);  // critical error
        }
        wreq->bufs[0].len  = client->tls.enc_out.size;
        wreq->bufs[0].base = client->tls.enc_out.data;
        nbufs = 1;
        LOGi("%s: %d bytes [TLS] enc_out.size = %d", __func__, total_len, rc);
    } else {
        LOGi("%s: %d bytes", __func__, total_len);
    }
    stream_read_stop(client);
    SET_CSTATE(CS_RESP_SEND);
    g_srv.num_writes++;
    int rc = uv_write((uv_write_t*)wreq, (uv_stream_t*)client, wreq->bufs, nbufs, write_cb);
    if (rc != 0) {
        g_srv.num_writes--;
        LOGe("%s: uv_write returned error = %d (%s)", __func__, rc, uv_strerror(rc));
        SET_CSTATE(CS_DESTROY);
        FIN(CA_SHUTDOWN);
    }
    hr = CA_OK;
fin:
    return hr;
}

int send_fatal(client_t * client, int status, const char* error_string)
{
    if (!status) {
        status = HTTP_STATUS_BAD_REQUEST;  // 400
    }
    LOGe("%s: status = %d", __func__, status);
    if (client->state != CS_RESP_SEND) {
        int body_size = error_string ? strlen(error_string) : 0;
        build_response(client, 0, status, NULL, error_string, body_size);
        stream_write(client);  // CS_RESP_SEND
    }
    return CA_SHUTDOWN;
}

int send_error(client_t * client, int status, const char* error_string)
{
    if (!status) {
        status = HTTP_STATUS_INTERNAL_SERVER_ERROR;  // 500
    }
    LOGe("%s: status = %d", __func__, status);
    if (client->state != CS_RESP_SEND) {
        int flags = (client->request.keep_alive) ? RF_SET_KEEP_ALIVE : 0;
        int body_size = error_string ? strlen(error_string) : 0;
        build_response(client, flags, status, NULL, error_string, body_size);
        stream_write(client);  // CS_RESP_SEND
    }
    if (client->request.keep_alive && g_srv.allow_keepalive) {
        return CA_OK;
    }
    return CA_SHUTDOWN;
}

void read_timer_cb(uv_timer_t * timer)
{
    client_t * client = (client_t *)timer->data;
    if (!client) {
        return;  // timer destroyed
    }
    before_loop_callback(client);
    update_log_prefix(client);
    if (client->state == CS_DESTROY) {
        stream_read_stop(client);
        shutdown_connection(client);
        return;
    }
    if (client->rx.status == RXS_FREEZED) {
        read_rxbuf_after_send(client, __func__);
        return;
    }
}

void read_rxbuf_after_send(client_t * client, const char * _func)
{
    ssize_t nread = client->rx.buf.size - client->rx.parsed_size;
    LOGd("%s: call read_cb(%d, NULL)", _func ? _func : __func__, (int)nread);
    read_cb((uv_stream_t *)client, nread, NULL);
}

void read_cb(uv_stream_t * handle, ssize_t nread, const uv_buf_t * _buf)
{
    int hr = CA_OK;  // client action
    int err = 0;     // code of HTTP error
    uv_buf_t buf = { 0 };  // data for llhttp parser
    client_t * client = (client_t *)handle;
    llhttp_t * parser = &client->request.parser;
    before_loop_callback(client);
    update_log_prefix(client);

    if (nread == 0) {
        LOGd("read_cb: nread = 0");
        return;  // continue socket reading without any checking
    }
    if (nread < 0) {
        char err_name[128];
        uv_err_name_r((int)nread, err_name, sizeof(err_name) - 1);
        LOGd("%s: nread = %d  error: %s", __func__, (int)nread, err_name);
        FIN_IF(nread == UV_EOF && !client->tls.enabled, CA_CLOSE);  // remote peer disconnected
        LOGe_IF(nread != UV_ECONNRESET && nread != UV_EOF, "%s: Read error: %s", __func__, err_name);
        FIN(CA_SHUTDOWN);
    }
    if (client->request.start_time) {
        client->request.update_time = UV_NOW;  // update the time even after calling from read_rxbuf_after_send (write_cb)
    }
    if (_buf == NULL) {
        // read_cb called from write_cb or asgi_done (after sending response for prev request)
        // RX buffer contain unparsed HTTP data
        goto parsing;
    }
    client->rx.pkt += (client->rx.pkt < ULLONG_MAX) ? 1 : 0;
    client->rx.raw_total = (client->rx.raw_total < ULLONG_MAX - (uint64_t)nread) ? client->rx.raw_total + nread : ULLONG_MAX;
    LOGd("%s: [nread = %d]", __func__, (int)nread);

    if (client->tls.enabled) {
        if ((size_t)client->rx.rawbuf.size + (size_t)nread > g_srv.read_buffer_size) {
            LOGe("%s: [UB] received biggest chunk (SIZE = %d + %d)", __func__, client->rx.rawbuf.size, (int)nread);
            client->rx.status = RXS_FAIL;
            FIN(CA_SHUTDOWN);
        }
        client->rx.rawbuf.size += (int)nread;
        if (client->rx.status == RXS_FREEZED) {
            LOGc("%s: [TLS] rx.status == RXS_FREEZED  ==> [UB]", __func__);
            FIN(CA_SHUTDOWN);
        }
        client->rx.buf.size = 0;  // reset buffer
        client->rx.parsed_size = 0;
        // TLS: Processing Encrypted Incoming Data
        int act = tls_process_rx_data(client);
        if (act == 100 + TLS_HS_PENDING) {
            // No decrypted data - waiting for the next reading
            client->rx.buf.size = 0;
            client->rx.rawbuf.size = 0; // reset buf - Reason: all raw data was transferred to tls.bio_in
            stream_read_start(client);
            return;  // continue reading from socket
        }
        FIN_IF(client->tls.close_notify, CA_CLOSE);  // client terminated the TLS connection itself
        FIN_IF(act != CA_OK, act);  // critical error on tls_process_rx_data
        client->rx.rawbuf.size = 0; // reset buf - Reason: all raw data was transferred to tls.bio_in
        if (client->rx.buf.size == 0) {
            // No decrypted data - waiting for the next reading
            stream_read_start(client);
            return;  // continue reading from socket
        }
        client->rx.parsed_size = 0;  // fresh data into rx.buf
    } else {
        client->rx.total = client->rx.raw_total;
        if ((size_t)client->rx.buf.size + (size_t)nread > g_srv.read_buffer_size) {
            LOGe("%s: [UB] received biggest chunk (SIZE = %d + %d)", __func__, client->rx.buf.size, (int)nread);
            client->rx.status = RXS_FAIL;
            FIN(CA_SHUTDOWN);
        }
        client->rx.buf.size += (int)nread;
    }

parsing:
    buf.base = client->rx.buf.data + client->rx.parsed_size;
    buf.len  = client->rx.buf.size - client->rx.parsed_size;
    if (g_log_level >= LL_TRACE) {
        LOGt("HTTP REQUEST RAW DATA [%d]:\n%.*s", (int)buf.len, (int)buf.len, buf.base);
    }
    int prev_psize = client->rx.parsed_size;
    SET_CSTATE(CS_REQ_PARSE);
    llhttp_resume(&client->request.parser);  // reset llhttp error to 0 (for continue parsing)
    client->request.parser_locked = true;
    enum llhttp_errno error = llhttp_execute(parser, buf.base, buf.len);
    LOGt("%s: llhttp_execute(%d) ret = %s pos = %d", __func__, (int)buf.len, llhttp_errno_name(error), (int)((size_t)llhttp_get_error_pos(parser) - (size_t)buf.base));
    int add_psize = 0;
    if (error == HPE_OK) {
        add_psize = (int)buf.len;
        LOGt("%s: ========= HPE_OK: add_psize = %d", __func__, add_psize);
    }
    if (error == HPE_PAUSED) {
        // HPE_PAUSED returned only after on_message_complete (request.load_state >= LS_MSG_END)
        char * pos = (char *)llhttp_get_error_pos(parser);
        add_psize = (int)((size_t)pos - (size_t)buf.base);
        LOGt("%s: ========= HPE_PAUSED: add_psize = %d", __func__, add_psize);
    }
    int curr_psize = prev_psize + add_psize;
    if (error == HPE_PAUSED) {
        // HPE_PAUSED returned only after on_message_complete (request.load_state >= LS_MSG_END)
        if (client->request.load_state < LS_MSG_END) {
            LOGf("%s: llhttp parser return HPE_PAUSED, but load_state < LS_MSG_END");
            FIN(CA_SHUTDOWN);
        }
        if (add_psize > (int)buf.len) {
            LOGf("%s: [UB] llhttp parser returned incorrect pos on HPE_PAUSED state !!!", __func__);
            FIN(CA_SHUTDOWN);
        }
        if (add_psize <= 0) {
            LOGf("%s: [UB] incorrect add_psize = %d (buf.len = %d)", __func__, add_psize, (int)buf.len);
            FIN(CA_SHUTDOWN);
        }
        if (curr_psize > client->rx.buf.size) {
            LOGf("%s: [UB] incorrect parsed_size: %d + %d > %d", __func__, prev_psize, add_psize, client->rx.buf.size);
            FIN(CA_SHUTDOWN);
        }
        client->rx.parsed_size = curr_psize;
        if (curr_psize == client->rx.buf.size) {
            LOGd("%s: %s >>> RXS_READING  (rx.buf fully parsed)", __func__, get_rxstatus(client->rx.status));
            client->rx.status = RXS_READING;  // restore READING mode for continue reading from TCP socket
            client->request.parser_locked = false;  // unblock llhttp parser state
            llhttp_reset(&client->request.parser);  // the next portion of data needs to be parsed from scratch
            stream_read_start(client);
            error = HPE_OK;  // received rx.buf fully parsed
        }
        if (error == HPE_PAUSED && add_psize >= 0) {
            LOGd("%s: %s >>> RXS_FREEZED  (rx.parsed_size = %d + %d = %d) [%d]", __func__, get_rxstatus(client->rx.status), prev_psize, add_psize, curr_psize, client->rx.buf.size);
            client->rx.status = RXS_FREEZED;
            client->request.parser_locked = true;
            stream_read_stop(client);  // freeze reading from TCP socket (until processing rx.buf)
        }
        error = HPE_OK;
    }
    if (error != HPE_OK) {
        const char * err_pos = llhttp_get_error_pos(parser);
        LOGe("Parse error: %s %s\n", llhttp_errno_name(error), client->request.parser.reason);
        err = HTTP_STATUS_BAD_REQUEST;
        err = (client->error >= 400 && client->error < 600) ? client->error : err;
        if (error == HPE_INVALID_VERSION && parser->reason) {
            if (strcmp(parser->reason, "Expected CRLF after version") == 0)
                err = HTTP_STATUS_BAD_REQUEST;  // 400
            if (strcmp(parser->reason, "Invalid major version") == 0)
                err = HTTP_STATUS_BAD_REQUEST;  // 400
            if (strcmp(parser->reason, "Invalid HTTP version") == 0)
                err = HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED;  // 505
        }
        int act = send_fatal(client, err, NULL);
        err = 0;  // skip call send_error
        FIN(act);
    }
    client->rx.parsed_size = curr_psize;
    if (client->request.load_state < LS_MSG_END) {
        // This code block not used if llhttp_execute returned HPE_PAUSED !!!
        // Check that the status HPE_OK means that the data transferred to the llhttp parser has been fully processed
        if (client->rx.parsed_size != client->rx.buf.size) {
            LOGf("%s: incorrect llhttp state after receivig HPE_OK !!!", __func__);
            LOGf("%s: ERROR: parsed_size: %d + %d != %d", __func__, prev_psize + (int)buf.len != client->rx.buf.size);
            FIN(CA_SHUTDOWN);
        }
        if (client->rx.status == RXS_FREEZED) {
            LOGd("%s: RXS_FREEZED ==> RXS_READING  (partial parsing)", __func__);
            client->rx.status = RXS_READING;
            stream_read_start(client);  // reqired to read the tail of the request from TCP socket
            client->request.parser_locked = true;
            if (err) {
                int act = send_fatal(client, err, NULL);
                err = 0;  // skip call send_error
                FIN(act);
            }
        }
        if (client->error) {
            err = (client->error >= 400 && client->error < 600) ? client->error : HTTP_STATUS_BAD_REQUEST;
            FIN(CA_OK);
        }
        LOGt("http chunk parsed: load_state = %d, wsgi_input_size = %lld",
            (int)client->request.load_state, (long long)client->request.wsgi_input_size);
        // continue read from socket (or read from rx.buf)
        FIN(CA_OK);
    }
    if (client->request.load_state != LS_OK) {
        // error from callback function "on_message_complete"
        if (client->request.expect_continue && client->error == HTTP_STATUS_EXPECTATION_FAILED) {
            client->request.expect_continue = 0;
            err = HTTP_STATUS_EXPECTATION_FAILED;
            FIN(CA_OK);
        }
        err = HTTP_STATUS_BAD_REQUEST;
        FIN(CA_OK);
    }
    LOGd("HTTP request successfully parsed (wsgi_input_size = %lld)", (long long)client->request.wsgi_input_size);
    if (client->asgi) {
        SET_CSTATE(CS_APP_CALL);
        err = asgi_call_app(client);
        if (!err) {
            SET_CSTATE(CS_RESP_BILD);
            stream_read_stop(client);
        }
        FIN(CA_OK);
    }
    SET_CSTATE(CS_APP_CALL);
    err = call_wsgi_app(client);
    SET_CSTATE(CS_RESP_BILD);
    if (err) {
        FIN(CA_OK);
    }
    err = process_wsgi_response(client);
    if (err) {
        FIN(CA_OK);
    }
    err = create_response(client);
    if (err) {
        FIN(CA_OK);
    }
    LOGi("Response created! (len = %d+%lld)", client->head.size, (long long)client->response.body_preloaded_size);
    hr = stream_write(client);  // CS_RESP_SEND

fin:
    if (client->tls.close_notify) {
        // The client sent a TLS close_notify => terminating the connection
        LOGn("%s: [TLS] CLOSE_NOTIFY received => closing the connection", __func__);
        if (hr == CA_OK) {
            hr = CA_CLOSE;
        }
    }
    //reset_read_buffer(client, 0);

    if (PyErr_Occurred()) {
        if (err == 0)
            err = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        PyErr_Print();
        PyErr_Clear();
    }
    if (err && hr == CA_OK) {
        if (err < HTTP_STATUS_BAD_REQUEST)
            err = HTTP_STATUS_BAD_REQUEST;
        hr = send_error(client, err, NULL);
    }
    if (client->request.parser_locked == false) {
        llhttp_reset(&client->request.parser);
    }
    if (client->request.load_state >= LS_MSG_END) {
        client->request.load_state = LS_WAIT;
    }
    if (hr == CA_SHUTDOWN) {
        stream_read_stop(client);
        shutdown_connection(client);
        return;
    }
    if (hr == CA_CLOSE) {
        stream_read_stop(client);
        close_connection(client);
        return;
    }
}

void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
    client_t * client = (client_t *)handle;
    update_log_prefix(client);
    const int read_buffer_size = (int)g_srv.read_buffer_size;
    LOGd("%s: size = %d (suggested = %d)", __func__, read_buffer_size, (int)suggested_size);
    buf->base = NULL;
    buf->len = 0;
    if (client->rx.status == RXS_RESTING || client->rx.status == RXS_DONE) {
        client->rx.status = RXS_READING;
    }
    if (client->rx.status != RXS_READING) {
        LOGc("%s: [UB] read_cb not processed all data! (rx.status = %d)", __func__, (int)client->rx.status);
        return;  // error
    }
    xbuf_t * rbuf = &client->rx.buf;
    if (!rbuf->data) {
        xbuf_init2(rbuf, client->buf_read_prealloc, read_buffer_size + 3);
        LOGd("%s: prepare rx.buf = %p (cap = %d)", __func__, rbuf->data, (int)rbuf->capacity);
    }
    if (client->tls.enabled) {
        xbuf_t * rawbuf = &client->rx.rawbuf;
        if (!rawbuf->data) {
            xbuf_init2(rawbuf, client->buf_read_prealloc + read_buffer_size + 8, read_buffer_size + 3);
            LOGd("%s: prepare rx.rawbuf = %p (cap = %d)", __func__, rawbuf->data, rawbuf->capacity);
        }
        int bio_in_size = (int)BIO_pending(client->tls.bio_in);
        if (bio_in_size > read_buffer_size / 2) {
            LOGc("%s: [TLS] BIO_in_size = %d (rx.buf.cap = %d)", __func__, bio_in_size, read_buffer_size);
            return; // error
        }
        rawbuf->size = 0;  // reset buffer
        buf->base = rawbuf->data;
    } else {
        rbuf->size = 0;   // reset buffer
        buf->base = rbuf->data;
    }
    buf->len = read_buffer_size;
    buf->base[0] = 0;
    client->rx.parsed_size = 0;  // reset llhttp parser pos
}

void connection_cb(uv_stream_t * uvserver, int status)
{
    before_loop_callback(NULL);
    update_log_prefix(NULL);
    if (status < 0) {
        LOGe("Connection error %s\n", uv_strerror(status));
        return;
    }
    server_t * server = (server_t *)uvserver;
    LOGi("new connection ================================= %s", server->tls.enabled ? "[TLS]" : "");
    size_t client_aux_size = g_srv.read_buffer_size + 8;  // only rx.buf
    if (server->tls.enabled) {
        client_aux_size *= 2;  // rx.rawbuf + rx.buf
    }
    client_t * client = client = calloc(1, sizeof(client_t) + client_aux_size);
    client->server = server;
    client->rx.status = RXS_RESTING;
    client->tls.enabled = server->tls.enabled;
    client->response.write_req.client = client;
    client->state = CS_ACCEPT;

    uv_tcp_init(g_srv.loop, &client->handle);
    
    uv_tcp_nodelay(&client->handle, (g_srv.tcp_nodelay > 0) ? 1 : 0);
    
    if (g_srv.tcp_keepalive < 0)
        uv_tcp_keepalive(&client->handle, 0, 60);  // disable

    if (g_srv.tcp_keepalive >= 1)
        uv_tcp_keepalive(&client->handle, 1, g_srv.tcp_keepalive);  // enable and set timeout

    if (g_srv.tcp_send_buf_size > 0) 
        uv_send_buffer_size((uv_handle_t *)client, &g_srv.tcp_send_buf_size);

    if (g_srv.tcp_recv_buf_size > 0) 
        uv_recv_buffer_size((uv_handle_t *)client, &g_srv.tcp_recv_buf_size);

    client->handle.data = MAGIC_CLIENT;

    int rc = uv_accept(uvserver, (uv_stream_t*)&client->handle);
    if (rc) {
        SET_CSTATE(CS_DESTROY);
        uv_close((uv_handle_t*)&client->handle, close_cb);
        return;
    }
    sockaddr_t sock_addr;
    memset(&sock_addr, 0, sizeof(sock_addr));
    int sock_len = sizeof(sock_addr);
    rc = uv_tcp_getpeername((uv_tcp_t*)&client->handle, &sock_addr.addr, &sock_len);
    LOGw_IF(rc, "%s: cannot get remote addr (err = %d)", __func__, rc);
    if (rc == 0) {
        char ip[48];
        rc = uv_ip_name(&sock_addr.addr, ip, sizeof(ip));
        LOGw_IF(rc, "%s: cannot get remote IP-addr (err = %d)", __func__, rc);
        if (rc)
            ip[0] = 0;
        if (sock_addr.addr.sa_family == AF_INET6) {
            sprintf(client->remote_addr, "[%s]:%d", ip, (int)ntohs(sock_addr.in6.sin6_port));
        } else {
            sprintf(client->remote_addr, "%s:%d", ip, (int)ntohs(sock_addr.in4.sin_port));
        }
    }
    update_log_prefix(client);
    LOGn("connected ================================= %s", client->tls.enabled ? "[TLS]" : "");
    if (client->tls.enabled) {
        // Initialize TLS for a new connection if enabled.
        if (tls_client_init(client, NULL) != 0) {
            LOGe("%s: tls_client_init failed => closing connection", __func__);
            SET_CSTATE(CS_DESTROY);
            uv_close((uv_handle_t *)&client->handle, close_cb);
            return;
        }
        LOGi("%s: [TLS] client initialized", __func__);
    }
    llhttp_init(&client->request.parser, HTTP_REQUEST, &g_srv.parser_settings);
    client->request.parser.data = client;
    client->request.start_time = UV_NOW;
    client->request.update_time = UV_NOW;
    stream_read_start(client);  // CS_REQ_READ
}

void client_aux_check(uv_handle_t * handle, void * arg)
{
    if (handle->data != MAGIC_CLIENT) {
        return;  // skip non clients handles
    }
    client_t * client = (client_t *)handle;
    uint64_t now = UV_NOW;  // equ uv_now()
    if (g_srv.read_req_timeout && client->request.update_time) {
        if (now > client->request.update_time + g_srv.read_req_timeout) {
            update_log_prefix(client);
            LOGi("%s: waiting next chunk of current request TIMED OUT => conn shutdown", __func__);
            goto shutdown;
        }
    }
    if (g_srv.curr_req_timeout && client->request.start_time) {
        if (now > client->request.start_time + g_srv.curr_req_timeout) {
            update_log_prefix(client);
            LOGi("%s: waiting to receive the current request TIMED OUT => conn shutdown", __func__);
            goto shutdown;
        }
    }
    if (g_srv.next_req_timeout && client->response.last_chunk_time) {
        if (now > client->response.last_chunk_time + g_srv.next_req_timeout) {
            update_log_prefix(client);
            LOGi("%s: waiting next request TIMED OUT => conn shutdown", __func__);
            goto shutdown;
        }
    }
    return;
shutdown:
    stream_read_stop(client);
    shutdown_connection(client);
}

void svc_timer_cb(uv_timer_t * timer)
{
    if (g_srv.read_req_timeout || g_srv.curr_req_timeout || g_srv.next_req_timeout) {
        uv_walk(g_srv.loop, client_aux_check, NULL);
    }
}

void signal_handler(uv_signal_t * req, int signum)
{
    if (signum == SIGINT) {
        uv_stop(g_srv.loop);
        uv_signal_stop(req);
        if (g_srv.hook_sigint == 2) {
            update_log_prefix(NULL);
            LOGw("%s: halt process", __func__);
            exit(0);
        }
        g_srv.exit_code = 1; // server interrupted by SIGINT
        LOGw("%s: set exit_code = %d", __func__, g_srv.exit_code);
    }
}

int init_srv(void)
{
    int hr = -1;
    if (g_srv_inited)
        return -1;

    g_srv.loop = uv_default_loop();

    configure_parser_settings(&g_srv.parser_settings);
    init_constants();
    int tls_num = tls_server_init_all();
    FIN_IF(tls_num < 0, tls_num);
    init_request_def_env();
    PyType_Ready(&StartResponse_Type);
    if (g_srv.asgi_app) {
        PyType_Ready(&ASGI_Type);
        hr = asyncio_init(&g_srv.aio);
        FIN_IF(hr, hr);
    }
    for (int idx = 0; idx < g_srv.servers_num; idx++) {
        server_t * server = SERVER(idx);
        sockaddr_t addr;
        int tcp_flags = 0;
        if (server->ipv6) {
            tcp_flags = AF_INET6;
            uv_ip6_addr(server->host, server->port, &addr.in6);
        } else {
            tcp_flags = AF_INET;
            uv_ip4_addr(server->host, server->port, &addr.in4);
        }
        uv_tcp_init_ex(g_srv.loop, (uv_tcp_t *)server, tcp_flags);
        int enabled = 1;
#ifdef _WIN32
        //uv__socket_sockopt((uv_handle_t*)server, SO_REUSEADDR, &enabled);
#else
        int so_reuseport = 15;  // SO_REUSEPORT
        uv__socket_sockopt((uv_handle_t*)server, so_reuseport, &enabled);
#endif
        int err = uv_tcp_bind((uv_tcp_t *)server, &addr.addr, 0);
        LOGe_IF(err, "Bind error %s", uv_strerror(err));
        FIN_IF(err, -9);
    }
    for (int idx = 0; idx < g_srv.servers_num; idx++) {
        server_t * server = SERVER(idx);
        int err = uv_listen((uv_stream_t*)server, g_srv.backlog, connection_cb);
        LOGe_IF(err, "Listen error %s", uv_strerror(err));
        FIN_IF(err, -10);
        server->root_path.obj = PyUnicode_FromString(server->root_path.str);
    }
    if (g_srv.hook_sigint > 0) {
        uv_signal_init(g_srv.loop, &g_srv.signal);
        uv_signal_start(&g_srv.signal, signal_handler, SIGINT);
    }
    uv_timer_init(g_srv.loop, &g_srv.svc_timer);
    uv_timer_start(&g_srv.svc_timer, svc_timer_cb, (uint64_t)g_srv.svc_timer_interval, (uint64_t)g_srv.svc_timer_interval);
    g_srv_inited = 1;
    hr = 0;

fin:
    if (hr) {
        if (g_srv.signal.signal_cb)
            uv_signal_stop(&g_srv.signal);

        if (hr <= -9 && hr >= -90) {
            for (int idx = 0; idx < g_srv.servers_num; idx++) {
                uv_close((uv_handle_t *)SERVER(idx), NULL);
            }
        }
        if (g_srv.loop)
            uv_loop_close(g_srv.loop);

        if (g_srv.aio.asyncio)
            asyncio_free(&g_srv.aio, false);

        tls_server_free(NULL);
    }    
    return hr;
}

PyObject * init_server(PyObject * Py_UNUSED(self), PyObject * server)
{
    int hr = -1;
    int64_t rv;
    PyObject * aio_loop = NULL;

    update_log_prefix(NULL);
    if (g_srv_inited) {
        PyErr_Format(PyExc_Exception, "server already inited");
        return PyLong_FromLong(-1000);
    }
    memset(&g_srv, 0, sizeof(g_srv));
    g_srv.pysrv = server;

    int64_t loglevel = get_obj_attr_int(server, "loglevel");
    if (loglevel == LLONG_MIN) {
        PyErr_Format(PyExc_ValueError, "Option loglevel not defined");
        return PyLong_FromLong(-1010);
    }
    set_log_level((int)loglevel);

    PyObject * app = PyObject_GetAttrString(server, "app");
    Py_XDECREF(app);
    if (!app) {
        PyErr_Format(PyExc_ValueError, "Option app not defined");
        return PyLong_FromLong(-1011);
    }
    if (asgi_app_check(app) == true) {
        g_srv.asgi_app = app;
    } else {
        g_srv.wsgi_app = app;
    }
    if (g_srv.asgi_app) {
        asyncio_load_cfg(&g_srv.aio);
    }
    int servers_num = get_obj_attr_bindlist(server, "bindlist", -1, NULL, NULL);
    if (servers_num < 1 || servers_num > HTTP_SERVERS_MAX) {
        PyErr_Format(PyExc_ValueError, "Option bindlist not defined or too long");
        return PyLong_FromLong(-1012);
    }
    if (servers_num > HTTP_SERVERS_MAX) {
        PyErr_Format(PyExc_ValueError, "Option bindlist contains too many items");
        return PyLong_FromLong(-1012);
    }
    for (int idx = 0; idx < servers_num; idx++) {
        const char * host = NULL;
        int port = 0;
        int rc = get_obj_attr_bindlist(server, "bindlist", idx, &host, &port);
        if (rc <= 0) {
            PyErr_Format(PyExc_ValueError, "Option bindlist[%d] => incorrect (err = %d)", idx, rc);
            return PyLong_FromLong(-1013);
        }
        strncpy(g_srv.servers[idx].host, host, sizeof(g_srv.servers[idx].host) - 1);
        g_srv.servers[idx].ipv6 = (strchr(host, ':') == NULL) ? 0 : 1;
        g_srv.servers[idx].port = port;
        g_srv.servers[idx].srv = &g_srv;
    }
    g_srv.servers_num = servers_num;

    int64_t backlog = get_obj_attr_int(server, "backlog");
    if (backlog == LLONG_MIN) {
        PyErr_Format(PyExc_ValueError, "Option backlog not defined");
        return PyLong_FromLong(-1014);
    }
    g_srv.backlog = (int)backlog;

    rv = get_obj_attr_int(server, "hook_sigint");
    g_srv.hook_sigint = (rv >= 0) ? (int)rv : 1;

    rv = get_obj_attr_int(server, "allow_keepalive");
    g_srv.allow_keepalive = (rv == 0) ? 0 : 1;

    rv = get_obj_attr_int(server, "resp_hdr_lower");
    if (rv == LLONG_MIN) {
        g_srv.resp_hdr_lower = (g_srv.asgi_app) ? 1 : 0;
    } else {
        g_srv.resp_hdr_lower = (rv == 0) ? 0 : 1;
    }
    rv = get_obj_attr_int(server, "add_header_date");
    g_srv.add_header_date = (rv == 0) ? 0 : 1;

    const char * srvname = get_obj_attr_str(server, "add_header_server");
    if (srvname && srvname[0]) {
        strncpy(g_srv.header_server, srvname, sizeof(g_srv.header_server) - 1);
        g_srv.add_header_server = (int)strlen(g_srv.header_server);
    }
    const char * root_path = get_obj_attr_str(server, "root_path");
    if (root_path && root_path[0]) {
        size_t root_path_len = strlen(root_path);
        size_t max_len = sizeof(SERVER(0)->root_path) - 1;
        if (root_path_len < 2 || root_path[0] != '/' || root_path[1] == '/' || root_path_len >= max_len) {
            PyErr_Format(PyExc_ValueError, "Option root_path contain incorrect value");
            return PyLong_FromLong(-1019);
        }
        if (root_path[root_path_len - 1] == '/') {
            root_path_len--;
        }
        FIN_IF(root_path_len < 2, -1019);
        FIN_IF(root_path[root_path_len] == '/', -1019);
        for (int idx = 0; idx < servers_num; idx++) {
            server_t * server = SERVER(idx);
            memcpy(server->root_path.str, root_path, root_path_len);
            server->root_path.str[root_path_len] = 0;
            server->root_path.len = root_path_len;
        }
    }
    rv = get_obj_attr_int(server, "max_headers_num");
    g_srv.max_headers_num = (rv > 0) ? (int)rv : 200;

    rv = get_obj_attr_int(server, "max_content_length");
    if (rv == LLONG_MIN) {
        rv = get_env_int("FASTPYSGI_MAX_CONTENT_LENGTH");
    }
    g_srv.max_content_length = (rv >= 0) ? rv : def_max_content_length;
    if (g_srv.max_content_length >= INT_MAX)
        g_srv.max_content_length = INT_MAX - 1;

    rv = get_obj_attr_int(server, "max_chunk_size");
    if (rv == LLONG_MIN) {
        rv = get_env_int("FASTPYSGI_MAX_CHUNK_SIZE");
    }
    g_srv.max_chunk_size = (rv >= 0) ? (size_t)rv : (size_t)def_max_chunk_size;
    g_srv.max_chunk_size = _min(g_srv.max_chunk_size, MAX_max_chunk_size);
    g_srv.max_chunk_size = _max(g_srv.max_chunk_size, MIN_max_chunk_size);

    rv = get_obj_attr_int(server, "read_buffer_size");
    if (rv == LLONG_MIN) {
        rv = get_env_int("FASTPYSGI_READ_BUFFER_SIZE");
    }
    g_srv.read_buffer_size = (rv >= 0) ? (size_t)rv : (size_t)def_read_buffer_size;
    g_srv.read_buffer_size = _min(g_srv.read_buffer_size, MAX_read_buffer_size);
    g_srv.read_buffer_size = _max(g_srv.read_buffer_size, MIN_read_buffer_size);
    if (rv != LLONG_MIN && rv < 0) {
        g_srv.read_buffer_size = (size_t)(-rv);  // ONLY FOR TESTING
    }

    rv = get_obj_attr_int(server, "tcp_nodelay");
    g_srv.tcp_nodelay = (rv >= 0) ? (int)rv : 0;

    rv = get_obj_attr_int(server, "tcp_keepalive");
    g_srv.tcp_keepalive = (rv >= -1) ? (int)rv : 0;

    rv = get_obj_attr_int(server, "tcp_send_buf_size");
    g_srv.tcp_send_buf_size = (rv >= 0) ? (int)rv : 0;

    rv = get_obj_attr_int(server, "tcp_recv_buf_size");
    g_srv.tcp_recv_buf_size = (rv >= 0) ? (int)rv : 0;

    rv = get_obj_attr_int(server, "svc_timer_interval");
    g_srv.svc_timer_interval = (rv > 0) ? (uint64_t)rv : 3000;  // default value = 3000 ms

    rv = get_obj_attr_int(server, "read_req_timeout");
    g_srv.read_req_timeout = (rv > 0) ? (uint64_t)rv : 0;  // default value = 0 ms (disabled)

    rv = get_obj_attr_int(server, "curr_req_timeout");
    g_srv.curr_req_timeout = (rv > 0) ? (uint64_t)rv : 0;  // default value = 0 ms (disabled)

    rv = get_obj_attr_int(server, "next_req_timeout");
    g_srv.next_req_timeout = (rv > 0) ? (uint64_t)rv : 0;  // default value = 0 ms (disabled)

    rv = get_obj_attr_int(server, "nowait");
    g_srv.nowait.mode = (rv <= 0) ? 0 : (int)rv;

    hr = init_srv();
    if (hr) {
        LOGc("%s: critical error = %d", __func__, hr);
        PyErr_Format(PyExc_Exception, "Cannot init TCP server. Error = %d", hr);
        memset(&g_srv, 0, sizeof(g_srv));
    } else {
        const char * ver = (strlen(FASTPYSGI_VERSION) == 0) ? "<unknown>" : FASTPYSGI_VERSION;
        const char * host = g_srv.servers[0].host;
        int port          = g_srv.servers[0].port;
        const char * app = (g_srv.asgi_app) ? "ASGI" : "WSGI";
        const char * tls = (g_srv.servers[0].tls.enabled) ? ", [TLS]" : "";
        int saved_level = g_log_level;
        set_log_level(LL_INFO);
        LOGi("version: %s, host: \"%s\", port: %d, app: %s, hook_sigint: %d, nowait.mode: %d, loglevel: %d%s",
            ver, host, port, app, g_srv.hook_sigint, g_srv.nowait.mode, saved_level, tls);
        set_log_level(saved_level);
    }
fin:
    return PyLong_FromLong(hr);
}

PyObject * get_version(PyObject * Py_UNUSED(self))
{
    return PyUnicode_FromString(FASTPYSGI_VERSION);
}

PyObject * change_setting(PyObject * Py_UNUSED(self), PyObject * args)
{
    PyObject * server = NULL;
    char * name = NULL;

    int rc = PyArg_ParseTuple(args, "Os", &server, &name);
    if (rc != 1)
        return PyLong_FromLong(-2);

    if (!server)
        return PyLong_FromLong(-3);

    if (!name || strlen(name) < 2)
        return PyLong_FromLong(-4);

    if (strcmp(name, "allow_keepalive") == 0) {
        int64_t rv = get_obj_attr_int(server, name);
        if (rv == 0 || rv == 1) {
            g_srv.allow_keepalive = (int)rv;
            LOGn("%s: SET allow_keepalive = %d", __func__, g_srv.allow_keepalive);
            return PyLong_FromLong(0);
        }
        return PyLong_FromLong(-5); // unsupported value
    }
    return PyLong_FromLong(-1);  // unknown setting
}

PyObject * run_server(PyObject * self, PyObject * server)
{
    if (!g_srv_inited) {
        PyErr_Format(PyExc_Exception, "server not inited");
        return PyLong_FromLong(-1);
    }    
    if (g_srv.asgi_app) {
        aio_loop_run(&g_srv.aio);
    }
    else {
        uv_run(g_srv.loop, UV_RUN_DEFAULT);
    }
    const char * reason = (g_srv.exit_code == 1) ? "(SIGINT)" : "";
    LOGn("%s: FIN %s", __func__, reason);
    PyObject * rc = close_server(self, server);
    Py_XDECREF(rc);
    return PyLong_FromLong(g_srv.exit_code);
}

PyObject * run_nowait(PyObject * self, PyObject * server)
{
    int ret_code = 0;
    if (!g_srv_inited) {
        PyErr_Format(PyExc_Exception, "server not inited!");
        return PyLong_FromLong(-1);
    }
    if (g_srv.nowait.base_handles == 0) {
        uv_run(g_srv.loop, UV_RUN_NOWAIT);
        g_srv.nowait.base_handles = g_srv.loop->active_handles;
        LOGd("%s: base_handles = %d", __func__, g_srv.nowait.base_handles);
    }
    int idle_runs = 0;
    while (1) {
        int rc = uv_run(g_srv.loop, UV_RUN_NOWAIT);
        if (rc != 0) {
            // https://docs.libuv.org/en/v1.x/loop.html?highlight=uv_run#c.uv_run
            // more callbacks are expected (meaning you should run the event loop again sometime in the future.
        }
        if (g_srv.exit_code != 0) {
            ret_code = g_srv.exit_code;
            break;
        }
        if (g_srv.nowait.mode == 1) {
            ret_code = 0;
            break;
        }
        if ((int)g_srv.loop->active_handles > g_srv.nowait.base_handles) {
            idle_runs = 0;
            continue;  // clients are still connected
        }
        idle_runs++;
        if (idle_runs >= 2) {
            ret_code = 0;
            break;
        }
    }
    return PyLong_FromLong(ret_code);
}

PyObject * close_server(PyObject * Py_UNUSED(self), PyObject * Py_UNUSED(server))
{
    if (g_srv_inited) {
        update_log_prefix(NULL);
        LOGn("%s", __func__);
        uv_timer_stop(&g_srv.svc_timer);
        if (g_srv.signal.signal_cb) {
            uv_signal_stop(&g_srv.signal);
            g_srv.signal.signal_cb = NULL;
        }
        if (g_srv.aio.asyncio) {
            aio_loop_shutdown(&g_srv.aio);
        }
        uv_close((uv_handle_t *)&g_srv.svc_timer, NULL);
        for (int idx = 0; idx < g_srv.servers_num; idx++) {
            uv_close((uv_handle_t *)SERVER(idx), NULL);
        }
        for (int num = 0; num < 10; num++) {
            uv_run(g_srv.loop, UV_RUN_NOWAIT);  // let libuv process close callbacks
        }
        uv_loop_close(g_srv.loop);
    }
    if (g_srv.aio.asyncio) {
        asyncio_free(&g_srv.aio, false);
    }
    for (int idx = 0; idx < g_srv.servers_num; idx++) {
        server_t * server = SERVER(idx);
        if (server) {
            Py_XDECREF(server->root_path.obj);
            Py_XDECREF(server->def_env);
            Py_XDECREF(server->def_scope);
        }
    }
    tls_server_free(NULL);
    g_srv_inited = 0;
    memset(&g_srv, 0, sizeof(g_srv));
    Py_RETURN_NONE;
}
