#ifndef HU_PROVIDERS_GEMINI_H
#define HU_PROVIDERS_GEMINI_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

/* True when `base` targets a Vertex AI endpoint (aiplatform.googleapis.com).
 * Vertex authenticates ONLY with an OAuth2 bearer token from Application
 * Default Credentials and rejects `?key=` API keys with HTTP 401; the
 * generative-language endpoint is the opposite (API key, no bearer). Exposed
 * so the request builder and its tests share one definition of "which auth". */
bool hu_gemini_base_is_vertex(const char *base, size_t len);

hu_error_t hu_gemini_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                            const char *base_url, size_t base_url_len, hu_provider_t *out);

/* Create Gemini provider with OAuth Bearer token (no API key in URL).
 * Use when GEMINI_OAUTH_TOKEN env var or similar supplies a ya29.* token. */
hu_error_t hu_gemini_create_with_oauth(hu_allocator_t *alloc, const char *oauth_token,
                                       size_t oauth_token_len, const char *base_url,
                                       size_t base_url_len, hu_provider_t *out);

#endif
