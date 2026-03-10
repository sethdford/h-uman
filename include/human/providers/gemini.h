#ifndef HU_PROVIDERS_GEMINI_H
#define HU_PROVIDERS_GEMINI_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>

hu_error_t hu_gemini_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                            const char *base_url, size_t base_url_len, hu_provider_t *out);

/* Create Gemini provider with OAuth Bearer token (no API key in URL).
 * Use when GEMINI_OAUTH_TOKEN env var or similar supplies a ya29.* token. */
hu_error_t hu_gemini_create_with_oauth(hu_allocator_t *alloc, const char *oauth_token,
                                       size_t oauth_token_len, const char *base_url,
                                       size_t base_url_len, hu_provider_t *out);

#endif
