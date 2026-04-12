#ifndef FASTWSGI_TLS_H_
#define FASTWSGI_TLS_H_

#include "common.h"
#include "xbuf.h"
#include "libssl.h"

/*
 * TLS layer implementation based on ssl.MemoryBIO + SSLContext.wrap_bio().
 *
 * Workflow:
 *
 *   libuv recv  =>  encrypted bytes  => tls_feed_encrypted()
 *                                               |
 *                                       BIO_write(bio_in)
 *                                               |
 *                                       SSL_read() => plaintext => llhttp / app
 *
 *   app response => plaintext => SSL_write()
 *                                       |
 *                               (bio_out accumulates encrypted output)
 *                                       |
 *                                tls_drain_encrypted() => enc_out => libuv send
 *
 * Handshake:
 *   While hs_state == TLS_HS_PENDING, all data is routed to tls_do_handshake().
 *   On completion hs_state becomes TLS_HS_DONE and normal application data transfer begins.
 */

/* TLS handshake states */
typedef enum {
    TLS_HS_NOTUSED  = 0,   // TLS not used
    TLS_HS_PENDING  = 1,   // handshake is not yet completed
    TLS_HS_DONE     = 2,   // handshake successfully completed
    TLS_HS_ERROR    = 3,   // an error occurred
} tls_hs_state_t;

/* Main TLS state structure for a single client connection */
typedef struct {
    int              enabled;       // 1 if TLS is enabled; else 0
    SSL            * ssl_obj;       // the SSL object that drives the TLS state machine
    /*
     * Memory BIOs.
     * bio_in  - we BIO_write() encrypted network data here; OpenSSL reads it during SSL_read / SSL_do_handshake.
     * bio_out - OpenSSL BIO_write()s encrypted output here; we BIO_read() it and send over the network.
     *
     * IMPORTANT: both BIOs are owned by `ssl` after ssl_set_bios() is called (SSL_set_bio transfers ownership).
     * Do NOT call ssl_bio_free() on them explicitly — ssl_free() will release them together with the SSL object.
     * We keep these pointers only for BIO_write / BIO_pending / BIO_read.
     */
    BIO            * bio_in;           // read BIO  (network -> OpenSSL)
    BIO            * bio_out;          // write BIO (OpenSSL -> network)

    tls_hs_state_t   hs_state;         // Handshake state
    int              close_notify;     // 1 if received TLS close_notify from client

    /* Buffer for encrypted data waiting to be sent via libuv.
     * uv_write() does not copy data => the buffer must remain valid
     * until the write_cb callback is invoked.
     * Uses xbuf_t with dynamic resizing. */
    xbuf_t           enc_out;       // encrypted outgoing buffer

    /* plain_in - accumulates plaintext bytes produced by SSL_read().
     * We read in a loop (multiple TLS records per call) and store the
     * result here so the caller gets one contiguous buffer. */
    xbuf_t           plain_in;      // decrypted incoming buffer

    /* Flag: encrypted buffer is being sent via uv_write().
     * While a write is in progress, enc_out must not be modified. */
    int              writing;
} tls_client_t;

// Server TLS configuration (created once during server initialization)
typedef struct {
    int        enabled;             // 1 if TLS is enabled; else 0
    SSL_CTX  * ctx;                 // shared SSL_CTX - used to create per-connection SSL objects
} tls_server_t;

/* -----------------------------------------------------------------------
 * Server API
 * --------------------------------------------------------------------- */

/*
 * Initialize server TLS context.
 * Returns 0 when TLS not used, 1 on success, negative value on error
 */
int  tls_server_init(int srv_idx);

/*
* Initialize TLS context for all TCP servers.
* Returns number of inited tls servers, negative value on error
*/
int  tls_server_init_all(void);

/*
 * Free server TLS context resources.
 */
void tls_server_free(server_t * server);

/* -----------------------------------------------------------------------
 * Client API
 * --------------------------------------------------------------------- */

/*
 * Initialize TLS for a new client connection.
 * server_hostname - SNI hostname (unused on server side; kept for API symmetry)
 * Returns 0 on success.
 */
int  tls_client_init(client_t * client, const char * server_hostname);

/*
 * Free TLS client resources.
 */
void tls_client_free(client_t * client);

/*
 * Feed encrypted bytes received from the socket into the input BIO.
 * After calling this, tls_do_handshake() or tls_read_decrypted() must be called.
 * Returns 0 on success, neg value on error.
 */
int  tls_feed_encrypted(client_t * client, const char * data, ssize_t size);

/*
 * Perform one step of the TLS handshake.
 * Returns:
 *   TLS_HS_DONE     - handshake completed
 *   TLS_HS_PENDING  - waiting for more data from client (SSLWantReadError)
 *   TLS_HS_ERROR    - error (connection must be closed)
 * After calling, check tls_has_encrypted_output() and send pending data.
 */
tls_hs_state_t tls_do_handshake(client_t * client);

/*
 * Read decrypted data from SSLObject into tls->plain_in.
 * Returns number of bytes read, 0 if no data available,
 * <0 on error (including graceful close = -2).
 */
int  tls_read_decrypted(client_t * client);

/*
 * Encrypt plaintext data and place the result into tls->enc_out.
 * data, size - data to encrypt.
 * Returns 0 on success, neg value on error.
 */
int  tls_encrypt(client_t * client, const char * data, ssize_t size, int chunk_idx);

/*
 * Check for encrypted data available in the outgoing BIO (bio_out).
 * Returns number of bytes pending to be sent (>0) or 0.
 */
ssize_t tls_has_encrypted_output(client_t * client);

/*
 * Read all encrypted data from the outgoing BIO into tls->enc_out.
 * After this, tls->enc_out.data is ready to be passed to uv_write().
 * Returns number of bytes or neg value on error.
 */
int  tls_drain_to_enc_out(client_t * client, int chunk_idx);

// -----------------------------------------------------------------------------------
void tls_write_cb(uv_write_t * req, int status);
int  tls_flush_enc_out(client_t * client);
int  tls_stream_write(client_t * client, int nbufs, int total_size);
int  tls_data_encode(client_t * client, uv_buf_t * bufs, int nbufs, int total_len);
int  tls_read_cb(client_t * client, ssize_t nread, uv_buf_t * buf);

#endif /* FASTWSGI_TLS_H_ */
