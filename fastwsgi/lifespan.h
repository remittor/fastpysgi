#ifndef FASTWSGI_LIFESPAN_H_
#define FASTWSGI_LIFESPAN_H_

#include "common.h"

/*
 * ASGI Lifespan protocol support (ASGI spec 2.0)
 *
 * Flow:
 *   server -> app : {"type": "lifespan.startup"}
 *   app -> server : {"type": "lifespan.startup.complete"}  (or .failed)
 *   ... server accepts HTTP requests ...
 *   server -> app : {"type": "lifespan.shutdown"}
 *   app -> server : {"type": "lifespan.shutdown.complete"} (or .failed)
 *
 * lifespan_mode_t controls behaviour when app does NOT support lifespan:
 *   LS_MODE_OFF  - skip lifespan entirely
 *   LS_MODE_ON   - require lifespan support, abort startup on error
 *   LS_MODE_AUTO - try lifespan, silently ignore if unsupported (TypeError/NotImplemented)
 */

typedef enum {
    LS_MODE_OFF  = 0,
    LS_MODE_ON   = 1,
    LS_MODE_AUTO = 2,
} lifespan_mode_t;

typedef enum {
    LS_STATE_IDLE        = 0,  /* not started yet */
    LS_STATE_STARTING    = 1,  /* waiting for startup.complete */
    LS_STATE_READY       = 2,  /* startup done, HTTP can proceed */
    LS_STATE_STOPPING    = 3,  /* waiting for shutdown.complete */
    LS_STATE_DONE        = 4,  /* shutdown complete */
    LS_STATE_FAILED      = 5,  /* startup.failed or exception */
    LS_STATE_UNSUPPORTED = 6,  /* app does not support lifespan (auto mode) */
} lifespan_state_t;

typedef struct {
    lifespan_mode_t  mode;
    int fail_on_startup_error;  /* 1 = abort server startup on lifespan startup failure */

    lifespan_state_t state;

    /* asyncio.Future that resolves when startup.complete arrives */
    PyObject * startup_future;
    /* asyncio.Future that resolves when shutdown.complete arrives */
    PyObject * shutdown_future;

    /* Future given to the app's receive() — server sets result to trigger events */
    PyObject * recv_future;
    /* Future given to the app's send() — server waits on this */
    PyObject * send_future;

    /* asyncio.Task for the lifespan coroutine */
    PyObject * task;

    /* scope dict passed to the app */
    PyObject * scope;

    /* error message from lifespan.startup.failed / lifespan.shutdown.failed */
    char fail_message[512];
} lifespan_t;


/*
 * Initialise the lifespan sub-system.
 * Called once from init_srv(), before asyncio loop starts.
 */
int lifespan_init(lifespan_t * ls);

/*
 * Free all Python objects owned by lifespan_t.
 */
void lifespan_free(lifespan_t * ls);

/*
 * Launch the lifespan coroutine and wait (inside the asyncio loop) until
 * startup.complete or startup.failed arrives.
 *
 * Returns:
 *   0   - startup succeeded (or lifespan=auto and app doesn't support it)
 *  neg  - startup failed and fail_on_startup_error == 1
 */
int lifespan_startup(lifespan_t * ls);

/*
 * Send lifespan.shutdown to the running app and wait for shutdown.complete.
 * Called from close_server() before asyncio_free().
 */
int lifespan_shutdown(lifespan_t * ls);


#endif /* FASTWSGI_LIFESPAN_H_ */
