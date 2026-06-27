#ifndef COMMANDS_H
#define COMMANDS_H

#include "session.h"
#include <ncurses.h>

typedef struct {
    SessionManager *sessions;
    int            *sidebar_sel;
    int            *focus;
    void          (*draw)(void);
    void          (*save_session)(const SessionManager *sm, int idx);
    void          (*delete_session)(int idx);   /* delete session by index */
    void          (*set_model)(const char *model);
    void          (*set_provider)(const char *provider_id);
    void          (*set_n_choices)(int n);
    const char     *current_model;
    const char     *current_provider;  /* "xai", "anthropic", "google", "ibm" */
    int             n_choices_val;     /* current value (1-5) */
} CmdCtx;

int         cmd_dispatch(const char *input, CmdCtx *ctx);
const char *cmd_hint(const char *partial);

#endif
