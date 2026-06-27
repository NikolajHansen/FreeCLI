#ifndef PROVIDER_REGISTRY_H
#define PROVIDER_REGISTRY_H

/*
 * Provider registry — shared between TUI (for UI display) and backend
 * (for dispatch). Only contains names and model lists; no libcurl dependency.
 */

typedef struct {
    const char         *id;           /* "xai", "anthropic", "google", "ibm" */
    const char         *display_name; /* shown in /provider overlay */
    const char * const *models;       /* NULL-terminated; first = default */
} ProviderInfo;

extern const ProviderInfo providers[];
extern const int           providers_count;

/* Find a ProviderInfo by id string. Returns NULL if not found. */
const ProviderInfo *provider_registry_find(const char *id);

#endif /* PROVIDER_REGISTRY_H */
