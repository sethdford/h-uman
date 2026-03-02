#ifndef SC_PROVIDERS_HELPERS_H
#define SC_PROVIDERS_HELPERS_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/provider.h"
#include <stdbool.h>
#include <stddef.h>

/* Check if model name indicates a reasoning model (o1, o3, o4-mini, gpt-5*, codex-mini) */
bool sc_helpers_is_reasoning_model(const char *model, size_t model_len);

/* Extract text content from OpenAI-style JSON response (choices[0].message.content) */
char *sc_helpers_extract_openai_content(sc_allocator_t *alloc, const char *body, size_t body_len);

/* Extract text content from Anthropic-style JSON response (content[0].text) */
char *sc_helpers_extract_anthropic_content(sc_allocator_t *alloc, const char *body, size_t body_len);

#endif /* SC_PROVIDERS_HELPERS_H */
