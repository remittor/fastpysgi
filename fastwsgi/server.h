#ifndef FASTWSGI_SERVER_H_
#define FASTWSGI_SERVER_H_

#include "common.h"
#include "llhttp.h"
#include "request.h"
#include "xbuf.h"
#include "asgi.h"
#include "tls.h"

#define max_preloaded_body_chunks 48

static const size_t MIN_max_chunk_size = 2*1024;
static const size_t def_max_chunk_size = 256*1024;
static const size_t MAX_max_chunk_size = 64*1024*1024;

static const int def_max_content_length = 999999999;

enum {
    MIN_read_buffer_size = 32 * 1024,
    def_read_buffer_size = 64 * 1024,
    MAX_read_buffer_size = 4 * 1024 * 1024
};

#define HTTP_SERVERS_MAX  (4)

struct server {
    uv_tcp_t server;   // Placement strictly at the beginning of the structure!
    struct srv * srv;  // pointer to global srv_t object (g_srv)
    int ipv6;
    char host[64];
    int port;
    struct {
        PyObject * obj;
        char   str[128];
        size_t len;       // length of root_path
    } root_path;          // SCRIPT_NAME
    tls_server_t tls;     // TLS config
    PyObject * def_env;   // default environ dict for WSGI
    PyObject * def_scope; // default scope dict for ASGI
}; /* server_t */ 

struct srv {
    uv_loop_t* loop;
    PyObject * pysrv;  // object fastpysgi.py@_Server
    int num_loop_cb;   // the number of callbacks that were called in one loop cycle
    int num_writes;    // the number of write operations
    llhttp_settings_t parser_settings;
    PyObject* wsgi_app;
    PyObject* asgi_app;
    int backlog;
    int hook_sigint;   // 0 - ignore SIGINT, 1 - handle SIGINT, 2 - handle SIGINT with halt prog
    uv_signal_t signal;
    int allow_keepalive;
    int resp_hdr_lower;    // 0 = not change case for header names, 1 = force lowercase
    int add_header_date;
    int add_header_server;
    char header_server[80];
    size_t read_buffer_size;
    uint64_t max_content_length;
    size_t max_chunk_size;
    int tcp_nodelay;       // 0 = Nagle's algo enabled; 1 = Nagle's algo disabled;
    int tcp_keepalive;     // negative = disabled; 0 = system default; 1...N = timeout in seconds
    int tcp_send_buf_size; // 0 = system default; 1...N = size in bytes
    int tcp_recv_buf_size; // 0 = system default; 1...N = size in bytes
    struct {
        int mode;          // 0 - disabled, 1 - nowait active, 2 - nowait with wait disconnect all peers
        int base_handles;  // number of base handles (listen socket + signal)
    } nowait;
    int exit_code;
    asyncio_t aio;
    int servers_num;        // number of servers 
    server_t servers[HTTP_SERVERS_MAX];
}; /* srv_t */


typedef enum {
    RXS_RESTING = 0,
    RXS_READING,
    RXS_FREEZED,        // active only llhttp parser
    RXS_FAIL,
    RXS_DONE,
} rx_status_t;

typedef enum {
    LS_WAIT            = 0,
    LS_MSG_BEGIN       = 1,
    LS_MSG_URL         = 2,  // URL fully loaded
    LS_MSG_HEADERS     = 3,  // all request headers loaded
    LS_MSG_BODY        = 4,  // load body
    LS_MSG_END         = 5,  // request readed fully
    LS_OK              = 6   // request loaded fully
} load_state_t;

typedef enum {
    CS_UNKNOWN = 0,
    CS_ACCEPT,
    CS_REQ_READ,         // wait and read data from socket
    CS_REQ_PARSE,
    CS_APP_CALL,
    CS_RESP_BILD,
    CS_RESP_SEND,        // sending data to remote client
    CS_RESP_SENDED,
    CS_RESP_END,
    CS_DESTROY,
} client_state_t;

typedef enum {
    CA_OK           = 0,  // continue read from socket
    CA_CLOSE        = 1,
    CA_SHUTDOWN     = 2
} client_action_t;

typedef struct {
    uv_write_t req;     // Placement strictly at the beginning of the structure!
    client_t * client;
    uv_buf_t   bufs[max_preloaded_body_chunks + 3];
} write_req_t;

struct client {
    uv_tcp_t handle;     // peer connection. Placement strictly at the beginning of the structure! 
    server_t * server;
    char remote_addr[64];
    struct {                 // read stream struct
        rx_status_t status;
        uint64_t pkt;        // total packet count of received from the socket
        uint64_t raw_total;  // total size of raw data received from the socket
        xbuf_t   rawbuf;     // buffer for reading from TCP socket (data = buf_read_prealloc + read_buffer_size + 8)
        xbuf_t   buf;        // buffer for llhttp parser (data = buf_read_prealloc)
        uint64_t total;      // total size of data received for llhttp parser
        int parsed_size;     // size of the data that was processed by the llhttp parser (buf.size = full size planned for parsing)
    } rx;
    client_state_t state;    // current client state
    tls_client_t tls;        // TLS client state. Active only if server->tls.enabled != 0
    asgi_t * asgi;           // ASGI 3.0 implementation
    struct {
        int load_state;          // load_state_t
        int64_t http_content_length; // if -1 => "Content-Length" not specified
        int chunked;             // Transfer-Encoding: chunked
        int keep_alive;          // 1 = Connection: Keep-Alive or HTTP/1.1
        int expect_continue;     // 1 = Expect: 100-continue
        size_t current_key_len;
        size_t current_val_len;
        int headers_num;       // number of headers
        PyObject* headers;     // PyDict
        PyObject* host;        // PyUnicode - value of Host header
        PyObject* wsgi_input_empty;  // empty io.ByteIO object for requests without body
        PyObject* wsgi_input;  // type: io.BytesIO
        int64_t wsgi_input_size;   // total size of wsgi_input PyBytes stream
        llhttp_t parser;
        bool parser_locked;
    } request;
    int error;    // error code on process request and response
    xbuf_t head;  // dynamic buffer for request and response headers data
    StartResponse * start_response;
    struct {
        int headers_size;        // size of headers for sending
        int64_t wsgi_content_length; // -1 = "Content-Length" not specified
        PyObject* wsgi_body;
        PyObject* body_iterator;
        size_t body_chunk_num;
        PyObject* body[max_preloaded_body_chunks + 1]; // pleloaded body's chunks (PyBytes)
        int64_t body_preloaded_size; // sum of all preloaded body's chunks
        int64_t body_total_size;
        int64_t body_total_written;
        int chunked;    // 1 = chunked sending; 2 = last chunk send
        write_req_t write_req;
    } response;
    // preallocated buffers
    char buf_head_prealloc[2*1024];
    char buf_read_prealloc[1];
}; /* client_t */

extern srv_t g_srv;

PyObject * get_version(PyObject * self);
PyObject * init_server(PyObject * self, PyObject * server);
PyObject * change_setting(PyObject * self, PyObject * args);
PyObject * run_server(PyObject * self, PyObject * server);
PyObject * run_nowait(PyObject * self, PyObject * server);
PyObject * close_server(PyObject * self, PyObject * server);

const char * get_cstate(int state);
const char * get_rxstatus(int status);

int x_send_status(client_t * client, int status);
int stream_write(client_t * client);
int stream_read_start_ex(client_t * client, const char * func);
int stream_read_stop_ex(client_t * client, const char * func);
void close_connection(client_t * client);

#define stream_read_start(_client_)  stream_read_start_ex((_client_), __func__)
#define stream_read_stop(_client_)    stream_read_stop_ex((_client_), __func__)

void read_rxbuf_after_send(client_t * client, const char * _func);

// ----------- functions from request.c ----------------------------

void reset_head_buffer(client_t * client);
void free_start_response(client_t * client);
void reset_response_preload(client_t * client);
void reset_response_body(client_t * client);

int call_wsgi_app(client_t * client);
int process_wsgi_response(client_t * client);
int create_response(client_t * client);

typedef enum {
    RF_EMPTY           = 0x00,
    RF_SET_KEEP_ALIVE  = 0x01,
    RF_HEADERS_WSGI    = 0x02,
    RF_HEADERS_ASGI    = 0x04,
    RF__MAX
} response_flag_t;

int build_response(client_t * client, int flags, int status, const void * headers, const void * body, int body_size);
PyObject* wsgi_iterator_get_next_chunk(client_t * client, int outpyerr);

// -----------------------------------------------------------------

static
inline
server_t * SERVER(int idx)
{
    return (server_t *) &g_srv.servers[idx];
}

static
inline
void before_loop_callback(client_t * client)
{
    g_srv.num_loop_cb++;
}

static
inline
void update_log_prefix(client_t * client)
{
    set_log_client_addr(client ? client->remote_addr : NULL);
}

#define SET_CSTATE(_state_) do { \
    LOGd("%s: change client state: %s --> %s", __func__, get_cstate(client->state), get_cstate(_state_)); \
    client->state = _state_; \
} while(0)

#define SET_CSTATE_FN(_state_, _func_) do { \
    LOGd("%s: change client state: %s --> %s", _func_, get_cstate(client->state), get_cstate(_state_)); \
    client->state = _state_; \
} while(0)

#endif
