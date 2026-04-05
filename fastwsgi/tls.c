#include "tls.h"
#include "logx.h"
#include "server.h"

/*
 * Size of a single chunk when reading from MemoryBIO.read() and SSLObject.read().
 * SSLObject.read() returns at most one TLS record per call (no more than 16KB).
 * Use a slightly larger buffer for safety.
 */
#define TLS_READ_CHUNK  (32 * 1024)

// TLS record content types (RFC 8446 and predecessors)
typedef enum {
    TLS_CONTENT_CHANGE_CIPHER_SPEC = 20,  // 0x14 = Change cipher spec
    TLS_CONTENT_ALERT              = 21,  // 0x15 = Alert
    TLS_CONTENT_HANDSHAKE          = 22,  // 0x16 = Handshake
    TLS_CONTENT_APPLICATION_DATA   = 23,  // 0x17 = Application data
    TLS_CONTENT_HEARTBEAT          = 24,  // 0x18 = Heartbeat (RFC 6520)
} tls_content_type_t;

// ProtocolVersion values for TLS/SSL (RFC 8446 and predecessors)
typedef enum {
    TLS_VERSION_SSL_3_0  = 0x0300,  // SSL 3.0 (deprecated, insecure)
    TLS_VERSION_1_0      = 0x0301,  // TLS 1.0 (deprecated, insecure)
    TLS_VERSION_1_1      = 0x0302,  // TLS 1.1 (not recommended)
    TLS_VERSION_1_2      = 0x0303,  // TLS 1.2 (widely used)
    TLS_VERSION_1_3      = 0x0304,  // TLS 1.3 (latest)
} tls_protocol_version_t;

PRAGMA_PACK_1
typedef struct ATTR_PACKED {
    uint8_t  content_type;  // tls_content_type_t
    uint16_t version;       // 0x0303 => TLS 1.2 and 1.3 
    uint16_t length;        // size of encrypted data
} tls_header_t;
PRAGMA_PACK_DEF

/* -----------------------------------------------------------------------
 * Helper functions
 * --------------------------------------------------------------------- */

/*
 * Read all available data from MemoryBIO into xbuf_t.
 * Uses MemoryBIO.read(n) => bytes.
 * Returns total number of bytes read or neg value on error.
 */
static
int bio_drain_to_xbuf(PyObject * bio, xbuf_t * out)
{
    int hr = -1;
    int total = 0;
    PyObject * chunk = NULL;
    PyObject * read_method = NULL;
    PyObject * size_arg = NULL;

    read_method = PyObject_GetAttrString(bio, "read");
    LOGe_IF(!read_method, "%s: failed to get MemoryBIO.read method", __func__);
    FIN_IF(!read_method, -1);
    size_arg = PyLong_FromLong(TLS_READ_CHUNK);
    FIN_IF(!size_arg, -1);
    while (1) {
        Py_XDECREF(chunk);
        chunk = PyObject_CallFunctionObjArgs(read_method, size_arg, NULL);
        LOGe_IF(!chunk, "%s: MemoryBIO.read() returned an error", __func__);
        FIN_IF(!chunk, -2);
        if (!PyBytes_Check(chunk)) {
            LOGe("%s: MemoryBIO.read() returned non-bytes object", __func__);
            FIN(-3);
        }
        Py_ssize_t chunk_size = PyBytes_GET_SIZE(chunk);
        FIN_IF(chunk_size == 0, 0);  // BIO is empty => all data has been read
        if (xbuf_add(out, PyBytes_AS_STRING(chunk), chunk_size) < 0) {
            LOGe("%s: xbuf_add failed (out of memory)", __func__);
            FIN(-4);
        }
        total += (int)chunk_size;
    }
fin:
    Py_XDECREF(chunk);
    Py_XDECREF(read_method);
    Py_XDECREF(size_arg);
    return (hr == 0) ? total : hr;
}

/*
 * Write data into MemoryBIO using bio.write(data).
 * Returns 0 on success, neg value on error.
 */
static
Py_ssize_t bio_write(PyObject * bio, const char * data, Py_ssize_t size)
{
    int hr = 0;
    if (size > 0) {
        PyObject * pdata = PyBytes_FromStringAndSize(data, size);
        if (!pdata) {
            LOGe("%s: PyBytes_FromStringAndSize failed", __func__);
            return -1;
        }
        PyObject * ret = PyObject_CallMethod(bio, "write", "O", pdata);
        Py_DECREF(pdata);
        if (!ret) {
            LOGe("%s: MemoryBIO.write() returned an error", __func__);
            return -2;
        }
        hr = PyLong_AsSsize_t(ret);
        Py_DECREF(ret);
    }
    return hr;
}

/*
 * Check whether the current Python exception is an instance of the given class.
 * If yes => clear the exception and return -1.
 */
static
int check_and_clear_exc(PyObject * exc_class)
{
    if (PyErr_Occurred()) {
        if (PyErr_ExceptionMatches(exc_class)) {
            PyErr_Clear();
            return -1;
        }
    }
    return 0;
}

static
void pyerr_clear(void)
{
    //PyErr_Print();
    PyErr_Clear();
}

/* -----------------------------------------------------------------------
 * Server API
 * --------------------------------------------------------------------- */

int tls_server_init_all(void)
{
    int hr = -1;
    int total = 0;
    for (int idx = 0; idx < g_srv.servers_num; idx++) {
        hr = tls_server_init(idx);
        FIN_IF(hr < 0, hr);
        if (hr == 1)
            total++;
    }
    return total;  // OK
fin:
    tls_server_free(NULL);  // free all
    return hr;
}

int tls_server_init(int srv_idx)
{
    int hr = -1700;
    server_t * server = SERVER(srv_idx);
    tls_server_t * stls = &server->tls;
    
    PyObject * ssl_mod = NULL;
    PyObject * ctx = NULL;
    PyObject * ssl_ctx_class = NULL;
    PyObject * proto_enum = NULL;
    PyObject * proto_val = NULL;
    PyObject * ret = NULL;

    PyObject * load_cert_chain = PyUnicode_FromString("load_cert_chain");
    PyObject * load_verify_locations = PyUnicode_FromString("load_verify_locations");
    PyObject * paths[3] = { NULL };
    PyObject * certfile = NULL;  // path to PEM certificate file (including chain)
    PyObject * keyfile  = NULL;  // path to PEM private key file (NULL = > same file as certfile)
    PyObject * ca_certs = NULL;  // path to CA bundle for client certificate verification (NULL = > not required)

    int rc = get_obj_attr_list_tup(g_srv.pysrv, "tls_list", -1, NULL, 0);
    FIN_IF(rc == -41, 0);     // attr tls_list is None => skip
    FIN_IF(rc == 0, 0);       // attr tls_list is empty List => skip
    FIN_IF(rc == -1, -1703);  // attr tls_list is not List => error
    FIN_IF(rc < 1, -1704);    // other error on parsing
    FIN_IF(rc > HTTP_SERVERS_MAX, -1704);
    rc = get_obj_attr_list_tup(g_srv.pysrv, "tls_list", srv_idx, paths, COUNTOF(paths));
    FIN_IF(rc == -42, 0);     // tls_list[srv_idx] is None => skip
    FIN_IF(rc == -11, -1705); // tls_list is not Tuple => error
    FIN_IF(rc < 1, -1706);    // other error on parsing
    certfile = paths[0];
    FIN_IF(!certfile || certfile == Py_None, 0);  // TLS not used => skip
    FIN_IF(certfile && !PyUnicode_CheckExact(certfile), -1707);
    keyfile = (!paths[1] || paths[1] == Py_None) ? NULL : paths[1];
    FIN_IF(keyfile && !PyUnicode_CheckExact(keyfile), -1708);
    ca_certs = (!paths[2] || paths[2] == Py_None) ? NULL : paths[2];
    FIN_IF(ca_certs && !PyUnicode_CheckExact(ca_certs), -1709);

    memset(stls, 0, sizeof(*stls));

    ssl_mod = g_srv.ssl_module;
    if (!ssl_mod) {
        ssl_mod = PyImport_ImportModule("ssl");
        LOGe_IF(!ssl_mod, "%s: failed to import ssl module", __func__);
        FIN_IF(!ssl_mod, -1702);
        g_srv.ssl_module = ssl_mod;
    }

    // Create SSLContext with PROTOCOL_TLS_SERVER.
    // ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    proto_enum = PyObject_GetAttrString(ssl_mod, "PROTOCOL_TLS_SERVER");
    LOGe_IF(!proto_enum, "%s: ssl.PROTOCOL_TLS_SERVER not found", __func__);
    FIN_IF(!proto_enum, -1715);

    ssl_ctx_class = PyObject_GetAttrString(ssl_mod, "SSLContext");
    LOGe_IF(!ssl_ctx_class, "%s: failed to get SSLContext class", __func__);
    FIN_IF(!ssl_ctx_class, -1720);

    ctx = PyObject_CallFunctionObjArgs(ssl_ctx_class, proto_enum, NULL);
    LOGe_IF(!ctx, "%s: failed to create ssl.SSLContext", __func__);
    FIN_IF(!ctx, -1725);

    stls->ctx = ctx;
    Py_INCREF(ctx);

    // ctx.load_cert_chain(certfile, keyfile)
    if (keyfile) {
        //ret = PyObject_CallMethod(ctx, "load_cert_chain", "ss", certfile, keyfile);
        ret = PyObject_CallMethodObjArgs(ctx, load_cert_chain, certfile, keyfile, NULL);
    } else {
        //ret = PyObject_CallMethod(ctx, "load_cert_chain", "s", certfile);
        ret = PyObject_CallMethodObjArgs(ctx, load_cert_chain, certfile, NULL);
    }
    LOGe_IF(!ret, "%s: ctx.load_cert_chain() failed => check certificate and key paths", __func__);
    FIN_IF(!ret, -1730);
    Py_CLEAR(ret);

    // If CA bundle is provided => enable client certificate verification
    if (ca_certs) {
        //ret = PyObject_CallMethod(ctx, "load_verify_locations", "s", ca_certs);
        ret = PyObject_CallMethodObjArgs(ctx, load_verify_locations, ca_certs, NULL);
        LOGe_IF(!ret, "%s: ctx.load_verify_locations() failed", __func__);
        FIN_if(!ret, -1750);
        Py_CLEAR(ret);
        // ctx.verify_mode = ssl.CERT_REQUIRED
        PyObject * cert_req = PyObject_GetAttrString(ssl_mod, "CERT_REQUIRED");
        if (cert_req) {
            PyObject_SetAttrString(ctx, "verify_mode", cert_req);
            Py_DECREF(cert_req);
        }
    }
    // Cache frequently used objects for faster access in hot path
    stls->wrap_bio           = PyObject_GetAttrString(ctx, "wrap_bio");
    stls->MemoryBIO          = PyObject_GetAttrString(ssl_mod, "MemoryBIO");
    stls->SSLWantReadError   = PyObject_GetAttrString(ssl_mod, "SSLWantReadError");
    stls->SSLZeroReturnError = PyObject_GetAttrString(ssl_mod, "SSLZeroReturnError");
    stls->SSLError           = PyObject_GetAttrString(ssl_mod, "SSLError");

    if (!stls->wrap_bio || !stls->MemoryBIO) {
        LOGe("%s: failed to fetch ssl module attributes", __func__);
        FIN(-1770);
    }
    if (!stls->SSLWantReadError || !stls->SSLZeroReturnError || !stls->SSLError) {
        LOGe("%s: failed to fetch ssl module attributes", __func__);
        FIN(-1771);
    }
    stls->enabled = 1;
    LOGn("%s: TLS successfully initialized with certfile = %s", __func__, PyUnicode_AsUTF8(certfile));
    hr = 1;  // OK
fin:
    Py_XDECREF(ctx);
    Py_XDECREF(ssl_ctx_class);
    Py_XDECREF(proto_enum);
    Py_XDECREF(load_cert_chain);
    Py_XDECREF(load_verify_locations);
    if (hr < 0) {
        PyErr_Format(PyExc_Exception, "TLS initialization error. ErrCode = %d", hr);
        tls_server_free(server);
    }
    return hr;
}

void tls_server_free(server_t * server)
{
    for (int idx = 0; idx < g_srv.servers_num; idx++) {
        server_t * srv = SERVER(idx);
        if ((server && server == srv) || (server == NULL)) {
            tls_server_t * stls = &srv->tls;
            Py_CLEAR(stls->SSLError);
            Py_CLEAR(stls->SSLZeroReturnError);
            Py_CLEAR(stls->SSLWantReadError);
            Py_CLEAR(stls->MemoryBIO);
            Py_CLEAR(stls->wrap_bio);
            Py_CLEAR(stls->ctx);
            stls->enabled = 0;
        }
    }
    if (server == NULL) {
        Py_CLEAR(g_srv.ssl_module);
    }
}

/* -----------------------------------------------------------------------
 * Client API
 * --------------------------------------------------------------------- */

int tls_client_init(client_t * client, const char * server_hostname)
{
    int hr = -1;
    int rc;
    PyObject * bio_in  = NULL;
    PyObject * bio_out = NULL;
    PyObject * ssl_obj = NULL;

    tls_server_t * stls = &client->server->tls;
    tls_client_t * tls = &client->tls;
    memset(tls, 0, sizeof(tls_client_t));

    if (!stls->enabled) {
        LOGe("%s: server TLS context is not initialized", __func__);
        return -1;
    }
    // Create two MemoryBIO objects => incoming and outgoing
    bio_in = PyObject_CallObject(stls->MemoryBIO, NULL);
    LOGe_IF(!bio_in, "%s: failed to create incoming MemoryBIO", __func__);
    FIN_IF(!bio_in, -2);

    bio_out = PyObject_CallObject(stls->MemoryBIO, NULL);
    LOGe_IF(!bio_out, "%s: failed to create outgoing MemoryBIO", __func__);
    FIN_IF(!bio_out, -3);

    PyObject * srv_side = Py_True;
    // Create SSLObject via SSLContext.wrap_bio(bio_in, bio_out, server_side=True).
    if (server_hostname && strlen(server_hostname) > 0) {
        // The wrap_bio method is stored separately as a bound method
        ssl_obj = PyObject_CallFunction(stls->wrap_bio, "OOOs", bio_in, bio_out, srv_side, server_hostname);
    } else {
        ssl_obj = PyObject_CallFunction(stls->wrap_bio, "OOO", bio_in, bio_out, srv_side);
    }
    LOGe_IF(!ssl_obj, "%s: SSLContext.wrap_bio() failed", __func__);
    FIN_if(!ssl_obj, -5, pyerr_clear());

    rc = xbuf_init(&tls->enc_out, NULL, 4 * 1024);
    LOGe_IF(rc, "%s: failed to allocate enc_out buffer", __func__);
    FIN_IF(rc, -11);

    rc = xbuf_init(&tls->plain_in, NULL, 4 * 1024);
    LOGe_IF(rc, "%s: failed to allocate plain_in buffer", __func__);
    FIN_IF(rc, -12);

    tls->enabled = 1;
    hr = 0;
fin:
    tls->bio_in  = bio_in;
    tls->bio_out = bio_out;
    tls->ssl_obj = ssl_obj;
    tls->hs_state = TLS_HS_PENDING;
    tls->hs_readed_bytes = 0;
    if (hr) {
        tls_client_free(client);
    }
    return hr;
}

void tls_client_free(client_t * client)
{
    if (1) {
        Py_CLEAR(client->tls.ssl_obj);
        Py_CLEAR(client->tls.bio_in);
        Py_CLEAR(client->tls.bio_out);
        xbuf_free(&client->tls.enc_out);
        xbuf_free(&client->tls.plain_in);
        client->tls.hs_readed_bytes = 0;
        client->tls.hs_state = TLS_HS_ERROR;
        client->tls.writing = 0;
    }
}

/* -----------------------------------------------------------------------
 * Data flow
 * --------------------------------------------------------------------- */

int tls_feed_encrypted(client_t * client, const char * data, Py_ssize_t size)
{
    int hr = 0;
    tls_client_t * tls = &client->tls;
    if (tls->bio_in && data && size > 0) {
        Py_ssize_t wsz = bio_write(tls->bio_in, data, size);
        LOGe_IF(wsz < 0, "%s: failed to write data into input BIO", __func__);
        FIN_IF(wsz < 0, (int)wsz);
        LOGe_IF(wsz != size, "%s: failed to WRITE data into input BIO", __func__);
        FIN_IF(wsz != size, -9);
    }
fin:
    return hr;
}

tls_hs_state_t tls_do_handshake(client_t * client)
{
    tls_server_t * stls = &client->server->tls;
    tls_client_t * tls = &client->tls;

    if (!tls->ssl_obj || tls->hs_state == TLS_HS_ERROR) {
        tls->hs_state = TLS_HS_ERROR;
        return TLS_HS_ERROR;
    }
    if (tls->hs_state == TLS_HS_DONE) {
        return TLS_HS_DONE;
    }
    // ssl_obj.do_handshake()
    PyObject * ret = PyObject_CallMethod(tls->ssl_obj, "do_handshake", NULL);
    if (ret) {
        Py_DECREF(ret);
        tls->hs_state = TLS_HS_DONE;
        LOGn("%s: TLS handshake successfully completed", __func__);
        return TLS_HS_DONE;
    }
    // We're still waiting for data from the client - that's normal.
    if (check_and_clear_exc(stls->SSLWantReadError)) {
        tls->hs_state = TLS_HS_PENDING;
        return TLS_HS_PENDING;
    }
    LOGe("%s: TLS handshake error", __func__);
    //PyErr_Print();
    PyErr_Clear();
    tls->hs_state = TLS_HS_ERROR;
    return TLS_HS_ERROR;
}

int tls_read_decrypted(client_t * client)
{
    int hr = -1;
    int total = 0;
    PyObject * chunk = NULL;
    tls_server_t * stls = &client->server->tls;
    tls_client_t * tls = &client->tls;

    if (!tls->ssl_obj) {
        return -1;
    }
    xbuf_reset(&tls->plain_in);
    if (tls->hs_state != TLS_HS_DONE) {
        return 0;  // data cannot be read until the handshake is complete
    }
    while (1) {
        Py_XDECREF(chunk);
        // ssl_obj.read(TLS_READ_CHUNK) => bytes
        // We read in a loop while there is data.
        chunk = PyObject_CallMethod(tls->ssl_obj, "read", "i", TLS_READ_CHUNK);
        if (!chunk) {
            if (check_and_clear_exc(stls->SSLWantReadError)) {
                // SSLWantReadError => The input BIO is empty, there is no more data
                FIN(0);
            }
            if (check_and_clear_exc(stls->SSLZeroReturnError)) {
                LOGn("%s: received TLS close_notify from client (data size = %d)", __func__, total);
                tls->close_notify = 1;  // graceful close signal
                FIN(0);
            }
            LOGe("%s: error reading from SSLObject", __func__);
            //PyErr_Print();
            PyErr_Clear();
            FIN(-3);
        }
        if (!PyBytes_Check(chunk)) {
            LOGe("%s: SSLObject.read() returned non-bytes", __func__);
            FIN(-4);
        }
        Py_ssize_t chunk_size = PyBytes_GET_SIZE(chunk);
        FIN_IF(chunk_size == 0, 0);  // BIO is empty => all data has been read
        int rc = xbuf_add(&tls->plain_in, PyBytes_AS_STRING(chunk), chunk_size);
        LOGe_IF(rc <= 0, "%s: xbuf_add failed while accumulating decrypted data (rc = %d) chunk_size = %d", __func__, rc, (int)chunk_size);
        FIN_IF(rc <= 0, -5);
        total += (int)chunk_size;
    }
fin:
    Py_XDECREF(chunk);
    return (hr == 0) ? total : hr;
}

int tls_encrypt(client_t * client, const char * data, Py_ssize_t size)
{
    int hr = 0;
    PyObject * pdata = NULL;
    PyObject * ret = NULL;
    tls_server_t * stls = &client->server->tls;
    tls_client_t * tls = &client->tls;

    if (!tls->ssl_obj || size <= 0) {
        return 0;
    }
    if (tls->hs_state != TLS_HS_DONE) {
        LOGe("%s: attempt to encrypt before handshake completion", __func__);
        return -1;
    }
    /*
     * ssl_obj.write(data) => int (number of bytes written).
     * SSLObject.write() is not required to write all bytes in one call.
     * We write in a loop until we've written everything.
     */
    Py_ssize_t written = 0;
    while (written < size) {
        Py_CLEAR(ret);
        Py_CLEAR(pdata);
        pdata = PyBytes_FromStringAndSize(data + written, size - written);
        LOGe_IF(!pdata, "%s: PyBytes_FromStringAndSize failed", __func__);
        FIN_IF(!pdata, -3);

        ret = PyObject_CallMethod(tls->ssl_obj, "write", "O", pdata);
        LOGe_IF(!ret, "%s: SSLObject.write() returned an error", __func__);
        FIN_if(!ret, -4, pyerr_clear());
        if (!PyLong_Check(ret)) {
            LOGe("%s: SSLObject.write() returned non-int", __func__);
            FIN(-5);
        }
        Py_ssize_t nret = (Py_ssize_t)PyLong_AsSsize_t(ret);
        LOGe_IF(nret <= 0, "%s: SSLObject.write() wrote 0 bytes", __func__);
        FIN_IF(nret <= 0, -6);
        written += nret;
    }
    // upload the encrypted result from bio_out to enc_out
    hr = tls_drain_to_enc_out(client);
fin:
    Py_XDECREF(pdata);
    Py_XDECREF(ret);
    return hr;
}

Py_ssize_t tls_has_encrypted_output(client_t * client)
{
    tls_client_t * tls = &client->tls;
    if (tls->bio_out) {
        // MemoryBIO.pending => number of bytes available to read
        PyObject * pending = PyObject_GetAttrString(tls->bio_out, "pending");
        if (pending) {
            Py_ssize_t size = PyLong_AsSsize_t(pending);
            Py_DECREF(pending);
            return (size < 0) ? 0 : size;
        }
    }
    return -1;
}

int tls_drain_to_enc_out(client_t * client)
{
    tls_client_t * tls = &client->tls;
    if (tls->bio_out) {
        xbuf_reset(&tls->enc_out);
        int size = bio_drain_to_xbuf(tls->bio_out, &tls->enc_out);
        LOGe_IF(size < 0, "%s: failed to read encrypted data from bio_out", __func__);
        return size;
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * TLS: sending encrypted data via separate write_req
 * --------------------------------------------------------------------- */

/*
 * TLS write completion callback.
 * Frees tls_wreq and triggers next step if needed.
 */
void tls_write_cb(uv_write_t * req, int status)
{
    write_req_t * wreq = (write_req_t *)req;
    client_t * client = (client_t *)wreq->client;
    g_srv.num_writes--;
    before_loop_callback(client);
    update_log_prefix(client);

    wreq->client = NULL;  // mark as free
    client->tls.writing = 0;
    
    if (status != 0) {
        LOGe("%s: TLS write error: %s", __func__, uv_strerror(status));
        close_connection(client);
        return;
    }
    if (client->tls.hs_state == TLS_HS_PENDING) {
        LOGd("%s: handshake waiting for client data", __func__);
        stream_read_start(client);
        return;
    }
    // Handshake complete, response data sent.
    // Let's check if there's more data in enc_out (unlikely, but possible)
    LOGd("%s: TLS write completed successfully", __func__);
    // we need to reset response.write_req and continue as write_cb:
    write_req_t * resp_wreq = &client->response.write_req;
    if (resp_wreq->client == client) {
        resp_wreq->client = NULL;
        if (!client->request.keep_alive || !g_srv.allow_keepalive) {
            close_connection(client);
            return;
        }
        reset_response_body(client);
        if (client->pipeline.status == PS_RESTING) {
            stream_read_start(client);
        }
    }
}

/*
 * Flush encrypted data from tls.enc_out via libuv.
 * Returns 0 on success, neg value on error.
 */
int tls_flush_enc_out(client_t * client)
{
    tls_client_t * tls = &client->tls;
    if (tls->enc_out.size <= 0) {
        return 0;  // nothing to send
    }
    write_req_t * wreq = &client->tls_wreq;
    if (wreq->client) {
        // The previous TLS record hasn't completed yet. This shouldn't happen with proper logic, but let's protect ourselves.
        LOGw("%s: tls_wreq busy, skipping flush", __func__);
        return 0;
    }
    tls->writing = 1;
    wreq->client = client;
    wreq->bufs[0].base = tls->enc_out.data;
    wreq->bufs[0].len = (unsigned int)tls->enc_out.size;

    stream_read_stop(client);

    int rc = uv_write( (uv_write_t *)wreq, (uv_stream_t *)client, wreq->bufs, 1, tls_write_cb);
    if (rc != 0) {
        LOGe("%s: uv_write returned error: %s", __func__, uv_strerror(rc));
        client->tls_wreq.client = NULL;
        tls->writing = 0;
        return -1;
    }
    g_srv.num_writes++;
    return 0;
}

int tls_stream_write(client_t * client)
{
    // Assembling plaintext: headers + body
    xbuf_t plain;
    xbuf_init(&plain, NULL, 0);
    write_req_t * wreq = &client->response.write_req;

    if (client->response.headers_size > 0) {
        xbuf_add(&plain, client->head.data, client->response.headers_size);
    }
    for (size_t i = 0; i < client->response.body_chunk_num; i++) {
        Py_ssize_t sz = PyBytes_GET_SIZE(client->response.body[i]);
        xbuf_add(&plain, PyBytes_AS_STRING(client->response.body[i]), sz);
    }
    if (client->response.chunked == 1) {
        xbuf_add(&plain, "\r\n", 2);
    }
    if (plain.size == 0) {
        xbuf_free(&plain);
        return CA_OK;
    }
    LOGd("%s: Encrypting %d bytes", __func__, plain.size);
    int rc = tls_encrypt(client, plain.data, (Py_ssize_t)plain.size);
    xbuf_free(&plain);
    if (rc < 0) {
        LOGe("%s: tls_encrypt failed", __func__);
        return CA_SHUTDOWN;
    }
    stream_read_stop(client);
    wreq->client = client;  // mark that a "logical" writing is in progress
    if (tls_flush_enc_out(client) < 0) {
        wreq->client = NULL;
        return CA_SHUTDOWN;
    }
    return CA_OK;
}

static
ssize_t tls_find_app_pkt(xbuf_t * rbuf)
{
    ssize_t hr = -1;
    FIN_IF(rbuf->size < (int)sizeof(tls_header_t), -1);  // not found
    const char * pos = rbuf->data;
    const char * end = pos + rbuf->size;
    while (1) {
        FIN_IF(pos + sizeof(tls_header_t) > end, -1);  // not found
        const tls_header_t * hdr = (const tls_header_t *)pos;
        FIN_IF(ntohs(hdr->version) < TLS_VERSION_SSL_3_0 || ntohs(hdr->version) > TLS_VERSION_1_3, -11);
        FIN_IF(hdr->content_type != 0 && (hdr->content_type < 20 || hdr->content_type > 24), -12);
        if (hdr->content_type == TLS_CONTENT_APPLICATION_DATA) {
            return (ssize_t)((size_t)pos - (size_t)rbuf->data);
        }
        pos += sizeof(tls_header_t) + hdr->length;
    }
fin:
    return hr;
}

int tls_read_cb(client_t * client, ssize_t nread, uv_buf_t * buf)
{
    int hr = CA_OK;
    tls_client_t * tls = &client->tls;
    
    if (client->tls.close_notify) {
        FIN(CA_CLOSE);
    }
    // We feed encrypted bytes into the incoming BIO
    int rc = tls_feed_encrypted(client, buf->base, (Py_ssize_t)nread);
    if (rc < 0) {
        LOGe("%s: error on tls_feed_encrypted (errcode = %d)", __func__, rc);
        FIN(CA_SHUTDOWN);
    }
    if (tls->hs_state != TLS_HS_DONE) {
        tls->hs_readed_bytes += (size_t)nread;
    }
    // The handshake isn't over yet => let's continue it
    if (tls->hs_state != TLS_HS_DONE) {
        tls_hs_state_t hs = tls_do_handshake(client);
        // We check whether data has appeared to be sent to the client during the handshake.
        if (tls_has_encrypted_output(client) > 0) {
            tls_drain_to_enc_out(client);
            tls_flush_enc_out(client);
        }
        if (hs == TLS_HS_ERROR) {
            LOGe("%s: handshake failed => closing the connection", __func__);
            FIN(CA_CLOSE);
        }
        if (hs != TLS_HS_DONE) {
            FIN(100 + TLS_HS_PENDING);
        }
        tls->hs_state = TLS_HS_DONE;
        LOGn("%s: Handshake complete => waiting for HTTP data", __func__);
    }
    // Handshake complete => decrypting data
    rc = tls_read_decrypted(client);
    if (rc < 0) {
        LOGe("%s: decryption error (errcode = %d)", __func__, rc);
        FIN(CA_CLOSE);
    }
    LOGd("%s: [TLS] %d bytes decrypted", __func__, rc);
    hr = CA_OK;
    // Replace buf with the decrypted data from tls.plain_in
    // The following code handles this data as with regular plaintext.
    // plain_in.data is owned by tls - no need to free it with free_read_buffer
    buf->base = client->tls.plain_in.data;
    buf->len  = client->tls.plain_in.size;
fin:
    return hr;
}
