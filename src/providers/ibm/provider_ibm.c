/*
 * IBM watsonx provider for FreeCLI.
 *
 * Auth: IBM IAM token exchange (POST iam.cloud.ibm.com/identity/token)
 *   Token is cached and refreshed before expiry (~55 min window).
 * API:  POST https://{region}.ml.cloud.ibm.com/ml/v1/text/chat?version=2023-05-29
 * Keys: WATSONX_API_KEY + WATSONX_PROJECT_ID + WATSONX_REGION (default: us-south)
 *   or  ~/.ibm/watsonx.json: {"apiKey":"...","projectId":"...","region":"us-south"}
 */

#include "provider.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IBM_IAM_URL        "https://iam.cloud.ibm.com/identity/token"
#define IBM_DEFAULT_REGION "us-south"
#define IBM_DEFAULT_MODEL  "ibm/granite-3-8b-instruct"
#define IBM_API_VERSION    "2023-05-29"
#define IBM_MAX_TOKENS     2048
/* Refresh token 5 minutes before the 60-minute expiry */
#define IBM_TOKEN_TTL_SEC  3300

typedef struct {
    char  *api_key;
    char  *project_id;
    char  *region;
    char  *iam_token;      /* cached access token */
    time_t token_expiry;   /* unix time when we must refresh */
} IBMPriv;

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

/* Read config from env or ~/.ibm/watsonx.json */
static int read_config(char **api_key, char **project_id, char **region) {
    *api_key = *project_id = *region = NULL;

    const char *env_key  = getenv("WATSONX_API_KEY");
    const char *env_proj = getenv("WATSONX_PROJECT_ID");
    const char *env_reg  = getenv("WATSONX_REGION");

    if (env_key  && *env_key)  *api_key    = strdup(env_key);
    if (env_proj && *env_proj) *project_id = strdup(env_proj);
    if (env_reg  && *env_reg)  *region     = strdup(env_reg);

    /* Fill gaps from config file */
    if (!*api_key || !*project_id) {
        const char *home = getenv("HOME");
        if (home) {
            char path[512];
            snprintf(path, sizeof(path), "%s/.ibm/watsonx.json", home);
            FILE *f = fopen(path, "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f); rewind(f);
                char *buf = malloc(sz + 1);
                if (buf) {
                    if (fread(buf, 1, sz, f) == (size_t)sz) {
                        buf[sz] = '\0';
                        cJSON *root = cJSON_Parse(buf);
                        if (root) {
                            cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "apiKey");
                            cJSON *pr = cJSON_GetObjectItemCaseSensitive(root, "projectId");
                            cJSON *rg = cJSON_GetObjectItemCaseSensitive(root, "region");
                            if (!*api_key    && cJSON_IsString(k)  && k->valuestring)
                                *api_key    = strdup(k->valuestring);
                            if (!*project_id && cJSON_IsString(pr) && pr->valuestring)
                                *project_id = strdup(pr->valuestring);
                            if (!*region     && cJSON_IsString(rg) && rg->valuestring)
                                *region     = strdup(rg->valuestring);
                            cJSON_Delete(root);
                        }
                    }
                    free(buf);
                }
                fclose(f);
            }
        }
    }

    if (!*region) *region = strdup(IBM_DEFAULT_REGION);
    return (*api_key && *project_id) ? 1 : 0;
}

/* Exchange API key for IAM access token. Returns heap string or NULL. */
static char *iam_exchange(const char *api_key) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char post_data[512];
    snprintf(post_data, sizeof(post_data),
             "grant_type=urn%%3Aibm%%3Aparams%%3Aoauth%%3Agrant-type%%3Aapikey"
             "&apikey=%s", api_key);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers,
        "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Accept: application/json");

    RespBuf rb = { NULL, 0 };
    curl_easy_setopt(curl, CURLOPT_URL, IBM_IAM_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(post_data));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    char *token = NULL;
    if (res == CURLE_OK && http_code == 200 && rb.data) {
        cJSON *root = cJSON_Parse(rb.data);
        if (root) {
            cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "access_token");
            if (cJSON_IsString(t) && t->valuestring)
                token = strdup(t->valuestring);
            cJSON_Delete(root);
        }
    }
    free(rb.data);
    return token;
}

/* Ensure we have a valid IAM token; refresh if expired. */
static int ensure_token(IBMPriv *priv) {
    if (priv->iam_token && time(NULL) < priv->token_expiry)
        return 1;
    free(priv->iam_token);
    priv->iam_token = iam_exchange(priv->api_key);
    if (!priv->iam_token) return 0;
    priv->token_expiry = time(NULL) + IBM_TOKEN_TTL_SEC;
    return 1;
}

static char *build_body(const ChatBuffer *history, const char *model,
                         const char *project_id) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model_id",
                            (model && *model) ? model : IBM_DEFAULT_MODEL);
    cJSON_AddStringToObject(root, "project_id", project_id);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    for (int i = 0; i < history->count; i++) {
        const Message *m = &history->msgs[i];
        if (m->tool_call_id) continue;
        if (m->n_tool_calls > 0 && !m->text) continue;

        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", m->is_user ? "user" : "assistant");
        cJSON_AddStringToObject(msg, "content", m->text ? m->text : "");
        cJSON_AddItemToArray(messages, msg);
    }

    cJSON *params = cJSON_AddObjectToObject(root, "parameters");
    cJSON_AddNumberToObject(params, "max_new_tokens", IBM_MAX_TOKENS);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static int ibm_init(Provider *p) {
    IBMPriv *priv = calloc(1, sizeof(IBMPriv));
    if (!priv) return 0;
    if (!read_config(&priv->api_key, &priv->project_id, &priv->region)) {
        free(priv->api_key); free(priv->project_id);
        free(priv->region);  free(priv);
        return 0;
    }
    p->priv = priv;
    return 1;
}

static ProviderReply *ibm_send(Provider *p, const ProviderRequest *req) {
    IBMPriv *priv = (IBMPriv *)p->priv;
    if (!priv) return NULL;
    if (!ensure_token(priv)) return NULL;

    const char *model = (req->model && *req->model) ? req->model : IBM_DEFAULT_MODEL;

    char url[256];
    snprintf(url, sizeof(url),
             "https://%s.ml.cloud.ibm.com/ml/v1/text/chat?version=%s",
             priv->region, IBM_API_VERSION);

    char *body = build_body(req->history, model, priv->project_id);
    if (!body) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) { free(body); return NULL; }

    struct curl_slist *headers = NULL;
    char auth_hdr[1024];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", priv->iam_token);
    headers = curl_slist_append(headers, auth_hdr);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

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
        /* /ml/v1/text/chat returns OpenAI-like choices array */
        cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON *choice  = cJSON_GetArrayItem(choices, 0);
            cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
            cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
            if (cJSON_IsString(content) && content->valuestring)
                reply->content = strdup(content->valuestring);
        }
        cJSON_Delete(root);
    }
    return reply;
}

static void ibm_free_p(Provider *p) {
    if (!p->priv) return;
    IBMPriv *priv = (IBMPriv *)p->priv;
    free(priv->api_key);
    free(priv->project_id);
    free(priv->region);
    free(priv->iam_token);
    free(priv);
    p->priv = NULL;
}

Provider provider_ibm = {
    .priv   = NULL,
    .init   = ibm_init,
    .send   = ibm_send,
    .free_p = ibm_free_p,
};
