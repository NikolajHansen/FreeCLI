#ifndef PROVIDER_H
#define PROVIDER_H

#include "chat.h"

typedef struct {
    char      *content;      /* always set (= all_choices[0] when n_choices > 1) */
    char      *reasoning;    /* may be NULL */
    ToolCall  *tool_calls;
    int        n_tool_calls;
    char     **all_choices;  /* NULL when n_choices == 1; else heap array [0..n_choices-1] */
    int        n_choices;    /* 1 = single response (default) */
} ProviderReply;

typedef struct {
    const ChatBuffer *history;
    const char       *model;
    int               enable_tools;
    int               n_choices;  /* 1 = single (default), 2-5 = request multiple candidates */
} ProviderRequest;

typedef struct Provider {
    void          *priv;
    int            (*init)(struct Provider *p);
    ProviderReply *(*send)(struct Provider *p, const ProviderRequest *req);
    void           (*free_p)(struct Provider *p);
} Provider;

void provider_reply_free(ProviderReply *reply);

/* xAI (Grok) */
extern Provider provider_xai;
extern const char * const provider_xai_models[];

/* Anthropic (Claude) */
extern Provider provider_anthropic;
extern const char * const provider_anthropic_models[];

/* Google (Gemini) */
extern Provider provider_google;
extern const char * const provider_google_models[];

/* IBM (watsonx) */
extern Provider provider_ibm;
extern const char * const provider_ibm_models[];

#endif

