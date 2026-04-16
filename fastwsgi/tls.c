#include "tls.h"
#include "logx.h"
#include "server.h"

#include <string.h>
#include <stdlib.h>

// Temporary stack buffer used when draining the write BIO in a loop
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
 * Read everything currently in bio_out and append it to `out`.
 * Returns total bytes appended or neg value on error.
 */
static
int bio_drain_to_xbuf(BIO * bio_out, xbuf_t * out)
{
    int hr = -1;
    int total = 0;
    while (1) {
        long size = BIO_pending(bio_out);
        LOGe_IF(size < 0, "%s: BIO_pending() returned an error = %d", __func__, (int)size);
        FIN_IF(size < 0, -2);
        FIN_IF(size >= INT_MAX / 2, -3);
        FIN_IF(out->size + size >= INT_MAX / 2, -3);
        char * chunk = xbuf_expand(out, (size_t)size + 1);
        LOGe_IF(!chunk, "%s: xbuf_expand failed (out of memory)", __func__);
        FIN_IF(!chunk, -4);
        size = (size == 0) ? 1 : size;
        int rc = g_ssl.BIO_read(bio_out, chunk, (int)size);
        FIN_IF(rc < 0 && BIO_should_retry(bio_out), 0);  // BIO not ready or empty
        LOGe_IF(rc < 0, "%s: BIO_read() returned an error = %d", __func__, rc);
        FIN_IF(rc < 0, -9);
        FIN_IF(rc == 0, 0);  // EOF: BIO is empty => all data has been read 
        out->size += rc;
        total += rc;
    }
fin:
    return (hr == 0) ? total : hr;
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

    const char * ssl_certfile = PyUnicode_AsUTF8(certfile);
    const char * ssl_keyfile  = keyfile  ? PyUnicode_AsUTF8(keyfile) : ssl_certfile;
    const char * ssl_ca_certs = ca_certs ? PyUnicode_AsUTF8(ca_certs) : NULL;

    FIN_IF(ssl_certfile[0] == 0, 0);  // TLS not used => skip

    memset(stls, 0, sizeof(*stls));

    // Load the OpenSSL shared library (idempotent)
    rc = ssl_lib_init();
    LOGe_IF(rc, "%s: ssl_lib_init() failed with error = %d", __func__, rc);
    FIN_IF(rc, -1710);

    // Create the server SSL_CTX
    SSL_CTX * ctx = g_ssl.SSL_CTX_new(g_ssl.TLS_server_method());
    LOGe_IF(!ctx, "%s: SSL_CTX_new(TLS_server_method) failed", __func__);
    FIN_IF(!ctx, -1720);
    stls->ctx = ctx;

    g_ssl.SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    g_ssl.SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv3);
    g_ssl.SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
    g_ssl.SSL_CTX_set_options(ctx, SSL_OP_PRIORITIZE_CHACHA);

    // Load certificate chain
    rc = g_ssl.SSL_CTX_use_certificate_chain_file(ctx, ssl_certfile);
    LOGe_IF(rc != 1, "%s: failed to load certificate chain from '%s'", __func__, ssl_certfile);
    FIN_if(rc != 1, -1730, ssl_log_errors("SSL_CTX_load_cert_chain"));

    // Load private key (may be in the same file as the certificate)
    rc = g_ssl.SSL_CTX_use_PrivateKey_file(ctx, ssl_keyfile, SSL_FILETYPE_PEM);
    LOGe_IF(rc != 1, "%s: failed to load private key from '%s'", __func__, ssl_keyfile);
    FIN_if(rc != 1, -1735, ssl_log_errors("SSL_CTX_load_priv_key"));

    // Verify that certificate and key match
    rc = g_ssl.SSL_CTX_check_private_key(ctx);
    LOGe_IF(rc != 1, "%s: certificate / private-key mismatch", __func__);
    FIN_if(rc != 1, -1740, ssl_log_errors("SSL_CTX_check_priv_key"));

    // If CA bundle is provided => enable client certificate verification
    if (ssl_ca_certs) {
        rc = g_ssl.SSL_CTX_load_verify_locations(ctx, ssl_ca_certs, NULL);
        LOGe_IF(rc != 1, "%s: failed to load CA certs from '%s'", __func__, ssl_ca_certs);
        FIN_if(rc != 1, -1750, ssl_log_errors("SSL_CTX_load_ca"));
        g_ssl.SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
        LOGn("%s: client certificate verification enabled (CA: %s)", __func__, ssl_ca_certs);
    }
    stls->enabled = 1;
    LOGn("%s: TLS successfully initialized with certfile = %s", __func__, ssl_certfile);
    hr = 1;  // OK
fin:
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
            SSL_CTX_FREE(stls->ctx);
            stls->enabled = 0;
        }
    }
    if (server == NULL) {
        ssl_lib_free();
    }
}

/* -----------------------------------------------------------------------
 * Client API
 * --------------------------------------------------------------------- */

int tls_client_init(client_t * client, const char * server_hostname)
{
    int hr = -1;
    int rc;
    (void)server_hostname;  // server_hostname is unused server-side; kept for API symmetry
    BIO * bio_in = NULL;
    BIO * bio_out = NULL;

    tls_server_t * stls = &client->server->tls;
    tls_client_t * tls = &client->tls;
    memset(tls, 0, sizeof(tls_client_t));

    if (!stls->enabled) {
        LOGe("%s: server TLS context is not initialized", __func__);
        return -1;
    }
    // Create the SSL object from the shared context
    tls->ssl_obj = g_ssl.SSL_new(stls->ctx);
    LOGe_IF(!tls->ssl_obj, "%s: SSL_new() failed", __func__);
    FIN_IF(!tls->ssl_obj, -5);

    // Create two memory BIOs
    bio_in  = g_ssl.BIO_new(g_ssl.BIO_s_mem());
    bio_out = g_ssl.BIO_new(g_ssl.BIO_s_mem());
    LOGe_IF(!bio_in || !bio_out, "%s: BIO_new(mem) failed", __func__);
    FIN_IF(!bio_in || !bio_out, -6);

    // Since our BIO is non-blocking an empty BIO_read() does not indicate EOF, just that no data is currently available.
    // The SSL routines should retry the read, which we can achieve by calling BIO_set_retry_read().
    g_ssl.BIO_set_flags(bio_in, BIO_FLAGS_READ);
    BIO_set_mem_eof_return(bio_in, -1);   // for non-blocking mode
    g_ssl.BIO_set_flags(bio_out, BIO_FLAGS_READ);
    BIO_set_mem_eof_return(bio_out, -1);  // for non-blocking mode

    // Attach BIOs and put SSL into server (accept) mode.
    // Keep pointers for BIO_write / BIO_pending / BIO_read.
    // SSL_set_bio() transfers ownership of both BIOs to ssl.
    g_ssl.SSL_set_bio(tls->ssl_obj, bio_in, bio_out);
    tls->bio_in  = bio_in;
    tls->bio_out = bio_out;
    bio_in  = NULL;
    bio_out = NULL;
    g_ssl.SSL_set_accept_state(tls->ssl_obj);

    rc = xbuf_init(&tls->enc_out, NULL, 16 * 1024 + 64);
    LOGe_IF(rc, "%s: failed to allocate enc_out buffer", __func__);
    FIN_IF(rc, -11);

    rc = xbuf_init(&tls->plain_in, NULL, 16 * 1024 + 64);
    LOGe_IF(rc, "%s: failed to allocate plain_in buffer", __func__);
    FIN_IF(rc, -12);

    tls->enabled = 1;
    hr = 0;
fin:
    SSL_BIO_FREE(bio_in);
    SSL_BIO_FREE(bio_out);
    tls->hs_state = TLS_HS_PENDING;
    if (hr) {
        tls_client_free(client);
    }
    return hr;
}

void tls_client_free(client_t * client)
{
    if (1) {
        SSL_FREE(client->tls.ssl_obj);  // SSL_free() turn calls BIO_free() on both BIOs attached via SSL_set_bio()
        client->tls.bio_in  = NULL;
        client->tls.bio_out = NULL;
        xbuf_free(&client->tls.enc_out);
        xbuf_free(&client->tls.plain_in);
        client->tls.hs_state = TLS_HS_ERROR;
        client->tls.writing = 0;
    }
}

/* -----------------------------------------------------------------------
 * Data flow
 * --------------------------------------------------------------------- */

int tls_shutdown(SSL * ssl)
{
    int rc = g_ssl.SSL_shutdown(ssl);
    if (rc == 1) return  1;   /* complete */
    if (rc == 0) return  0;   /* waiting for peer close_notify */

    int err = g_ssl.SSL_get_error(ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        return 0;

    //ssl_log_errors("ssl_shutdown");
    return -1;
}

int tls_feed_encrypted(client_t * client, const char * data, ssize_t size)
{
    int hr = 0;
    tls_client_t * tls = &client->tls;
    if (tls->bio_in && data && size > 0) {
        int wsz = g_ssl.BIO_write(tls->bio_in, data, size);
        LOGe_IF(wsz < 0, "%s: failed to write data into input BIO", __func__);
        FIN_IF(wsz < 0, (int)wsz);
        LOGe_IF(wsz != (int)size, "%s: failed to WRITE data into input BIO", __func__);
        FIN_IF(wsz != (int)size, -9);
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
    int rc = g_ssl.SSL_do_handshake(tls->ssl_obj);
    if (rc == 1) {
        tls->hs_state = TLS_HS_DONE;
        LOGn("%s: TLS handshake successfully completed", __func__);
        return TLS_HS_DONE;
    }
    int err = g_ssl.SSL_get_error(tls->ssl_obj, rc);
    if (err == SSL_ERROR_WANT_READ) {
        // Want-read: need more bytes from the client
        tls->hs_state = TLS_HS_PENDING;
        return TLS_HS_PENDING;
    }
    LOGe("%s: TLS handshake failed (rc = %d, err = %d)", __func__, rc, err);
    tls->hs_state = TLS_HS_ERROR;
    return TLS_HS_ERROR;
}

int tls_read_decrypted(client_t * client)
{
    int hr = -1;
    int total = 0;
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
        char * chunk = xbuf_expand(&tls->plain_in, TLS_READ_CHUNK);
        LOGe_IF(!chunk, "%s: xbuf_expand failed (out of memory)", __func__);
        FIN_IF(!chunk, -4);
        int rc = g_ssl.SSL_read(tls->ssl_obj, chunk, TLS_READ_CHUNK);
        if (rc <= 0) {
            int err = g_ssl.SSL_get_error(tls->ssl_obj, rc);
            if (err == SSL_ERROR_WANT_READ) {
                // SSLWantReadError => The input BIO is empty, there is no more data
                FIN(0);
            }
            if (err == SSL_ERROR_ZERO_RETURN) {
                LOGn("%s: received TLS close_notify from client (data size = %d)", __func__, total);
                tls->close_notify = 1;  // graceful close signal
                FIN(0);
            }
            LOGe("%s: error reading from SSL_obj (rc = %d, err = %d)", __func__, rc, err);
            FIN(-5);
        }
        tls->plain_in.size += rc;
        total += rc;
    }
fin:
    return (hr == 0) ? total : hr;
}

int tls_encrypt(client_t * client, const char * data, ssize_t size, int chunk_idx)
{
    int hr = 0;
    tls_server_t * stls = &client->server->tls;
    tls_client_t * tls = &client->tls;

    if (!tls->ssl_obj || size <= 0) {
        return 0;
    }
    if (tls->hs_state != TLS_HS_DONE) {
        LOGe("%s: attempt to encrypt before handshake completion", __func__);
        return -1;
    }
    // SSL_write() may not consume all bytes in one call (e.g. when size
    // exceeds the maximum TLS record size of 16 KiB).  Loop until done.
    ssize_t written = 0;
    while (written < size) {
        int chunk_size = (size - written > 16384) ? 16384 : (int)(size - written);
        int rc = g_ssl.SSL_write(tls->ssl_obj, data + written, chunk_size);
        LOGe_IF(rc < 0, "%s: SSL_write() returned an error = %d", __func__, rc);
        FIN_IF(rc < 0, -4);
        LOGe_IF(rc == 0, "%s: SSL_write() wrote 0 bytes", __func__);
        FIN_IF(rc == 0, -6);
        written += rc;
    }
    // upload the encrypted result from bio_out to enc_out
    hr = tls_drain_to_enc_out(client, chunk_idx);
fin:
    return hr;
}

ssize_t tls_has_encrypted_output(client_t * client)
{
    tls_client_t * tls = &client->tls;
    if (tls->bio_out) {
        long size = BIO_pending(tls->bio_out);
        return (size < 0) ? 0 : (ssize_t)size;
    }
    return 0;
}

int tls_drain_to_enc_out(client_t * client, int chunk_idx)
{
    tls_client_t * tls = &client->tls;
    if (tls->bio_out) {
        if (chunk_idx == 0) {
            xbuf_reset(&tls->enc_out);
        }
        int size = bio_drain_to_xbuf(tls->bio_out, &tls->enc_out);
        LOGe_IF(size < 0, "%s: failed to read encrypted data from bio_out", __func__);
        return size;
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * TLS: sending encrypted data via separate write_req
 * --------------------------------------------------------------------- */

void tls_write_cb(uv_write_t * req, int status)
{
    int hr = -1;
    write_req_t * wreq = (write_req_t *)req;
    client_t * client = (client_t *)wreq->client;
    g_srv.num_writes--;
    client->tls.writing = 0;
    before_loop_callback(client);
    update_log_prefix(client);
    if (status != 0) {
        LOGe("%s: TLS write error: %s", __func__, uv_strerror(status));
        FIN(-10);
    }
    if (client->tls.hs_state == TLS_HS_PENDING) {
        LOGd("%s: handshake waiting for client data", __func__);
        stream_read_start(client);
        return;
    }
    // Handshake complete, response data sent.
    LOGd("%s: TLS write completed successfully", __func__);
    stream_read_start(client);
    return;  // OK
fin:
    if (hr <= 0) {
        close_connection(client);
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
    write_req_t * wreq = &client->response.write_req;
    if (tls->writing || client->state == CS_RESP_SEND) {
        // The previous TLS record hasn't completed yet. This shouldn't happen with proper logic, but let's protect ourselves.
        LOGe("%s: uv_write still busy => concurrent TLS flush detected", __func__);
        return -1;
    }
    wreq->bufs[0].len  = tls->enc_out.size;
    wreq->bufs[0].base = tls->enc_out.data;
    //stream_read_stop(client);
    //SET_CSTATE(CS_RESP_SEND);
    LOGd("%s: [TLS] send %d bytes", __func__, tls->enc_out.size);
    tls->writing = 1;
    g_srv.num_writes++;
    int rc = uv_write((uv_write_t *)wreq, (uv_stream_t *)client, wreq->bufs, 1, tls_write_cb);
    if (rc != 0) {
        g_srv.num_writes--;
        LOGe("%s: uv_write returned error: %s", __func__, uv_strerror(rc));
        tls->writing = 0;
        return -1;
    }
    return 0;
}

int tls_data_encode(client_t * client, uv_buf_t * bufs, int nbufs, int total_len)
{
    int hr = -1;
    tls_client_t * tls = &client->tls;
    FIN_IF(nbufs <= 0 || total_len < 0, -2);
    FIN_IF(!bufs, -3);
    FIN_IF(bufs[0].len == 0, -4);
    xbuf_reset(&tls->enc_out);
    int data_size = total_len + 512;
    if (data_size > tls->enc_out.capacity) {
        char * data = xbuf_resize(&tls->enc_out, data_size, 0);
        FIN_IF(!data, -5);
    }
    LOGd_IF(total_len > 0, "%s: Encrypting %d bytes...", __func__, total_len);
    LOGd_IF(!total_len, "%s: Encrypting ??? bytes...", __func__);
    int chunk_idx = -1;
    for (int idx = 0; idx < nbufs; idx++) {
        if (bufs[idx].len > 0) {
            chunk_idx++;
            int rc = tls_encrypt(client, bufs[idx].base, bufs[idx].len, chunk_idx);
            LOGe_IF(rc < 0, "%s: tls_encrypt() failed (rc = %d)", __func__, rc);
            FIN_IF(rc < 0, -10);
        }
    }
    hr = tls->enc_out.size;
fin:
    return hr;
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
        pos += sizeof(tls_header_t) + ntohs(hdr->length);
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
    int rc = tls_feed_encrypted(client, buf->base, nread);
    if (rc < 0) {
        LOGe("%s: error on tls_feed_encrypted (errcode = %d)", __func__, rc);
        FIN(CA_SHUTDOWN);
    }
    // The handshake isn't over yet => let's continue it
    if (tls->hs_state != TLS_HS_DONE) {
        tls_hs_state_t hs = tls_do_handshake(client);
        // We check whether data has appeared to be sent to the client during the handshake.
        if (tls_has_encrypted_output(client) > 0) {
            int size = tls_drain_to_enc_out(client, 0);
            FIN_IF(size <= 0, CA_SHUTDOWN);
            int rc = tls_flush_enc_out(client);
            FIN_IF(rc < 0, CA_SHUTDOWN);
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
