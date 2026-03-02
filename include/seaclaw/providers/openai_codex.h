#ifndef SC_OPENAI_CODEX_H
#define SC_OPENAI_CODEX_H

#include "seaclaw/provider.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"

#define SC_OPENAI_CODEX_URL "https://chatgpt.com/backend-api/codex/responses"
#define SC_OPENAI_CODEX_PREFIX "openai-codex/"

sc_error_t sc_openai_codex_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out);

#endif /* SC_OPENAI_CODEX_H */
