#ifndef COMMANDS_H
#define COMMANDS_H

#include "session.h"
#include <ncurses.h>
#include <stdint.h>

typedef struct {
    SessionManager *sessions;
    int            *sidebar_sel;
    int            *focus;
    void          (*draw)(void);
    void          (*save_session)(const SessionManager *sm, int idx);
    void          (*delete_session)(int idx);
    void          (*set_model)(const char *model);
    void          (*set_provider)(const char *provider_id);
    const char     *current_model;
    const char     *current_provider;
    /*
     * send_worker_req — invoke a backend worker operation.
     * worker_type matches BackendType enum values (uint32_t to avoid
     * pulling backend_worker.h into the UI layer).
     * args_json: JSON-encoded arguments string (may be NULL).
     * Returns the corr_id assigned to the request, or 0 on failure.
     */
    uint64_t      (*send_worker_req)(uint32_t worker_type,
                                     const char *op,
                                     const char *args_json);
} CmdCtx;

int         cmd_dispatch(const char *input, CmdCtx *ctx);
const char *cmd_hint(const char *partial);

#endif
