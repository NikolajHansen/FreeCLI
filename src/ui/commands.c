#include "commands.h"
#include "overlay.h"
#include "provider.h"
#include "provider_registry.h"
#include "chat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *HELP_TEXT =
    "Available commands:\n"
    "  /new [name]      create a new chat session\n"
    "  /delete          delete the current session\n"
    "  /clear           clear current session messages\n"
    "  /rename <name>   rename current session\n"
    "  /provider        choose AI provider\n"
    "  /model           choose model for current provider\n"
    "  /choices [1-5]   request N alternative responses to pick from\n"
    "  /help            show this help";

static const char *CMD_HINT =
    "/new  /delete  /clear  /rename <name>  /provider  /model  /choices [1-5]  /help";

const char *cmd_hint(const char *partial) {
    if (!partial || partial[0] != '/') return NULL;
    return CMD_HINT;
}

static void system_msg(SessionManager *sm, const char *text) {
    ChatBuffer *cb = sm_active_chat(sm);
    if (cb) chat_add(cb, text, false);
}

int cmd_dispatch(const char *input, CmdCtx *ctx) {
    if (!input || input[0] != '/') return 0;
    SessionManager *sm = ctx->sessions;

    /* /help */
    if (strncmp(input, "/help", 5) == 0 &&
        (input[5] == '\0' || input[5] == ' ')) {
        system_msg(sm, HELP_TEXT);
        ctx->draw();
        return 1;
    }

    /* /delete — delete current session */
    if (strncmp(input, "/delete", 7) == 0 &&
        (input[7] == '\0' || input[7] == ' ')) {
        if (ctx->delete_session)
            ctx->delete_session(sm->active);
        ctx->draw();
        return 1;
    }

    /* /clear */
    if (strncmp(input, "/clear", 6) == 0 &&
        (input[6] == '\0' || input[6] == ' ')) {
        ChatBuffer *cb = sm_active_chat(sm);
        if (cb) {
            int saved_id = sm_active_session(sm) ? sm_active_session(sm)->id : -1;
            chat_free(cb);
            chat_init(cb);
            if (saved_id >= 0) ctx->save_session(sm, sm->active);
            ctx->draw();
        }
        return 1;
    }

    /* /new [name] */
    if (strncmp(input, "/new", 4) == 0 &&
        (input[4] == '\0' || input[4] == ' ')) {
        const char *name = (input[4] == ' ' && input[5] != '\0') ? input + 5 : NULL;
        char default_name[64];
        if (!name) {
            snprintf(default_name, sizeof(default_name),
                     "Chat %d", sm_count(sm) + 1);
            name = default_name;
        }
        int idx = sm_new_session(sm, name);
        if (idx >= 0) {
            sm_select(sm, idx);
            *ctx->sidebar_sel = idx;
            *ctx->focus = 2;
            ctx->save_session(sm, -1);
        }
        ctx->draw();
        return 1;
    }

    /* /rename <name> */
    if (strncmp(input, "/rename", 7) == 0 &&
        input[7] == ' ' && input[8] != '\0') {
        Session *s = sm_active_session(sm);
        if (s) {
            strncpy(s->name, input + 8, 63);
            s->name[63] = '\0';
            ctx->save_session(sm, -1);
            ctx->draw();
        }
        return 1;
    }

    /* /provider — pick AI provider */
    if (strncmp(input, "/provider", 9) == 0 &&
        (input[9] == '\0' || input[9] == ' ')) {
        /* Build display-name list */
        const char **names = malloc(providers_count * sizeof(char *));
        if (!names) return 1;
        for (int i = 0; i < providers_count; i++)
            names[i] = providers[i].display_name;

        /* Find current provider index */
        int cur = 0;
        if (ctx->current_provider) {
            for (int i = 0; i < providers_count; i++) {
                if (strcmp(providers[i].id, ctx->current_provider) == 0) {
                    cur = i; break;
                }
            }
        }

        int picked = overlay_pick("Select Provider", names, providers_count, cur);
        free(names);
        ctx->draw();

        if (picked >= 0 && picked < providers_count) {
            if (ctx->set_provider)
                ctx->set_provider(providers[picked].id);
            /* Reset model to this provider's default */
            if (ctx->set_model && providers[picked].models)
                ctx->set_model(providers[picked].models[0]);
            char msg[128];
            snprintf(msg, sizeof(msg), "Provider: %s  Model: %s",
                     providers[picked].display_name,
                     providers[picked].models ? providers[picked].models[0] : "?");
            system_msg(sm, msg);
            ctx->draw();
        }
        return 1;
    }

    /* /model — pick model for current provider */
    if (strncmp(input, "/model", 6) == 0 &&
        (input[6] == '\0' || input[6] == ' ')) {
        const ProviderInfo *pi = provider_registry_find(ctx->current_provider);
        if (!pi) pi = &providers[0];

        int n = 0;
        while (pi->models[n]) n++;

        int cur = 0;
        if (ctx->current_model) {
            for (int i = 0; i < n; i++) {
                if (strcmp(pi->models[i], ctx->current_model) == 0) {
                    cur = i; break;
                }
            }
        }

        int picked = overlay_pick("Select Model", pi->models, n, cur);
        ctx->draw();

        if (picked >= 0 && picked < n && ctx->set_model) {
            ctx->set_model(pi->models[picked]);
            char msg[128];
            snprintf(msg, sizeof(msg), "Model set to: %s", pi->models[picked]);
            system_msg(sm, msg);
            ctx->draw();
        }
        return 1;
    }

    /* /choices [N] — set number of response alternatives */
    if (strncmp(input, "/choices", 8) == 0 &&
        (input[8] == '\0' || input[8] == ' ')) {
        const char *arg = (input[8] == ' ') ? input + 9 : NULL;
        if (arg && *arg) {
            int n = atoi(arg);
            if (n >= 1 && n <= 5) {
                if (ctx->set_n_choices) ctx->set_n_choices(n);
                char msg[64];
                snprintf(msg, sizeof(msg),
                         n == 1 ? "Choices: 1 (single response)"
                                : "Choices: %d — next reply will offer %d alternatives",
                         n, n);
                system_msg(sm, msg);
            } else {
                system_msg(sm, "Usage: /choices 1-5");
            }
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "Choices: %d (use /choices 1-5 to change)",
                     ctx->n_choices_val);
            system_msg(sm, msg);
        }
        ctx->draw();
        return 1;
    }

    char errmsg[128];
    snprintf(errmsg, sizeof(errmsg),
             "Unknown command: %s  (try /help)", input);
    system_msg(sm, errmsg);
    ctx->draw();
    return 1;
}
