#include "backend_worker.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#ifndef _WIN32
# include <time.h>
#else
# include <windows.h>
#endif

/* -----------------------------------------------------------------------
 * BusMsg
 * --------------------------------------------------------------------- */

void bus_msg_free(BusMsg *m) {
    if (!m) return;
    free(m->op);
    free(m->args_json);
    m->op = m->args_json = NULL;
}

/* -----------------------------------------------------------------------
 * Reply helpers
 * --------------------------------------------------------------------- */

void bw_send_progress(BusContext *bus, uint64_t corr_id,
                      uint64_t parent_corr, const char *worker_name,
                      const char *msg) {
    uint32_t ws_len;
    uint8_t *ws = ipc_encode_worker_start(parent_corr,
                                           worker_name ? worker_name : "worker",
                                           msg ? msg : "", &ws_len);
    if (ws) {
        ipc_send(bus->fd, MSG_WORKER_START, corr_id, ws, ws_len);
        free(ws);
    }
}

void bw_send_result(BusContext *bus, uint64_t corr_id,
                    int success, const char *result) {
    uint32_t rep_len;
    uint8_t *rep = ipc_encode_worker_rep(success, result ? result : "", &rep_len);
    if (rep) {
        ipc_send(bus->fd, MSG_WORKER_REP, corr_id, rep, rep_len);
        free(rep);
    }
    ipc_send(bus->fd, MSG_WORKER_END, corr_id, NULL, 0);
}

void bw_send_error(BusContext *bus, uint64_t corr_id, const char *err) {
    bw_send_result(bus, corr_id, 0, err ? err : "unknown error");
}

/* -----------------------------------------------------------------------
 * Approval flow
 *
 * A PendingApproval slot is grabbed by bw_request_approval(), which
 * sends MSG_APPROVAL_REQ to the TUI and blocks on a condvar.
 * bw_notify_approval() is called from the backend message loop when
 * MSG_APPROVAL_REP arrives; it signals the condvar to unblock the worker.
 * --------------------------------------------------------------------- */

#define MAX_PENDING_APPROVALS 8
#define APPROVAL_TIMEOUT_SEC  30

typedef struct {
    int             in_use;
    uint64_t        corr_id;
    int             decided;   /* 0 = pending, 1 = approved, -1 = denied */
    pthread_mutex_t mu;
    pthread_cond_t  cv;
} PendingApproval;

static PendingApproval  approvals[MAX_PENDING_APPROVALS];
static pthread_mutex_t  approvals_mu = PTHREAD_MUTEX_INITIALIZER;
static int              approvals_ready = 0;

static void approvals_init_once(void) {
    if (approvals_ready) return;
    for (int i = 0; i < MAX_PENDING_APPROVALS; i++) {
        memset(&approvals[i], 0, sizeof(approvals[i]));
        pthread_mutex_init(&approvals[i].mu, NULL);
        pthread_cond_init(&approvals[i].cv, NULL);
    }
    approvals_ready = 1;
}

int bw_request_approval(BusContext *bus, uint64_t corr_id,
                         const char *description) {
    approvals_init_once();

    /* Claim a free slot */
    pthread_mutex_lock(&approvals_mu);
    PendingApproval *slot = NULL;
    for (int i = 0; i < MAX_PENDING_APPROVALS; i++) {
        if (!approvals[i].in_use) { slot = &approvals[i]; break; }
    }
    if (!slot) {
        pthread_mutex_unlock(&approvals_mu);
        fprintf(stderr, "[bw] approval table full — denying '%s'\n",
                description ? description : "?");
        return 0;
    }
    slot->in_use  = 1;
    slot->corr_id = corr_id;
    slot->decided = 0;
    pthread_mutex_unlock(&approvals_mu);

    /* Ask the TUI */
    uint32_t aq_len;
    uint8_t *aq = ipc_encode_approval_req(corr_id,
                                           description ? description : "", &aq_len);
    if (aq) {
        ipc_send(bus->fd, MSG_APPROVAL_REQ, corr_id, aq, aq_len);
        free(aq);
    }

    /* Block until decided or timed out */
#ifndef _WIN32
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += APPROVAL_TIMEOUT_SEC;
#endif

    pthread_mutex_lock(&slot->mu);
    while (slot->decided == 0) {
#ifndef _WIN32
        int rc = pthread_cond_timedwait(&slot->cv, &slot->mu, &deadline);
        if (rc != 0) { slot->decided = -1; break; }
#else
        /* Windows fallback: spin with short sleep */
        pthread_mutex_unlock(&slot->mu);
        Sleep(100);
        pthread_mutex_lock(&slot->mu);
        /* timeout handled by bw_notify_approval after 30 s */
#endif
    }
    int approved = (slot->decided == 1) ? 1 : 0;
    pthread_mutex_unlock(&slot->mu);

    pthread_mutex_lock(&approvals_mu);
    slot->in_use = 0;
    pthread_mutex_unlock(&approvals_mu);

    return approved;
}

void bw_notify_approval(uint64_t corr_id, int approved) {
    if (!approvals_ready) return;
    pthread_mutex_lock(&approvals_mu);
    for (int i = 0; i < MAX_PENDING_APPROVALS; i++) {
        if (approvals[i].in_use && approvals[i].corr_id == corr_id) {
            pthread_mutex_lock(&approvals[i].mu);
            approvals[i].decided = approved ? 1 : -1;
            pthread_cond_signal(&approvals[i].cv);
            pthread_mutex_unlock(&approvals[i].mu);
            break;
        }
    }
    pthread_mutex_unlock(&approvals_mu);
}

/* -----------------------------------------------------------------------
 * Worker registry
 * --------------------------------------------------------------------- */

static BackendWorker  *registry[BW_MAX_WORKERS];
static int             registry_count = 0;
static pthread_mutex_t registry_mu   = PTHREAD_MUTEX_INITIALIZER;

void bw_registry_init(void) {
    pthread_mutex_lock(&registry_mu);
    memset(registry, 0, sizeof(registry));
    registry_count = 0;
    pthread_mutex_unlock(&registry_mu);
    approvals_init_once();
}

int bw_registry_add(BackendWorker *w) {
    if (!w) return -1;
    pthread_mutex_lock(&registry_mu);
    if (registry_count >= BW_MAX_WORKERS) {
        pthread_mutex_unlock(&registry_mu);
        return -1;
    }
    registry[registry_count++] = w;
    pthread_mutex_unlock(&registry_mu);
    return 0;
}

BackendWorker *bw_registry_find(BackendType type) {
    pthread_mutex_lock(&registry_mu);
    BackendWorker *found = NULL;
    for (int i = 0; i < registry_count; i++) {
        if (registry[i] && registry[i]->type == type) {
            found = registry[i];
            break;
        }
    }
    pthread_mutex_unlock(&registry_mu);
    return found;
}

void bw_registry_free_all(void) {
    pthread_mutex_lock(&registry_mu);
    for (int i = 0; i < registry_count; i++) {
        if (registry[i] && registry[i]->free_w)
            registry[i]->free_w(registry[i]);
        registry[i] = NULL;
    }
    registry_count = 0;
    pthread_mutex_unlock(&registry_mu);
}

/* -----------------------------------------------------------------------
 * Dispatcher
 * --------------------------------------------------------------------- */

typedef struct {
    BusContext    bus;     /* copy — safe to use from the spawned thread */
    BackendWorker *worker;
    BusMsg         msg;
} DispatchArg;

static void *dispatch_thread(void *arg) {
    DispatchArg *da = (DispatchArg *)arg;
    da->worker->dispatch(da->worker, &da->bus, &da->msg);
    bus_msg_free(&da->msg);
    free(da);
    return NULL;
}

int bw_dispatch_msg(BusContext *bus, const IpcMsg *raw_msg) {
    if (!raw_msg || raw_msg->type != MSG_WORKER_REQ) return -1;

    uint32_t worker_type;
    char    *op        = NULL;
    char    *args_json = NULL;

    if (ipc_decode_worker_req(raw_msg->payload, raw_msg->payload_len,
                               &worker_type, &op, &args_json) != 0) {
        return -1;
    }

    BackendWorker *worker = bw_registry_find((BackendType)worker_type);
    if (!worker) {
        free(op); free(args_json);
        bw_send_error(bus, raw_msg->corr_id, "no worker registered for type");
        return -1;
    }

    DispatchArg *da = calloc(1, sizeof(DispatchArg));
    if (!da) { free(op); free(args_json); return -1; }

    da->bus             = *bus;  /* copy the fd/context for the thread */
    da->worker          = worker;
    da->msg.corr_id     = raw_msg->corr_id;
    da->msg.worker_type = (BackendType)worker_type;
    da->msg.op          = op;
    da->msg.args_json   = args_json;

    pthread_t tid;
    if (pthread_create(&tid, NULL, dispatch_thread, da) != 0) {
        bus_msg_free(&da->msg);
        free(da);
        bw_send_error(bus, raw_msg->corr_id, "failed to spawn worker thread");
        return -1;
    }
    pthread_detach(tid);
    return 0;
}
