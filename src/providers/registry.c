#include "provider_registry.h"
#include "provider.h"
#include <string.h>

const ProviderInfo providers[] = {
    { "xai",       "xAI (Grok)",        provider_xai_models       },
    { "anthropic", "Anthropic (Claude)", provider_anthropic_models },
    { "google",    "Google (Gemini)",    provider_google_models    },
    { "ibm",       "IBM (watsonx)",      provider_ibm_models       },
};

const int providers_count = 4;

const ProviderInfo *provider_registry_find(const char *id) {
    if (!id) return &providers[0];
    for (int i = 0; i < providers_count; i++)
        if (strcmp(providers[i].id, id) == 0) return &providers[i];
    return NULL;
}
