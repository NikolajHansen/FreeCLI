#include "provider.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#define XAI_URL "https://api.x.ai/v1/chat/completions"
#define XAI_DEFAULT_MODEL "grok-3-fast"


typedef struct {
    char *api_key;
} XAIPriv;

/* --- HTTP response buffer --- */
typedef struct {
    char  *data;
    size_t len;
} RespBuf;

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t n = size * nmemb;
    RespBuf *rb = (RespBuf *)userp;
    rb->data = realloc(rb->data, rb->len + n + 1);
    if (!rb->data) return 0;
    memcpy(rb->data + rb->len, contents, n);
    rb->len += n;
    rb->data[rb->len] = '\0';
    return n;
}

/* --- Read API key from ~/.grok/user-settings.json --- */
static char *read_api_key(void) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (!pw) return NULL;
        home = pw->pw_dir;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/.grok/user-settings.json", home);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return NULL;

    cJSON *key_item = cJSON_GetObjectItemCaseSensitive(root, "apiKey");
    char *key = NULL;
    if (cJSON_IsString(key_item) && key_item->valuestring)
        key = strdup(key_item->valuestring);

    cJSON_Delete(root);
    return key;
}

/* --- Build the ask_ai tool definition --- */
static void add_ask_ai_tool(cJSON *tools_array) {
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "function");

    cJSON *fn = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(fn, "name", "ask_ai");
    cJSON_AddStringToObject(fn, "description",
        "Query another AI model for specialized knowledge, a second opinion, "
        "or to delegate a sub-task. The result will be returned to you as a "
        "tool response so you can incorporate it into your final answer.");

    cJSON *params = cJSON_AddObjectToObject(fn, "parameters");
    cJSON_AddStringToObject(params, "type", "object");

    cJSON *props = cJSON_AddObjectToObject(params, "properties");

    cJSON *model_prop = cJSON_AddObjectToObject(props, "model");
    cJSON_AddStringToObject(model_prop, "type", "string");
    cJSON_AddStringToObject(model_prop, "description",
        "The AI model to query (e.g. 'grok-3' for deeper reasoning, "
        "'grok-3-mini' for quick facts)");
    cJSON *model_enum = cJSON_AddArrayToObject(model_prop, "enum");
    for (int i = 0; provider_xai_models[i]; i++)
        cJSON_AddItemToArray(model_enum, cJSON_CreateString(provider_xai_models[i]));

    cJSON *prompt_prop = cJSON_AddObjectToObject(props, "prompt");
    cJSON_AddStringToObject(prompt_prop, "type", "string");
    cJSON_AddStringToObject(prompt_prop, "description",
        "The question or task to send to the other AI");

    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("model"));
    cJSON_AddItemToArray(required, cJSON_CreateString("prompt"));

    cJSON_AddItemToArray(tools_array, tool);
}

/* --- Build JSON request body --- */
static char *build_body(const ChatBuffer *history, const char *model,
                         int enable_tools) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model",
                            (model && *model) ? model : XAI_DEFAULT_MODEL);
    cJSON_AddNumberToObject(root, "temperature", 0.7);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    /* System message describing tool capability */
    if (enable_tools) {
        char sysprompt[1024];
        char models_list[256] = "";
        for (int i = 0; provider_xai_models[i]; i++) {
            if (i > 0) strncat(models_list, ", ",
                               sizeof(models_list) - strlen(models_list) - 1);
            strncat(models_list, provider_xai_models[i],
                    sizeof(models_list) - strlen(models_list) - 1);
        }
        snprintf(sysprompt, sizeof(sysprompt),
            "You are a helpful AI assistant with access to inter-AI collaboration "
            "via the ask_ai tool. Use it when querying another model adds genuine "
            "value: e.g. for a second opinion, specialized knowledge, or delegating "
            "a sub-task. Available models: %s. "
            "Use tools thoughtfully — not for every question.",
            models_list);
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", sysprompt);
        cJSON_AddItemToArray(messages, sys);
    }

    for (int i = 0; i < history->count; i++) {
        const Message *m = &history->msgs[i];
        cJSON *msg_obj = cJSON_CreateObject();

        if (m->tool_call_id) {
            /* role: tool — result of a previous tool call */
            cJSON_AddStringToObject(msg_obj, "role", "tool");
            cJSON_AddStringToObject(msg_obj, "tool_call_id", m->tool_call_id);
            cJSON_AddStringToObject(msg_obj, "content",
                                    m->text ? m->text : "");
        } else if (!m->is_user && m->n_tool_calls > 0) {
            /* role: assistant — with tool_calls */
            cJSON_AddStringToObject(msg_obj, "role", "assistant");
            if (m->text && *m->text)
                cJSON_AddStringToObject(msg_obj, "content", m->text);
            else
                cJSON_AddNullToObject(msg_obj, "content");

            cJSON *tc_arr = cJSON_AddArrayToObject(msg_obj, "tool_calls");
            for (int t = 0; t < m->n_tool_calls; t++) {
                cJSON *tc = cJSON_CreateObject();
                cJSON_AddStringToObject(tc, "id",   m->tool_calls[t].id   ? m->tool_calls[t].id   : "");
                cJSON_AddStringToObject(tc, "type", "function");
                cJSON *fn = cJSON_AddObjectToObject(tc, "function");
                cJSON_AddStringToObject(fn, "name",
                    m->tool_calls[t].name ? m->tool_calls[t].name : "");
                cJSON_AddStringToObject(fn, "arguments",
                    m->tool_calls[t].arguments ? m->tool_calls[t].arguments : "{}");
                cJSON_AddItemToArray(tc_arr, tc);
            }
        } else {
            cJSON_AddStringToObject(msg_obj, "role",
                                    m->is_user ? "user" : "assistant");
            cJSON_AddStringToObject(msg_obj, "content",
                                    m->text ? m->text : "");
        }

        cJSON_AddItemToArray(messages, msg_obj);
    }

    if (enable_tools) {
        cJSON *tools = cJSON_AddArrayToObject(root, "tools");
        add_ask_ai_tool(tools);
        cJSON_AddStringToObject(root, "tool_choice", "auto");
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* --- Provider interface --- */

static int xai_init(Provider *p) {
    XAIPriv *priv = calloc(1, sizeof(XAIPriv));
    if (!priv) return 0;
    priv->api_key = read_api_key();
    if (!priv->api_key) { free(priv); return 0; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    p->priv = priv;
    return 1;
}

static ProviderReply *xai_send(Provider *p, const ProviderRequest *req) {
    XAIPriv *priv = (XAIPriv *)p->priv;
    if (!priv || !priv->api_key) return NULL;

    char *body = build_body(req->history, req->model, req->enable_tools);
    if (!body) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return NULL; }

    struct curl_slist *headers = NULL;
    char auth[512];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", priv->api_key);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    RespBuf rb = { NULL, 0 };

    curl_easy_setopt(curl, CURLOPT_URL, XAI_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    if (res != CURLE_OK || http_code != 200 || !rb.data) {
        free(rb.data);
        return NULL;
    }

    ProviderReply *reply = calloc(1, sizeof(ProviderReply));
    if (!reply) { free(rb.data); return NULL; }

    cJSON *root = cJSON_Parse(rb.data);
    free(rb.data);
    if (root) {
        cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON *choice   = cJSON_GetArrayItem(choices, 0);
            cJSON *message  = cJSON_GetObjectItemCaseSensitive(choice, "message");
            cJSON *content  = cJSON_GetObjectItemCaseSensitive(message, "content");
            cJSON *rcontent = cJSON_GetObjectItemCaseSensitive(message, "reasoning_content");
            cJSON *tc_arr   = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");

            if (cJSON_IsString(content) && content->valuestring)
                reply->content   = strdup(content->valuestring);
            if (cJSON_IsString(rcontent) && rcontent->valuestring)
                reply->reasoning = strdup(rcontent->valuestring);

            /* Parse tool_calls array */
            if (cJSON_IsArray(tc_arr)) {
                int n = cJSON_GetArraySize(tc_arr);
                reply->tool_calls = calloc(n, sizeof(ToolCall));
                if (reply->tool_calls) {
                    reply->n_tool_calls = n;
                    for (int i = 0; i < n; i++) {
                        cJSON *tc   = cJSON_GetArrayItem(tc_arr, i);
                        cJSON *id   = cJSON_GetObjectItemCaseSensitive(tc, "id");
                        cJSON *fn   = cJSON_GetObjectItemCaseSensitive(tc, "function");
                        cJSON *name = fn ? cJSON_GetObjectItemCaseSensitive(fn, "name") : NULL;
                        cJSON *args = fn ? cJSON_GetObjectItemCaseSensitive(fn, "arguments") : NULL;
                        if (cJSON_IsString(id))   reply->tool_calls[i].id        = strdup(id->valuestring);
                        if (cJSON_IsString(name)) reply->tool_calls[i].name      = strdup(name->valuestring);
                        if (cJSON_IsString(args)) reply->tool_calls[i].arguments = strdup(args->valuestring);
                    }
                }
            }
        }
        cJSON_Delete(root);
    }

    return reply;
}

static void xai_free_p(Provider *p) {
    if (!p->priv) return;
    XAIPriv *priv = (XAIPriv *)p->priv;
    free(priv->api_key);
    free(priv);
    curl_global_cleanup();
    p->priv = NULL;
}

Provider provider_xai = {
    .priv   = NULL,
    .init   = xai_init,
    .send   = xai_send,
    .free_p = xai_free_p,
};
