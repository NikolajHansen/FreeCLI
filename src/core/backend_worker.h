#ifndef BACKEND_WORKER_H
#define BACKEND_WORKER_H

/*
 * backend_worker.h — BackendWorker abstraction for FreeCLI's comm bus.
 *
 * Workers are pluggable units that handle operations dispatched from the
 * TUI via MSG_WORKER_REQ.  Each worker type registers once; incoming
 * requests are routed to the matching worker and run on a detached thread.
 *
 * Transport note: BusContext currently wraps the existing TCP fd.  The
 * interface is intentionally transport-agnostic so the ZeroMQ migration
 * (ipc:// DEALER/ROUTER) requires changes only inside BusContext and the
 * bw_send_* helpers — not in any worker implementation.
 */

#include "ipc.h"
#include <stdint.h>

/* -----------------------------------------------------------------------
 * BackendType — identifies which class of worker handles a request.
 * --------------------------------------------------------------------- */
typedef enum {
    BACKEND_TYPE_LLM        = 0,
    BACKEND_TYPE_MCP        = 1,
    BACKEND_TYPE_SHELL      = 2,
    BACKEND_TYPE_DATABASE   = 3,
    BACKEND_TYPE_FILESYSTEM = 4,
    BACKEND_TYPE_OTHER      = 99,
} BackendType;

/* -----------------------------------------------------------------------
 * BusContext — opaque transport handle.
 *
 * Workers receive this and pass it to bw_send_* helpers.  They must not
 * inspect or use the internals directly — that is the migration boundary.
 * --------------------------------------------------------------------- */
typedef struct {
    net_fd_t fd;        /* current: TCP socket to TUI */
    /* future: void *zmq_sock;  ZeroMQ DEALER socket */
} BusContext;

/* -----------------------------------------------------------------------
 * BusMsg — a decoded worker request.  Lifetime: one dispatch() call.
 * --------------------------------------------------------------------- */
typedef struct {
    uint64_t    corr_id;      /* per-request id; echoed in all replies   */
    BackendType worker_type;
    char       *op;           /* operation name, e.g. "read_file"        */
    char       *args_json;    /* JSON arguments string (heap, may be NULL) */
} BusMsg;

void bus_msg_free(BusMsg *m);

/* -----------------------------------------------------------------------
 * BackendWorker — vtable.
 *
 * Implement init + dispatch (+ free_w if priv needs cleanup).
 * --------------------------------------------------------------------- */
typedef struct BackendWorker {
    BackendType  type;
    const char  *name;   /* short display name shown in the workers pane */

    /*
     * init — called once at startup on the main thread.
     * Set self->priv here.  Return 0 on success, -1 on failure.
     */
    int  (*init)(struct BackendWorker *self);

    /*
     * dispatch — called on a detached pthread for each MSG_WORKER_REQ
     * whose worker_type matches this worker.
     *
     * MUST call bw_send_result() or bw_send_error() before returning.
     * May call bw_send_progress() any number of times first.
     * May call bw_request_approval() to gate mutating operations.
     *
     * Returns 0 on success, -1 on error (result already sent either way).
     */
    int  (*dispatch)(struct BackendWorker *self,
                     BusContext *bus, const BusMsg *msg);

    /*
     * free_w — release worker resources at shutdown.  May be NULL.
     */
    void (*free_w)(struct BackendWorker *self);

    void *priv; /* worker-private data; owned by the worker */
} BackendWorker;

/* -----------------------------------------------------------------------
 * Reply helpers — call these from inside dispatch().
 * --------------------------------------------------------------------- */

/*
 * bw_send_progress — send a one-line status update to the TUI worker pane.
 * parent_corr is the top-level user request corr_id (0 if unknown).
 */
void bw_send_progress(BusContext *bus, uint64_t corr_id,
                      uint64_t parent_corr, const char *worker_name,
                      const char *msg);

/*
 * bw_send_result — send the final result and close out this corr_id.
 * success: 1 = ok, 0 = error.
 * result:  JSON string (success) or plain error description (failure).
 */
void bw_send_result(BusContext *bus, uint64_t corr_id,
                    int success, const char *result);

/* Convenience: send an error result and close the corr_id. */
void bw_send_error(BusContext *bus, uint64_t corr_id, const char *err);

/* -----------------------------------------------------------------------
 * Approval flow.
 *
 * bw_request_approval() blocks the calling dispatch thread, sends
 * MSG_APPROVAL_REQ to the TUI, and waits up to 30 seconds for a reply.
 * Returns 1 if approved, 0 if denied or timed out.
 *
 * bw_notify_approval() must be called by the backend message loop when
 * MSG_APPROVAL_REP arrives.  It wakes the blocked dispatch thread.
 * --------------------------------------------------------------------- */
int  bw_request_approval(BusContext *bus, uint64_t corr_id,
                          const char *description);
void bw_notify_approval(uint64_t corr_id, int approved);

/* -----------------------------------------------------------------------
 * Worker registry.
 * --------------------------------------------------------------------- */
#define BW_MAX_WORKERS 16

/* Must be called once before registering workers. */
void bw_registry_init(void);

/* Register a worker.  Returns 0 on success, -1 if the registry is full. */
int bw_registry_add(BackendWorker *w);

/* Find the registered worker for a given type.  Returns NULL if not found. */
BackendWorker *bw_registry_find(BackendType type);

/* Call free_w() on all workers and clear the registry. */
void bw_registry_free_all(void);

/* -----------------------------------------------------------------------
 * Dispatcher — route a raw MSG_WORKER_REQ to the matching worker.
 * Spawns a detached pthread.
 * Returns 0 if dispatched, -1 if no worker found or payload decode failed.
 * --------------------------------------------------------------------- */
int bw_dispatch_msg(BusContext *bus, const IpcMsg *raw_msg);

#endif /* BACKEND_WORKER_H */
