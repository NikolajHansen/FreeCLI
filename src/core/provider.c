#include "provider.h"
#include <stdlib.h>

void provider_reply_free(ProviderReply *reply) {
    if (!reply) return;
    free(reply->content);
    free(reply->reasoning);
    toolcall_array_free(reply->tool_calls, reply->n_tool_calls);
    free(reply->tool_calls);
    if (reply->all_choices) {
        for (int i = 0; i < reply->n_choices; i++)
            free(reply->all_choices[i]);
        free(reply->all_choices);
    }
    free(reply);
}
