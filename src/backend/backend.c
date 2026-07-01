/*
 * freecli-backend — background process handling all LLM API calls.
 *
 * Usage: freecli-backend <port>
 */

#include "ipc.h"
#include "provider.h"
#include "provider_registry.h"
#include "chat.h"
#include "cJSON.h"
#include "backend_worker.h"
#include "../workers/fs_worker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifndef _WIN32
# include <locale.h>
# include <time.h>
#endif

#define MAX_TOOL_ITERS 5

/* --- Provider dispatch table --- */

typedef struct {
    const char *id;
    Provider   *provider;
    int         initialized;   /* -1=failed, 0=not tried, 1=ok */
} BackendProvider;

static BackendProvider backend_providers[] = {
    { "xai",       &provider_xai,       0 },
    { "anthropic", &provider_anthropic, 0 },
    { "google",    &provider_google,    0 },
    { "ibm",       &provider_ibm,       0 },
};
#define N_BACKEND_PROVIDERS 4

static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Lazy-init a provider. Returns the Provider* or NULL on failure. */
static Provider *get_provider(const char *id) {
    if (!id || !*id) id = "xai";
    BackendProvider *bp = NULL;
    for (int i = 0; i < N_BACKEND_PROVIDERS; i++) {
        if (strcmp(backend_providers[i].id, id) == 0) {
            bp = &backend_providers[i];
            break;
        }
    }
    if (!bp) bp = &backend_providers[0]; /* fallback to xai */

    pthread_mutex_lock(&init_mutex);
    if (bp->initialized == 0) {
        bp->initialized = bp->provider->init(bp->provider) ? 1 : -1;
        if (bp->initialized == -1)
            fprintf(stderr, "[backend] provider '%s' init failed "
                            "(check API key)\n", bp->id);
    }
    pthread_mutex_unlock(&init_mutex);

    return (bp->initialized == 1) ? bp->provider : NULL;
}

static void shutdown_providers(void) {
    for (int i = 0; i < N_BACKEND_PROVIDERS; i++) {
        if (backend_providers[i].initialized == 1)
            backend_providers[i].provider->free_p(backend_providers[i].provider);
    }
}

static pthread_mutex_t sub_corr_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t        sub_corr_next  = 0x8000000000000000ULL;

static uint64_t alloc_sub_corr(void) {
    pthread_mutex_lock(&sub_corr_mutex);
    uint64_t c = sub_corr_next++;
    pthread_mutex_unlock(&sub_corr_mutex);
    return c;
}

typedef struct {
    net_fd_t  tui_fd;
    uint64_t  corr_id;
    uint32_t  session_idx;
    char    **texts;
    uint8_t  *is_user;
    uint32_t  msg_count;
    char     *model;       /* heap-allocated, may be NULL */
    char     *provider_id; /* heap-allocated, may be NULL → defaults to "xai" */
} RequestArg;

/*
 * Execute a single ask_ai sub-query.
 * sub_model is the target model name; we look up its provider from the registry.
 * For sub-queries the model name encodes the provider implicitly (e.g. "grok-3"
 * → xai, "claude-opus-4-5" → anthropic). We try each provider in order and use
 * the first that initialises. This keeps the tool interface simple for the LLM.
 */
static char *run_subquery(net_fd_t tui_fd, uint64_t parent_corr,
                           const char *sub_model, const char *prompt) {
    uint64_t sub_corr = alloc_sub_corr();

    char desc[128];
    snprintf(desc, sizeof(desc), "%.40s", prompt);
    uint32_t ws_len;
    uint8_t *ws_payload = ipc_encode_worker_start(parent_corr,
                                                   sub_model ? sub_model : "?",
                                                   desc, &ws_len);
    if (ws_payload) {
        ipc_send(tui_fd, MSG_WORKER_START, sub_corr, ws_payload, ws_len);
        free(ws_payload);
    }

    /* Determine provider: match model name prefix heuristics */
    const char *prov_id = "xai";
    if (sub_model) {
        if (strncmp(sub_model, "claude",  6) == 0) prov_id = "anthropic";
        else if (strncmp(sub_model, "gemini", 6) == 0) prov_id = "google";
        else if (strncmp(sub_model, "ibm/",  4) == 0 ||
                 strncmp(sub_model, "meta-",  5) == 0) prov_id = "ibm";
    }

    Provider *prov = get_provider(prov_id);
    /* Fallback to xai if the target provider fails */
    if (!prov) prov = get_provider("xai");

    ChatBuffer sub_cb;
    chat_init(&sub_cb);
    chat_add(&sub_cb, prompt, true);

    ProviderRequest sub_req = {
        .history      = &sub_cb,
        .model        = sub_model,
        .enable_tools = 0,
    };
    ProviderReply *pr = prov ? prov->send(prov, &sub_req) : NULL;
    chat_free(&sub_cb);

    char *result = (pr && pr->content && *pr->content)
                   ? strdup(pr->content)
                   : strdup("(no response from sub-model)");
    provider_reply_free(pr);

    ipc_send(tui_fd, MSG_WORKER_END, sub_corr, NULL, 0);
    return result;
}

static int parse_ask_ai_args(const char *args_json,
                              char **out_model, char **out_prompt) {
    *out_model = *out_prompt = NULL;
    if (!args_json) return -1;
    cJSON *root = cJSON_Parse(args_json);
    if (!root) return -1;
    cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "model");
    cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "prompt");
    if (cJSON_IsString(m) && m->valuestring) *out_model  = strdup(m->valuestring);
    if (cJSON_IsString(p) && p->valuestring) *out_prompt = strdup(p->valuestring);
    cJSON_Delete(root);
    return (*out_model && *out_prompt) ? 0 : -1;
}

static void *handle_request(void *arg) {
    RequestArg *ra = (RequestArg *)arg;

    Provider *prov = get_provider(ra->provider_id);
    if (!prov) {
        /* All providers failed — report error */
        const char *err = "provider unavailable (check API keys)";
        ipc_send(ra->tui_fd, MSG_REP_ERROR, ra->corr_id,
                 err, (uint32_t)strlen(err));
        free(ra->model); free(ra->provider_id); free(ra);
        return NULL;
    }

    ChatBuffer cb;
    chat_init(&cb);
    for (uint32_t i = 0; i < ra->msg_count; i++) {
        chat_add(&cb, ra->texts[i], (int)ra->is_user[i]);
        free(ra->texts[i]);
    }
    free(ra->texts);
    free(ra->is_user);

    const char *model = ra->model;

    /* --- Agentic loop (tools only injected for providers that support them) --- */
    ProviderReply *pr = NULL;
    const char *final_content   = NULL;
    const char *final_reasoning = NULL;
    int iters = 0;
    /* Only xAI currently has ask_ai tool support injected.
     * Multi-choice requests bypass the agentic loop entirely. */
    int supports_tools = (ra->provider_id == NULL ||
                          strcmp(ra->provider_id, "xai") == 0);

    while (iters++ < MAX_TOOL_ITERS) {
        ProviderRequest req = {
            .history      = &cb,
            .model        = model,
            .enable_tools = supports_tools ? 1 : 0,
        };
        pr = prov->send(prov, &req);

        if (!pr) break; /* provider error */

        if (pr->n_tool_calls == 0) {
            /* Plain response — we're done */
            final_content   = pr->content;
            final_reasoning = pr->reasoning;
            break;
        }

        /* Process tool calls */
        /* First, record assistant message with tool_calls in chat history */
        ToolCall *tc_copy = calloc(pr->n_tool_calls, sizeof(ToolCall));
        if (tc_copy) {
            for (int t = 0; t < pr->n_tool_calls; t++) {
                tc_copy[t].id        = pr->tool_calls[t].id        ? strdup(pr->tool_calls[t].id)        : NULL;
                tc_copy[t].name      = pr->tool_calls[t].name      ? strdup(pr->tool_calls[t].name)      : NULL;
                tc_copy[t].arguments = pr->tool_calls[t].arguments ? strdup(pr->tool_calls[t].arguments) : NULL;
            }
            chat_add_tool_calls(&cb, pr->content, pr->reasoning,
                                tc_copy, pr->n_tool_calls);
        }

        /* Execute each tool call */
        for (int t = 0; t < pr->n_tool_calls; t++) {
            ToolCall *tc = &pr->tool_calls[t];

            if (!tc->name || strcmp(tc->name, "ask_ai") != 0) {
                /* Unknown tool — append a placeholder result */
                const char *placeholder = "(tool not available)";
                chat_add_tool_result(&cb, tc->id ? tc->id : "", placeholder);
                continue;
            }

            char *sub_model = NULL, *sub_prompt = NULL;
            if (parse_ask_ai_args(tc->arguments, &sub_model, &sub_prompt) == 0) {
                char *sub_result = run_subquery(ra->tui_fd, ra->corr_id,
                                                sub_model, sub_prompt);
                chat_add_tool_result(&cb, tc->id ? tc->id : "",
                                     sub_result ? sub_result : "(error)");
                free(sub_result);
            } else {
                chat_add_tool_result(&cb, tc->id ? tc->id : "",
                                     "(could not parse ask_ai arguments)");
            }
            free(sub_model);
            free(sub_prompt);
        }

        provider_reply_free(pr);
        pr = NULL;
    }

    /* Send final reply to TUI */
    const char *content   = final_content   ? final_content   : "";
    const char *reasoning = final_reasoning ? final_reasoning : NULL;

    {
        uint32_t  payload_len;
        uint8_t  *payload = ipc_encode_rep_final(ra->session_idx, content, reasoning,
                                                  &payload_len);
        if (payload) {
            ipc_send(ra->tui_fd, MSG_REP_FINAL, ra->corr_id, payload, payload_len);
            free(payload);
        } else {
            const char *err = "internal encode error";
            ipc_send(ra->tui_fd, MSG_REP_ERROR, ra->corr_id, err, (uint32_t)strlen(err));
        }
    }

    /* Auto-name on first turn */
    if (ra->msg_count == 1 && content && *content) {
        ChatBuffer name_cb;
        chat_init(&name_cb);
        const char *user_q = (cb.count > 0 && cb.msgs[0].is_user)
                             ? cb.msgs[0].text : NULL;
        if (user_q) {
            char naming_prompt[2048];
            snprintf(naming_prompt, sizeof(naming_prompt),
                "Summarise the following conversation in 2-4 words as a concise "
                "topic label. Reply with ONLY those words, no punctuation, no "
                "quotes, no explanation.\n\nUser: %.500s\nAssistant: %.500s",
                user_q, content);
            chat_add(&name_cb, naming_prompt, true);

            Provider *naming_prov = get_provider("xai");
            if (naming_prov) {
                ProviderRequest name_req = {
                    .history      = &name_cb,
                    .model        = "grok-3-mini-fast",
                    .enable_tools = 0,
                };
                ProviderReply *name_pr = naming_prov->send(naming_prov, &name_req);
                if (name_pr && name_pr->content && *name_pr->content) {
                    char *t = name_pr->content;
                    while (*t == ' ' || *t == '\n' || *t == '\r' || *t == '\t') t++;
                    char *end = t + strlen(t) - 1;
                    while (end > t && (*end == ' ' || *end == '\n'
                                       || *end == '\r' || *end == '\t' || *end == '.'))
                        *end-- = '\0';
                    if (*t) {
                        uint32_t rn_len;
                        uint8_t *rn_payload = ipc_encode_session_rename(
                                                 ra->session_idx, t, &rn_len);
                        if (rn_payload) {
                            ipc_send(ra->tui_fd, MSG_SESSION_RENAME, ra->corr_id,
                                     rn_payload, rn_len);
                            free(rn_payload);
                        }
                    }
                }
                provider_reply_free(name_pr);
            }
        }
        chat_free(&name_cb);
    }

    provider_reply_free(pr);
    chat_free(&cb);
    free(ra->model);
    free(ra->provider_id);
    free(ra);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: freecli-backend <port>\n");
        return 1;
    }
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 1;
    }

#ifndef _WIN32
    /* Fix LC_NUMERIC so cJSON emits '0.7' not '0,7' on locale systems */
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
#endif

    if (!net_init()) {
        fprintf(stderr, "net_init failed\n");
        return 1;
    }

    /* Initialise worker registry and register built-in workers */
    bw_registry_init();
    {
        BackendWorker *fs_w = fs_worker_create(NULL);  /* uses FREECLI_SANDBOX or ~/freecli-sandbox */
        if (fs_w && fs_w->init(fs_w) == 0) {
            bw_registry_add(fs_w);
        } else if (fs_w) {
            fs_w->free_w(fs_w);
            fprintf(stderr, "[backend] filesystem worker disabled\n");
        }
    }

    /* Eagerly init xAI (default provider); others are lazy */
    if (!provider_xai.init(&provider_xai)) {
        fprintf(stderr, "[backend] warning: xAI provider init failed "
                        "(check ~/.grok/user-settings.json)\n");
        /* Don't exit — other providers may still work */
    } else {
        backend_providers[0].initialized = 1;
    }

    net_fd_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == NET_INVALID) {
        fprintf(stderr, "socket() failed\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "connect to 127.0.0.1:%d failed\n", port);
        net_close(fd);
        return 1;
    }

    for (;;) {
        IpcMsg msg;
        int r = ipc_recv(fd, &msg);
        if (r < 0) break;
        if (r == 0) {
#ifdef _WIN32
            Sleep(20);
#else
            struct timespec ts = { 0, 20000000L };
            nanosleep(&ts, NULL);
#endif
            continue;
        }

        if (msg.type == MSG_REQ_SEND) {
            RequestArg *ra = calloc(1, sizeof(RequestArg));
            ra->tui_fd  = fd;
            ra->corr_id = msg.corr_id;

            if (ipc_decode_req_send(msg.payload, msg.payload_len,
                                     &ra->session_idx,
                                     &ra->msg_count,
                                     &ra->texts,
                                     &ra->is_user,
                                     &ra->model,
                                     &ra->provider_id) != 0) {
                free(ra);
            } else {
                pthread_t tid;
                pthread_create(&tid, NULL, handle_request, ra);
                pthread_detach(tid);
            }
        } else if (msg.type == MSG_WORKER_REQ) {
            BusContext bus = { fd };
            bw_dispatch_msg(&bus, &msg);
        } else if (msg.type == MSG_APPROVAL_REP && msg.payload) {
            uint64_t req_corr_id;
            int approved;
            if (ipc_decode_approval_rep(msg.payload, msg.payload_len,
                                         &req_corr_id, &approved) == 0) {
                bw_notify_approval(req_corr_id, approved);
            }
        }

        free(msg.payload);
    }

    net_close(fd);
    bw_registry_free_all();
    shutdown_providers();
    net_cleanup();
    return 0;
}
