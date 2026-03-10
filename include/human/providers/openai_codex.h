#ifndef HU_OPENAI_CODEX_H
#define HU_OPENAI_CODEX_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"

#define HU_OPENAI_CODEX_URL    "https://chatgpt.com/backend-api/codex/responses"
#define HU_OPENAI_CODEX_PREFIX "openai-codex/"

hu_error_t hu_openai_codex_create(hu_allocator_t *alloc, const char *api_key, size_t api_key_len,
                                  const char *base_url, size_t base_url_len, hu_provider_t *out);

#endif /* HU_OPENAI_CODEX_H */
