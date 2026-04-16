#ifndef FASTPYSGI_LIBSSL_H_
#define FASTPYSGI_LIBSSL_H_

/*
 * Minimal OpenSSL type and constant declarations required to build a TLS
 * server using memory BIOs.
 *
 * We deliberately do NOT include any real <openssl/...> headers.  Every
 * type, constant and function-pointer typedef needed is defined here so
 * the project compiles without the OpenSSL development package installed.
 *
 * All symbols are resolved at runtime:
 *   POSIX   - dlopen / dlsym
 *   Windows - LoadLibraryExW / GetProcAddress
 *
 * Compatibility targets:
 *   OpenSSL 1.1.x  (libssl-1_1.dll  / libssl.so.1.1)
 *   OpenSSL 3.x    (libssl-3.dll    / libssl.so.3)
 */

#include <stddef.h>
#include <stdint.h>

/* =========================================================================
 * Opaque OpenSSL handle types.
 * We never dereference these structs - we only pass pointers around.
 * ========================================================================= */

typedef struct ssl_ctx_st      SSL_CTX;
typedef struct ssl_st          SSL;
typedef struct bio_st          BIO;
typedef struct bio_method_st   BIO_METHOD;
typedef struct ssl_method_st   SSL_METHOD;
typedef struct x509_store_ctx_st X509_STORE_CTX;

/* =========================================================================
 * Error codes returned by SSL_get_error()
 * ========================================================================= */

#define SSL_ERROR_NONE              0
#define SSL_ERROR_SSL               1
#define SSL_ERROR_WANT_READ         2
#define SSL_ERROR_WANT_WRITE        3
#define SSL_ERROR_WANT_X509_LOOKUP  4
#define SSL_ERROR_SYSCALL           5
#define SSL_ERROR_ZERO_RETURN       6
#define SSL_ERROR_WANT_CONNECT      7
#define SSL_ERROR_WANT_ACCEPT       8

/* =========================================================================
 * SSL_CTX_set_options() options
 * ========================================================================= */

#define SSL_OP_ALLOW_NO_DHE_KEX                         0x00000400U
#define SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS              0x00000800U
#define SSL_OP_NO_QUERY_MTU                             0x00001000U
#define SSL_OP_COOKIE_EXCHANGE                          0x00002000U
#define SSL_OP_NO_TICKET                                0x00004000U
#define SSL_OP_CISCO_ANYCONNECT                         0x00008000U
#define SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION   0x00010000U
#define SSL_OP_NO_COMPRESSION                           0x00020000U
#define SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION        0x00040000U
#define SSL_OP_NO_ENCRYPT_THEN_MAC                      0x00080000U
#define SSL_OP_ENABLE_MIDDLEBOX_COMPAT                  0x00100000U
#define SSL_OP_PRIORITIZE_CHACHA                        0x00200000U
#define SSL_OP_CIPHER_SERVER_PREFERENCE                 0x00400000U
#define SSL_OP_TLS_ROLLBACK_BUG                         0x00800000U
#define SSL_OP_NO_ANTI_REPLAY                           0x01000000U
#define SSL_OP_NO_SSLv3                                 0x02000000U
#define SSL_OP_NO_TLSv1                                 0x04000000U
#define SSL_OP_NO_TLSv1_2                               0x08000000U
#define SSL_OP_NO_TLSv1_1                               0x10000000U
#define SSL_OP_NO_TLSv1_3                               0x20000000U
#define SSL_OP_NO_DTLSv1                                0x04000000U
#define SSL_OP_NO_DTLSv1_2                              0x08000000U
#define SSL_OP_NO_SSL_MASK (SSL_OP_NO_SSLv3|SSL_OP_NO_TLSv1|SSL_OP_NO_TLSv1_1|SSL_OP_NO_TLSv1_2|SSL_OP_NO_TLSv1_3)
#define SSL_OP_NO_DTLS_MASK (SSL_OP_NO_DTLSv1|SSL_OP_NO_DTLSv1_2)
#define SSL_OP_NO_RENEGOTIATION                         0x40000000U
#define SSL_OP_CRYPTOPRO_TLSEXT_BUG                     0x80000000U
#define SSL_OP_ALL    (SSL_OP_CRYPTOPRO_TLSEXT_BUG|\
                       SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS|\
                       SSL_OP_LEGACY_SERVER_CONNECT|\
                       SSL_OP_TLSEXT_PADDING|\
                       SSL_OP_SAFARI_ECDHE_ECDSA_BUG)

/* =========================================================================
 * SSL_CTX_set_verify() mode flags
 * ========================================================================= */

#define SSL_VERIFY_NONE                 0x00
#define SSL_VERIFY_PEER                 0x01
#define SSL_VERIFY_FAIL_IF_NO_PEER_CERT 0x02
#define SSL_VERIFY_CLIENT_ONCE          0x04

/* =========================================================================
 * File-type argument for SSL_CTX_use_PrivateKey_file()
 * ========================================================================= */

#define SSL_FILETYPE_PEM    1
#define SSL_FILETYPE_ASN1   2

/* =========================================================================
 * BIO_ctrl() command codes used by this code base
 * ========================================================================= */

#define BIO_CTRL_RESET          1   // opt - rewind/zero etc
#define BIO_CTRL_EOF            2   // opt - are we at the eof
#define BIO_CTRL_INFO           3   // opt - extra tit-bits
#define BIO_CTRL_SET            4   // man - set the 'IO' type
#define BIO_CTRL_GET            5   // man - get the 'IO' type
#define BIO_CTRL_PUSH           6   // opt - internal, used to signify change
#define BIO_CTRL_POP            7   // opt - internal, used to signify change
#define BIO_CTRL_GET_CLOSE      8   // man - set the 'close' on free
#define BIO_CTRL_SET_CLOSE      9   // man - set the 'close' on free
#define BIO_CTRL_PENDING        10  // opt - is their more data buffered
#define BIO_CTRL_FLUSH          11  // opt - 'flush' buffered output
#define BIO_CTRL_DUP            12  // man - extra stuff for 'duped' BIO
#define BIO_CTRL_WPENDING       13  // opt - number of bytes still to write

#define BIO_C_SET_BUF_MEM_EOF_RETURN 130
#define BIO_C_SET_WRITE_BUF_SIZE     136
#define BIO_C_GET_WRITE_BUF_SIZE     137

#define BIO_pending(bio)                         g_ssl.BIO_ctrl((bio), BIO_CTRL_PENDING, 0, NULL)
#define BIO_ctrl_pending(bio)            (size_t)g_ssl.BIO_ctrl((bio), BIO_CTRL_PENDING, 0, NULL)
#define BIO_wpending(bio)                        g_ssl.BIO_ctrl((bio), BIO_CTRL_WPENDING, 0, NULL)
#define BIO_ctrl_wpending(bio)           (size_t)g_ssl.BIO_ctrl((bio), BIO_CTRL_WPENDING, 0, NULL)
#define BIO_set_mem_eof_return(bio, val)         g_ssl.BIO_ctrl((bio), BIO_C_SET_BUF_MEM_EOF_RETURN, val, NULL)
#define BIO_set_write_buf_size(bio,size)    (int)g_ssl.BIO_ctrl((bio), BIO_C_SET_WRITE_BUF_SIZE, size, NULL)
#define BIO_get_write_buf_size(bio,size) (size_t)g_ssl.BIO_ctrl((bio), BIO_C_GET_WRITE_BUF_SIZE, size, NULL) 

//----------------------------------------------------------

#define BIO_FLAGS_READ          0x01
#define BIO_FLAGS_WRITE         0x02
#define BIO_FLAGS_IO_SPECIAL    0x04
#define BIO_FLAGS_SHOULD_RETRY  0x08
#define BIO_FLAGS_RWS           (BIO_FLAGS_READ|BIO_FLAGS_WRITE|BIO_FLAGS_IO_SPECIAL)
#define BIO_FLAGS_BASE64_NO_NL  0x100
#define BIO_FLAGS_MEM_RDONLY    0x200
#define BIO_FLAGS_NONCLEAR_RST  0x400
#define BIO_FLAGS_IN_EOF        0x800

#define BIO_should_read(bio)       !!g_ssl.BIO_test_flags(bio, BIO_FLAGS_READ)
#define BIO_should_write(bio)      !!g_ssl.BIO_test_flags(bio, BIO_FLAGS_WRITE)
#define BIO_should_io_special(bio) !!g_ssl.BIO_test_flags(bio, BIO_FLAGS_IO_SPECIAL)
#define BIO_retry_type(bio)        !!g_ssl.BIO_test_flags(bio, BIO_FLAGS_RWS)
#define BIO_should_retry(bio)      !!g_ssl.BIO_test_flags(bio, BIO_FLAGS_SHOULD_RETRY)

/* =========================================================================
 * OPENSSL_init_ssl() flags (OpenSSL 1.1+ only; no-op on 1.0)
 * ========================================================================= */

#define OPENSSL_INIT_LOAD_SSL_STRINGS       0x00200000L
#define OPENSSL_INIT_LOAD_CRYPTO_STRINGS    0x00000002L

/* =========================================================================
 * Function-pointer typedefs - one per OpenSSL function we need.
 * Named fn_<openssl_function_name> to avoid collisions with macros.
 * ========================================================================= */

typedef unsigned long (*fn_OpenSSL_version_num)(void);
typedef int (*fn_OPENSSL_init_ssl)(uint64_t opts, const void * settings);

typedef const SSL_METHOD * (*fn_TLS_server_method)(void);

typedef SSL_CTX * (*fn_SSL_CTX_new)(const SSL_METHOD * meth);
typedef void (*fn_SSL_CTX_free)(SSL_CTX * ctx);
typedef int  (*fn_SSL_CTX_use_certificate_chain_file)(SSL_CTX * ctx, const char * file);
typedef int  (*fn_SSL_CTX_use_PrivateKey_file)(SSL_CTX * ctx, const char * file, int type);
typedef int  (*fn_SSL_CTX_check_private_key)(const SSL_CTX * ctx);
typedef int  (*fn_SSL_CTX_load_verify_locations)(SSL_CTX * ctx, const char * CAfile, const char * CApath);
typedef void (*fn_SSL_CTX_set_verify)(SSL_CTX * ctx, int mode, int (*verify_callback)(int, X509_STORE_CTX *));

typedef unsigned long (*fn_SSL_CTX_get_options)(const SSL_CTX * ctx);
typedef unsigned long (*fn_SSL_get_options)(const SSL * ssl);
typedef unsigned long (*fn_SSL_CTX_clear_options)(SSL_CTX * ctx, unsigned long op);
typedef unsigned long (*fn_SSL_clear_options)(SSL * ssl, unsigned long op);
typedef unsigned long (*fn_SSL_CTX_set_options)(SSL_CTX * ctx, unsigned long op);
typedef unsigned long (*fn_SSL_set_options)(SSL * ssl, unsigned long op);

typedef SSL * (*fn_SSL_new)(SSL_CTX * ctx);
typedef void (*fn_SSL_free)(SSL * ssl);
typedef void (*fn_SSL_set_bio)(SSL * ssl, BIO * rbio, BIO * wbio);
typedef void (*fn_SSL_set_accept_state)(SSL * ssl);
typedef int  (*fn_SSL_do_handshake)(SSL * ssl);
typedef int  (*fn_SSL_read)(SSL * ssl, void * buf, int num);
typedef int  (*fn_SSL_write)(SSL * ssl, const void * buf, int num);
typedef int  (*fn_SSL_pending)(SSL * ssl);
typedef int  (*fn_SSL_get_error)(const SSL * ssl, int ret);
typedef int  (*fn_SSL_shutdown)(SSL * ssl);

typedef BIO * (*fn_BIO_new)(const BIO_METHOD * type);
typedef const BIO_METHOD * (*fn_BIO_s_mem)(void);
typedef int  (*fn_BIO_free)(BIO * bio);
typedef int  (*fn_BIO_write)(BIO * bio, const void * data, int size);
typedef int  (*fn_BIO_read)(BIO * bio, void * data, int size);
typedef long (*fn_BIO_ctrl)(BIO * bio, int cmd, long arg, void * parg);
typedef void (*fn_BIO_set_flags)(BIO * bio, int flags);
typedef int  (*fn_BIO_test_flags)(BIO * bio, int flags);

typedef unsigned long (*fn_ERR_get_error)(void);
typedef void          (*fn_ERR_error_string_n)(unsigned long e, char * buf, size_t len);
typedef void          (*fn_ERR_clear_error)(void);

/* =========================================================================
 * Global vtable - one instance, populated by ssl_lib_init().
 * Declared in ssl.c; included as extern everywhere via this header.
 * ========================================================================= */

typedef struct {
    void * h_ssl;        // handle libssl
    void * h_crypto;     // handle of libcrypto (BIO / ERR symbols may live here)

    int    loaded;       // 1 after a successful ssl_lib_init() call

    fn_OpenSSL_version_num                 OpenSSL_version_num;
    fn_OPENSSL_init_ssl                    OPENSSL_init_ssl;
    fn_TLS_server_method                   TLS_server_method;

    fn_SSL_CTX_new                         SSL_CTX_new;
    fn_SSL_CTX_free                        SSL_CTX_free;
    fn_SSL_CTX_use_certificate_chain_file  SSL_CTX_use_certificate_chain_file;
    fn_SSL_CTX_use_PrivateKey_file         SSL_CTX_use_PrivateKey_file;
    fn_SSL_CTX_check_private_key           SSL_CTX_check_private_key;
    fn_SSL_CTX_load_verify_locations       SSL_CTX_load_verify_locations;
    fn_SSL_CTX_set_verify                  SSL_CTX_set_verify;

    fn_SSL_CTX_get_options                 SSL_CTX_get_options;
    fn_SSL_get_options                     SSL_get_options;
    fn_SSL_CTX_clear_options               SSL_CTX_clear_options;
    fn_SSL_clear_options                   SSL_clear_options;
    fn_SSL_CTX_set_options                 SSL_CTX_set_options;
    fn_SSL_set_options                     SSL_set_options;

    fn_SSL_new                             SSL_new;
    fn_SSL_free                            SSL_free;
    fn_SSL_set_bio                         SSL_set_bio;
    fn_SSL_set_accept_state                SSL_set_accept_state;
    fn_SSL_do_handshake                    SSL_do_handshake;
    fn_SSL_read                            SSL_read;
    fn_SSL_write                           SSL_write;
    fn_SSL_pending                         SSL_pending;
    fn_SSL_get_error                       SSL_get_error;
    fn_SSL_shutdown                        SSL_shutdown;

    fn_BIO_new                             BIO_new;
    fn_BIO_s_mem                           BIO_s_mem;
    fn_BIO_free                            BIO_free;
    fn_BIO_write                           BIO_write;
    fn_BIO_read                            BIO_read;
    fn_BIO_ctrl                            BIO_ctrl;
    fn_BIO_set_flags                       BIO_set_flags;
    fn_BIO_test_flags                      BIO_test_flags;

    fn_ERR_get_error                       ERR_get_error;
    fn_ERR_error_string_n                  ERR_error_string_n;
    fn_ERR_clear_error                     ERR_clear_error;
} libssl_t;

// Defined in ssl.c; used everywhere via this header
extern libssl_t g_ssl;

/* -------------------------------------------------------------------------
 * Library discovery and loading
 * ------------------------------------------------------------------------- */

/*
 * Locate, dlopen and import all required OpenSSL symbols.
 *
 * Windows search order (for each candidate DLL name):
 *   1. <python_executable_dir>/DLLs/<name>
 *   2. <python_executable_dir>/<name>
 *   Candidate names tried in order:
 *     libssl-3.dll, libssl-1_1.dll
 *   Matching libcrypto-*.dll is loaded from the same location.
 *   Python executable directory is obtained via GetModuleFileNameW(NULL).
 *
 * POSIX search order (dlopen uses the system library search path, which
 * already covers /usr/lib, /usr/local/lib, /usr/lib/x86_64-linux-gnu, etc.):
 *   libssl.so.3, libssl.so.1.1
 *   libcrypto.so.3, libcrypto.so.1.1
 *
 * Returns  0 on success.
 * Returns -1 if no usable shared library could be found / loaded.
 * Returns -2 if one or more required symbols could not be resolved.
 */
int ssl_lib_init(void);

/*
 * Unload shared libraries and zero the vtable.
 * Safe to call even if ssl_lib_init() was never called or failed.
 */
void ssl_lib_free(void);

/*
 * Return 1 if ssl_lib_init() completed successfully, 0 otherwise.
 */
int ssl_lib_loaded(void);

/* --- error helpers -------------------------------------------------------- */

void ssl_log_errors(const char * context);

#define SSL_CTX_FREE(ctx) do { if (g_ssl.loaded && (ctx)) g_ssl.SSL_CTX_free(ctx); ctx = NULL; } while(0)
#define SSL_BIO_FREE(bio) do { if (g_ssl.loaded && (bio)) g_ssl.BIO_free(bio); bio = NULL; } while(0)
#define SSL_FREE(obj)     do { if (g_ssl.loaded && (obj)) g_ssl.SSL_free(obj); obj = NULL; } while(0)

#endif /* FASTPYSGI_LIBSSL_H_ */
