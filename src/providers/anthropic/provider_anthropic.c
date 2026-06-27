/*
 * Anthropic Claude provider for FreeCLI.
 *
 * API: POST https://api.anthropic.com/v1/messages
 * Auth: x-api-key header
 * Key: ANTHROPIC_API_KEY env var, or ~/.anthropic/api_key plain-text file
 */

#include "provider.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANTHROPIC_URL     "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION "2023-06-01"
#define ANTHROPIC_DEFAULT_MODEL "claude-opus-4-5"
#define ANTHROPIC_MAX_TOKENS    4096

typedef struct { char *api_key; } AnthropicPriv;

typedef struct { char *data; size_t len; } RespBuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t n = size * nmemb;
    RespBuf *rb = (RespBuf *)userp;
    rb->data = realloc(rb->data, rb->len + n + 1);
    if (!rb->data) return 0;
    memcpy(rb->data + rb->len, ptr, n);
    rb->len += n;
    rb->data[rb->len] = '\0';
    return n;
}

static char *read_api_key(void) {
    /* 1. Environment variable */
    const char *env = getenv("ANTHROPIC_API_KEY");
    if (env && *env) return strdup(env);

    /* 2. ~/.anthropic/api_key */
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char path[512];
    snprintf(path, sizeof(path), "%s/.anthropic/api_key", home);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[256] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        /* strip trailing newline */
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
    }
    fclose(f);
    return (*buf) ? strdup(buf) : NULL;
}

static char *build_body(const ChatBuffer *history, const char *model) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model",
                            (model && *model) ? model : ANTHROPIC_DEFAULT_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", ANTHROPIC_MAX_TOKENS);

    /* Anthropic: system prompt is top-level, not a message */
    cJSON_AddStringToObject(root, "system",
        "You are a helpful AI assistant.");

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    for (int i = 0; i < history->count; i++) {
        const Message *m = &history->msgs[i];
        /* Skip tool-call internals — Anthropic tool calling is separate;
         * for now emit as plain assistant/user turns */
        if (m->tool_call_id) continue;
        if (m->n_tool_calls > 0 && !m->text) continue;

        const char *role = m->is_user ? "user" : "assistant";
        const char *text = m->text ? m->text : "";

        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", role);
        cJSON_AddStringToObject(msg, "content", text);
        cJSON_AddItemToArray(messages, msg);
    }

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static int anthropic_init(Provider *p) {
    AnthropicPriv *priv = calloc(1, sizeof(AnthropicPriv));
    if (!priv) return 0;
    priv->api_key = read_api_key();
    if (!priv->api_key) { free(priv); return 0; }
    p->priv = priv;
    return 1;
}

static ProviderReply *anthropic_send(Provider *p, const ProviderRequest *req) {
    AnthropicPriv *priv = (AnthropicPriv *)p->priv;
    if (!priv || !priv->api_key) return NULL;

    char *body = build_body(req->history, req->model);
    if (!body) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return NULL; }

    struct curl_slist *headers = NULL;
    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "x-api-key: %s", priv->api_key);
    headers = curl_slist_append(headers, auth_hdr);
    headers = curl_slist_append(headers, "anthropic-version: " ANTHROPIC_VERSION);
    headers = curl_slist_append(headers, "content-type: application/json");

    RespBuf rb = { NULL, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, ANTHROPIC_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

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
    cJSON *root = cJSON_Parse(rb.data);
    free(rb.data);
    if (root) {
        /* response: {"content": [{"type": "text", "text": "..."}]} */
        cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
        if (cJSON_IsArray(content) && cJSON_GetArraySize(content) > 0) {
            cJSON *block = cJSON_GetArrayItem(content, 0);
            cJSON *text  = cJSON_GetObjectItemCaseSensitive(block, "text");
            if (cJSON_IsString(text) && text->valuestring)
                reply->content = strdup(text->valuestring);
        }
        cJSON_Delete(root);
    }
    return reply;
}

static void anthropic_free_p(Provider *p) {
    if (!p->priv) return;
    AnthropicPriv *priv = (AnthropicPriv *)p->priv;
    free(priv->api_key);
    free(priv);
    p->priv = NULL;
}

Provider provider_anthropic = {
    .priv   = NULL,
    .init   = anthropic_init,
    .send   = anthropic_send,
    .free_p = anthropic_free_p,
};
