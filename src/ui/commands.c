#include "commands.h"
#include "overlay.h"
#include "provider.h"
#include "provider_registry.h"
#include "chat.h"
#include "cJSON.h"
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
    "  /fs list [path]           list sandbox directory\n"
    "  /fs read <path>           read a file from the sandbox\n"
    "  /fs delete <path>         delete a file (requires approval)\n"
    "  /fs create <path> [text]  create a file (requires approval)\n"
    "  /fs write <path> [text]   overwrite a file (requires approval)\n"
    "  /help            show this help";

static const char *CMD_HINT =
    "/new  /delete  /clear  /rename  /provider  /model  /fs  /help";

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

    /* /fs <op> [path] [content...]
     *
     * Sends a MSG_WORKER_REQ to the filesystem worker.
     * All paths are relative to the sandbox root.
     *
     * Ops:
     *   /fs list [path]           — list directory (default: root)
     *   /fs read <path>           — read file contents
     *   /fs delete <path>         — delete file (requires Y/N approval)
     *   /fs create <path> [text]  — create file (requires Y/N approval)
     *   /fs write  <path> [text]  — overwrite file (requires Y/N approval)
     */
    if (strncmp(input, "/fs", 3) == 0 &&
        (input[3] == '\0' || input[3] == ' ')) {

        if (!ctx->send_worker_req) {
            system_msg(sm, "[fs] worker not available");
            ctx->draw();
            return 1;
        }

        /* Parse: /fs <op> [rest] */
        const char *rest = input + 3;
        while (*rest == ' ') rest++;

        if (*rest == '\0') {
            system_msg(sm, "/fs: missing operation  (list|read|delete|create|write)");
            ctx->draw();
            return 1;
        }

        /* Extract op token */
        char op_name[32];
        const char *after_op = rest;
        int op_len = 0;
        while (*after_op && *after_op != ' ') { after_op++; op_len++; }
        if (op_len >= (int)sizeof(op_name)) op_len = (int)sizeof(op_name) - 1;
        memcpy(op_name, rest, (size_t)op_len);
        op_name[op_len] = '\0';

        /* Skip space after op */
        while (*after_op == ' ') after_op++;

        /* Map to backend op name */
        const char *backend_op = NULL;
        if      (strcmp(op_name, "list")   == 0) backend_op = "list_dir";
        else if (strcmp(op_name, "read")   == 0) backend_op = "read_file";
        else if (strcmp(op_name, "delete") == 0) backend_op = "delete_file";
        else if (strcmp(op_name, "create") == 0) backend_op = "create_file";
        else if (strcmp(op_name, "write")  == 0) backend_op = "modify_file";

        if (!backend_op) {
            char errbuf[96];
            snprintf(errbuf, sizeof(errbuf),
                     "/fs: unknown op '%s'  (list|read|delete|create|write)", op_name);
            system_msg(sm, errbuf);
            ctx->draw();
            return 1;
        }

        /* Extract path (first word after op) */
        char path[1024] = "";
        const char *after_path = after_op;
        if (*after_op) {
            int plen = 0;
            while (after_path[plen] && after_path[plen] != ' ') plen++;
            if (plen >= (int)sizeof(path)) plen = (int)sizeof(path) - 1;
            memcpy(path, after_op, (size_t)plen);
            path[plen] = '\0';
            after_path += plen;
            while (*after_path == ' ') after_path++;
        }

        /* Build JSON args */
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "path", path);

        /* content: rest of line after path (for create/write) */
        if ((strcmp(backend_op, "create_file") == 0 ||
             strcmp(backend_op, "modify_file")  == 0) && *after_path) {
            cJSON_AddStringToObject(args, "content", after_path);
        } else if (strcmp(backend_op, "create_file") == 0 ||
                   strcmp(backend_op, "modify_file")  == 0) {
            cJSON_AddStringToObject(args, "content", "");
        }

        char *args_json = cJSON_PrintUnformatted(args);
        cJSON_Delete(args);

        /* BACKEND_TYPE_FILESYSTEM == 4 */
        uint64_t corr = ctx->send_worker_req(4, backend_op, args_json);
        free(args_json);

        if (corr == 0) {
            system_msg(sm, "[fs] failed to send request to backend");
        } else {
            char pending_msg[128];
            snprintf(pending_msg, sizeof(pending_msg),
                     "[fs] %s %s — waiting…", op_name, path);
            system_msg(sm, pending_msg);
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
