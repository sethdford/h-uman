#ifndef HU_PROVIDERS_HELPERS_H
#define HU_PROVIDERS_HELPERS_H

#include "human/core/allocator.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

/* Check if model name indicates a reasoning model (o1, o3, o4-mini, gpt-5*, codex-mini) */
bool hu_helpers_is_reasoning_model(const char *model, size_t model_len);

/* Extract text content from OpenAI-style JSON response (choices[0].message.content) */
char *hu_helpers_extract_openai_content(hu_allocator_t *alloc, const char *body, size_t body_len);

/* Extract text content from Anthropic-style JSON response (content[0].text) */
char *hu_helpers_extract_anthropic_content(hu_allocator_t *alloc, const char *body,
                                           size_t body_len);

#endif /* HU_PROVIDERS_HELPERS_H */
