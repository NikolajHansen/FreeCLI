/*
 * Google Gemini provider for FreeCLI.
 *
 * API: POST https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent
 * Auth: x-goog-api-key header
 * Key: GEMINI_API_KEY (or GOOGLE_API_KEY) env var, or ~/.google/gemini_api_key
 *
 * Request format: {"contents": [{"role": "user"/"model", "parts": [{"text": "..."}]}]}
 * Response: candidates[0].content.parts[0].text
 */

#include "provider.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GOOGLE_BASE_URL   "https://generativelanguage.googleapis.com/v1beta/models/"
#define GOOGLE_DEFAULT_MODEL "gemini-2.5-flash"
#define GOOGLE_MAX_TOKENS    4096

typedef struct { char *api_key; } GooglePriv;
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
    const char *env = getenv("GEMINI_API_KEY");
    if (!env || !*env) env = getenv("GOOGLE_API_KEY");
    if (env && *env) return strdup(env);

    const char *home = getenv("HOME");
    if (!home) return NULL;
    char path[512];
    snprintf(path, sizeof(path), "%s/.google/gemini_api_key", home);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[256] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
    }
    fclose(f);
    return (*buf) ? strdup(buf) : NULL;
}

static char *build_body(const ChatBuffer *history, int n_choices) {
    cJSON *root     = cJSON_CreateObject();
    cJSON *contents = cJSON_AddArrayToObject(root, "contents");

    for (int i = 0; i < history->count; i++) {
        const Message *m = &history->msgs[i];
        if (m->tool_call_id) continue;
        if (m->n_tool_calls > 0 && !m->text) continue;

        /* Gemini uses "user" and "model" (not "assistant") */
        const char *role = m->is_user ? "user" : "model";
        const char *text = m->text ? m->text : "";

        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "role", role);
        cJSON *parts = cJSON_AddArrayToObject(entry, "parts");
        cJSON *part  = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", text);
        cJSON_AddItemToArray(parts, part);
        cJSON_AddItemToArray(contents, entry);
    }

    cJSON *gen_cfg = cJSON_AddObjectToObject(root, "generationConfig");
    cJSON_AddNumberToObject(gen_cfg, "maxOutputTokens", GOOGLE_MAX_TOKENS);
    if (n_choices > 1)
        cJSON_AddNumberToObject(gen_cfg, "candidateCount", n_choices);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static int google_init(Provider *p) {
    GooglePriv *priv = calloc(1, sizeof(GooglePriv));
    if (!priv) return 0;
    priv->api_key = read_api_key();
    if (!priv->api_key) { free(priv); return 0; }
    p->priv = priv;
    return 1;
}

static ProviderReply *google_send(Provider *p, const ProviderRequest *req) {
    GooglePriv *priv = (GooglePriv *)p->priv;
    if (!priv || !priv->api_key) return NULL;

    const char *model = (req->model && *req->model) ? req->model : GOOGLE_DEFAULT_MODEL;

    /* Build URL: base + model + ":generateContent" */
    char url[512];
    snprintf(url, sizeof(url), "%s%s:generateContent", GOOGLE_BASE_URL, model);

    char *body = build_body(req->history, req->n_choices > 1 ? req->n_choices : 1);
    if (!body) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return NULL; }

    struct curl_slist *headers = NULL;
    char key_hdr[512];
    snprintf(key_hdr, sizeof(key_hdr), "x-goog-api-key: %s", priv->api_key);
    headers = curl_slist_append(headers, key_hdr);
    headers = curl_slist_append(headers, "content-type: application/json");

    RespBuf rb = { NULL, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
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
        /* candidates[i].content.parts[0].text */
        cJSON *cands = cJSON_GetObjectItemCaseSensitive(root, "candidates");
        int nc = cJSON_IsArray(cands) ? cJSON_GetArraySize(cands) : 0;
        if (nc > 0) {
            /* Primary response from candidates[0] */
            cJSON *cand    = cJSON_GetArrayItem(cands, 0);
            cJSON *content = cJSON_GetObjectItemCaseSensitive(cand, "content");
            cJSON *parts   = cJSON_GetObjectItemCaseSensitive(content, "parts");
            if (cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0) {
                cJSON *part = cJSON_GetArrayItem(parts, 0);
                cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
                if (cJSON_IsString(text) && text->valuestring)
                    reply->content = strdup(text->valuestring);
            }

            /* Collect all candidates when nc > 1 */
            if (nc > 1) {
                reply->all_choices = calloc(nc, sizeof(char *));
                if (reply->all_choices) {
                    reply->n_choices = nc;
                    for (int i = 0; i < nc; i++) {
                        cJSON *cd  = cJSON_GetArrayItem(cands, i);
                        cJSON *cnt = cJSON_GetObjectItemCaseSensitive(cd, "content");
                        cJSON *pts = cJSON_GetObjectItemCaseSensitive(cnt, "parts");
                        if (cJSON_IsArray(pts) && cJSON_GetArraySize(pts) > 0) {
                            cJSON *pt = cJSON_GetArrayItem(pts, 0);
                            cJSON *tx = cJSON_GetObjectItemCaseSensitive(pt, "text");
                            reply->all_choices[i] = (cJSON_IsString(tx) && tx->valuestring)
                                                    ? strdup(tx->valuestring)
                                                    : strdup("");
                        } else {
                            reply->all_choices[i] = strdup("");
                        }
                    }
                }
            }
        }
        cJSON_Delete(root);
    }
    return reply;
}

static void google_free_p(Provider *p) {
    if (!p->priv) return;
    GooglePriv *priv = (GooglePriv *)p->priv;
    free(priv->api_key);
    free(priv);
    p->priv = NULL;
}

Provider provider_google = {
    .priv   = NULL,
    .init   = google_init,
    .send   = google_send,
    .free_p = google_free_p,
};
