#include "provider.h"

/* NULL-terminated list of xAI models. First entry is the default.
 * Compiled into both freecli (TUI) and freecli-backend. */
const char * const provider_xai_models[] = {
    "grok-3-fast",
    "grok-3",
    "grok-3-mini-fast",
    "grok-3-mini",
    "grok-2-1212",
    NULL
};
