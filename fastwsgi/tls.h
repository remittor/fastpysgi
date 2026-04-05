#ifndef FASTWSGI_TLS_H_
#define FASTWSGI_TLS_H_

#include "common.h"
#include "xbuf.h"

/*
 * TLS layer implementation based on ssl.MemoryBIO + SSLContext.wrap_bio().
 *
 * Workflow:
 *
 *   libuv recv  =>  encrypted bytes  => tls_feed_encrypted()
 *                                               |
 *                                       incoming MemoryBIO
 *                                               |
 *                                       SSLObject.read() => plaintext => llhttp / app
 *
 *   app response => plaintext => SSLObject.write()
 *                                       |
 *                                outgoing MemoryBIO
 *                                       |
 *                                tls_drain_encrypted() => encrypted bytes => libuv send
 *
 * Handshake:
 *   While tls_t.hs_done == 0, all data is routed to tls_do_handshake().
 *   After the handshake is completed, the flag is set to 1 and normal
 *   data transfer begins.
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
    PyObject       * ssl_obj;       // ssl.SSLObject - result of SSLContext.wrap_bio()
    PyObject       * bio_in;        // ssl.MemoryBIO - incoming (encrypted from client)
    PyObject       * bio_out;       // ssl.MemoryBIO - outgoing (encrypted to client)

    size_t           hs_readed_bytes;  // size of read data for handshake
    tls_hs_state_t   hs_state;         // Handshake state
    int              close_notify;     // 1 if received TLS close_notify from client

    /* Buffer for encrypted data waiting to be sent via libuv.
     * uv_write() does not copy data => the buffer must remain valid
     * until the write_cb callback is invoked.
     * Uses xbuf_t with dynamic resizing. */
    xbuf_t           enc_out;       // encrypted outgoing buffer

    /* Buffer for decrypted data read from SSLObject.
     * Needed because SSLObject.read() may return less data
     * than actually available in BIO in a single call.
     * Data is read in a loop. */
    xbuf_t           plain_in;      // decrypted incoming buffer

    /* Flag: encrypted buffer is being sent via uv_write().
     * While a write is in progress, enc_out must not be modified. */
    int              writing;
} tls_client_t;

// Server TLS configuration (created once during server initialization)
typedef struct {
    int        enabled;             // 1 if TLS is enabled; else 0
    PyObject * ctx;                 // ssl.SSLContext
    // Cached attributes for faster access
    PyObject * wrap_bio;            // SSLContext.wrap_bio (methhod)
    PyObject * MemoryBIO;           // ssl.MemoryBIO (class)
    PyObject * SSLWantReadError;    // ssl.SSLWantReadError
    PyObject * SSLZeroReturnError;  // ssl.SSLZeroReturnError
    PyObject * SSLError;            // ssl.SSLError
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
 * ts     - server configuration
 * tls    - TLS state structure for this client (fields are zero-initialized internally)
 * server_hostname - SNI hostname (NULL => not provided)
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
int  tls_feed_encrypted(client_t * client, const char * data, Py_ssize_t size);

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
int  tls_encrypt(client_t * client, const char * data, Py_ssize_t size);

/*
 * Check for encrypted data available in the outgoing BIO (bio_out).
 * Returns number of bytes pending to be sent (>0) or 0.
 */
Py_ssize_t tls_has_encrypted_output(client_t * client);

/*
 * Read all encrypted data from the outgoing BIO into tls->enc_out.
 * After this, tls->enc_out.data is ready to be passed to uv_write().
 * Returns number of bytes or neg value on error.
 */
int  tls_drain_to_enc_out(client_t * client);

// -----------------------------------------------------------------------------------
void tls_write_cb(uv_write_t * req, int status);
int  tls_flush_enc_out(client_t * client);
int  tls_stream_write(client_t * client);
int  tls_read_cb(client_t * client, ssize_t nread, uv_buf_t * buf);

#endif /* FASTWSGI_TLS_H_ */
