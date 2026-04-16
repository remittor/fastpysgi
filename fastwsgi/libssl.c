#include "libssl.h"
#include "logx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <dlfcn.h>
#endif

/* =========================================================================
 * Global vtable instance (declared extern in libssl.h)
 * ========================================================================= */

libssl_t g_ssl;

/* =========================================================================
 * Internal symbol-loading helpers
 * ========================================================================= */

/*
 * Look up a symbol name first in h_ssl, then in h_crypto.
 * Returns the function pointer or NULL if not found in either library.
 */
static
void * get_lib_sym(const char * name)
{
    void * ptr = NULL;
#ifdef _WIN32
    ptr = (void *)GetProcAddress((HMODULE)g_ssl.h_ssl, name);
#else
    ptr = dlsym(g_ssl.h_ssl, name);
#endif
    if (!ptr) {
#ifdef _WIN32
        ptr = (void *)GetProcAddress((HMODULE)g_ssl.h_crypto, name);
#else
        ptr = dlsym(g_ssl.h_crypto, name);
#endif
    }
    return ptr;
}

#define LOAD_SYM(name)                                                   \
    do {                                                                 \
        g_ssl.name = (fn_##name)get_lib_sym(#name);                      \
        if (!g_ssl.name) {                                               \
            LOGw("%s: required symbol not found: %s", __func__, #name);  \
            sym_err++;                                                   \
        }                                                                \
    } while (0)

#define LOAD_SYM_OPT(name)                                               \
    do {                                                                 \
        g_ssl.name = (fn_##name)get_lib_sym(#name);                      \
        if (!g_ssl.name) {                                               \
            LOGd("%s: optional symbol not found: %s", __func__, #name);  \
        }                                                                \
    } while (0)

/* =========================================================================
 * Platform-specific library discovery
 * ========================================================================= */

#ifdef _WIN32

/*
 * Locate and load libssl + libcrypto on Windows.
 *
 * We obtain the running Python interpreter's directory via
 * GetModuleFileNameW(NULL) and probe two sub-locations:
 *   <exe_dir>/DLLs/<name>
 *   <exe_dir>/<name>
 *
 * OpenSSL 3 names are tried before OpenSSL 1.1 names.
 */
static
int lib_load_all(void)
{
    int hr = -1;
    wchar_t fullname[MAX_PATH] = { 0 };
    wchar_t suffix[MAX_PATH] = { 0 };
    wchar_t path[MAX_PATH] = { 0 };
    // Obtain the directory that contains python.exe (or pythonw.exe)
    if (!GetModuleFileNameW(NULL, path, MAX_PATH-1)) {
        LOGe("%s: GetModuleFileNameW failed (err = %lu)", __func__, GetLastError());
        return -2;
    }
    wchar_t * last_hyphen;
    wchar_t * last_sep = wcsrchr(path, L'\\');
    FIN_IF(!last_sep, -3);
    last_sep[0] = 0;
    // Sub-directories to probe (empty string = exe_dir itself)
    static const wchar_t * const subdirs[ ] = { L"\\DLLs", L"", NULL };
    // Candidate DLL base names - ordered: OpenSSL 3 first, then 1.1
    static const wchar_t * const ssl_names[ ] = {
        L"libssl-3.dll",
        L"libssl-1_1.dll",
        NULL
    };
    for (int si = 0; ssl_names[si]; si++) {
        for (int di = 0; subdirs[di]; di++) {
            _snwprintf(fullname, MAX_PATH-1, L"%s%s\\%s", path, subdirs[di], ssl_names[si]);
            HMODULE hnd = LoadLibraryExW(fullname, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (hnd) {
                g_ssl.h_ssl = (void *)hnd;
                break;
            }
            DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND && err != ERROR_MOD_NOT_FOUND) {
                LOGe("%s: cannot load DLL \"%S\" (err = %lu)", __func__, fullname, err);
                FIN(-12);
            }
        }
    }
    LOGe_IF(!g_ssl.h_ssl, "%s: could not find libssl DLL under %S", __func__, path);
    FIN_IF(!g_ssl.h_ssl, -11);
    last_sep = wcsrchr(fullname, L'\\');
    last_hyphen = wcsrchr(fullname, L'-');
    FIN_IF(!last_sep || !last_hyphen, -17);
    FIN_IF((size_t)last_hyphen < (size_t)last_sep, -18);
    wcsncpy(suffix, last_hyphen, ARRAY_SIZE(suffix) - 1);
    _swprintf(last_sep + 1, L"%s%s", L"libcrypto", suffix);
    {
        HMODULE hnd = LoadLibraryExW(fullname, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        g_ssl.h_crypto = (void *)hnd;
    }
    LOGe_IF(!g_ssl.h_crypto, "%s: cannot load DLL \"%S\" (err = %lu)", __func__, fullname, GetLastError());
    FIN_IF(!g_ssl.h_crypto, -21);
    hr = 0;
fin:
    return hr;
}

#else /* POSIX */

/*
 * Locate and load libssl + libcrypto on POSIX systems.
 * Versioned sonames are tried first so we prefer the newest installed copy.
 */
static
int lib_load_all(void)
{
    int hr = -1;
    struct stat st;
    char so_name[256] = { 0 };
    static const char * const ssl_names[ ] = {
        "libssl.so.3",
        "libssl.so.1.1",
        "libssl.so",
        NULL
    };
    const char * lib_name = NULL;
    for (int idx = 0; ssl_names[idx]; idx++) {
        lib_name = ssl_names[idx];
        void * hnd = dlopen(lib_name, RTLD_NOW | RTLD_GLOBAL);
        if (hnd) {
            g_ssl.h_ssl = hnd;
            break;
        }
        const char * err = dlerror();
        if (err && strstr(err, "No such file") == NULL) {
            LOGe("%s: cannot load lib \"%s\" (err = %s)", __func__, lib_name, err);
            FIN(-12);
        }
    }
    LOGe_IF(!g_ssl.h_ssl, "%s: could not find libssl.so* (not installed?)", __func__);
    FIN_IF(!g_ssl.h_ssl, -11);
    const char * ext = strstr(lib_name, ".so");
    FIN_IF(!ext, -15);
    snprintf(so_name, sizeof(so_name)-1, "libcrypto%s", ext);
    g_ssl.h_crypto = dlopen(so_name, RTLD_NOW | RTLD_GLOBAL);
    LOGe_IF(!g_ssl.h_crypto, "%s: cannot load lib \"%s\" (err = %s)", __func__, so_name, dlerror());
    FIN_IF(!g_ssl.h_crypto, -22);
    hr = 0;
fin:
    return hr;
}

#endif /* _WIN32 / POSIX */

/* =========================================================================
 * Public API: ssl_lib_init / ssl_lib_free / ssl_lib_loaded
 * ========================================================================= */

int ssl_lib_init(void)
{
    int hr = -1;
    int sym_err = 0;
    FIN_IF(g_ssl.loaded, 0);
    memset(&g_ssl, 0, sizeof(g_ssl));
    // Locate and open the shared libraries.
    hr = lib_load_all();
    FIN_IF(hr, hr);
    // Resolve all required symbols
    LOAD_SYM(OpenSSL_version_num);
    LOAD_SYM(OPENSSL_init_ssl);
    LOAD_SYM(TLS_server_method);
    LOAD_SYM(SSL_CTX_new);
    LOAD_SYM(SSL_CTX_free);
    LOAD_SYM(SSL_CTX_use_certificate_chain_file);
    LOAD_SYM(SSL_CTX_use_PrivateKey_file);
    LOAD_SYM(SSL_CTX_check_private_key);
    LOAD_SYM(SSL_CTX_load_verify_locations);
    LOAD_SYM(SSL_CTX_set_verify);
    LOAD_SYM(SSL_CTX_get_options);
    LOAD_SYM(SSL_CTX_set_options);
    LOAD_SYM(SSL_CTX_clear_options);
    LOAD_SYM(SSL_new);
    LOAD_SYM(SSL_free);
    LOAD_SYM(SSL_get_options);
    LOAD_SYM(SSL_set_options);
    LOAD_SYM(SSL_clear_options);
    LOAD_SYM(SSL_set_bio);
    LOAD_SYM(SSL_set_accept_state);
    LOAD_SYM(SSL_do_handshake);
    LOAD_SYM(SSL_read);
    LOAD_SYM(SSL_write);
    LOAD_SYM(SSL_pending);
    LOAD_SYM(SSL_get_error);
    LOAD_SYM(SSL_shutdown);
    LOAD_SYM(BIO_new);
    LOAD_SYM(BIO_s_mem);
    LOAD_SYM(BIO_free);
    LOAD_SYM(BIO_write);
    LOAD_SYM(BIO_read);
    LOAD_SYM(BIO_ctrl);
    LOAD_SYM(BIO_set_flags);
    LOAD_SYM(BIO_test_flags);
    LOAD_SYM(ERR_get_error);
    LOAD_SYM(ERR_error_string_n);
    LOAD_SYM(ERR_clear_error);
    LOGe_IF(sym_err, "%s: one or more required symbols could not be loaded", __func__);
    FIN_IF(sym_err, -30);
    // load error strings, etc.
    int rc = g_ssl.OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
    FIN_IF(rc != 1, -33);
    hr = 0;
    g_ssl.loaded = 1;
    LOGd("%s: OpenSSL loaded (version: 0x%X)", __func__, g_ssl.OpenSSL_version_num());
fin:
    if (hr) {
        ssl_lib_free();
    }
    return hr;
}

void ssl_lib_free(void)
{
#ifdef _WIN32
    if (g_ssl.h_ssl)    FreeLibrary((HMODULE)g_ssl.h_ssl);
    if (g_ssl.h_crypto) FreeLibrary((HMODULE)g_ssl.h_crypto);
#else
    if (g_ssl.h_ssl)    dlclose(g_ssl.h_ssl);
    if (g_ssl.h_crypto) dlclose(g_ssl.h_crypto);
#endif
    memset(&g_ssl, 0, sizeof(g_ssl));
}

int ssl_lib_loaded(void)
{
    return g_ssl.loaded;
}

void ssl_log_errors(const char * context)
{
    unsigned long err;
    char buf[256];
    if (g_ssl.loaded) {
        while ((err = g_ssl.ERR_get_error()) != 0) {
            g_ssl.ERR_error_string_n(err, buf, sizeof(buf));
            LOGe("[OpenSSL] %s: %s", context ? context : "?", buf);
        }
    }
}
