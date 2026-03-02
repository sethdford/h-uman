#ifndef SC_PROVIDERS_GEMINI_H
#define SC_PROVIDERS_GEMINI_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include <stddef.h>

sc_error_t sc_gemini_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out);

/* Create Gemini provider with OAuth Bearer token (no API key in URL).
 * Use when GEMINI_OAUTH_TOKEN env var or similar supplies a ya29.* token. */
sc_error_t sc_gemini_create_with_oauth(sc_allocator_t *alloc,
    const char *oauth_token, size_t oauth_token_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out);

#endif
