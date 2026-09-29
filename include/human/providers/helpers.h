#ifndef HU_PROVIDERS_HELPERS_H
#define HU_PROVIDERS_HELPERS_H

#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

/* Check if model name indicates a reasoning model (o1, o3, o4-mini, gpt-5*, codex-mini) */
bool hu_helpers_is_reasoning_model(const char *model, size_t model_len);

/* Model-output scaffold stripper (2026-09-04). Local chat servers hand back the
 * chat template's thinking scaffold and the model's markdown habit verbatim: a
 * leading "<think>…</think>" block (or a bare "</think>" when the template
 * opened the block for it), then the reply — often as one ```json … ``` fence.
 * Every JSON consumer choked on that (reflection loop: 2,415 schema_invalid
 * runs in two days; init proposer), and the persona path saw "</think>"-only
 * replies as empty. Strips both in place and trims; returns true when anything
 * was removed. *len is updated and the buffer stays NUL-terminated. */
bool hu_helpers_strip_model_scaffold(char *s, size_t *len);

/* hu_strndup + hu_helpers_strip_model_scaffold, returning an exact-size copy.
 * *out_len receives the stripped length (0 when the reply was scaffold only). */
char *hu_helpers_dup_model_text(hu_allocator_t *alloc, const char *s, size_t len, size_t *out_len);

/* Extract text content from OpenAI-style JSON response (choices[0].message.content) */
char *hu_helpers_extract_openai_content(hu_allocator_t *alloc, const char *body, size_t body_len);

/* Extract text content from Anthropic-style JSON response (content[0].text) */
char *hu_helpers_extract_anthropic_content(hu_allocator_t *alloc, const char *body,
                                           size_t body_len);

/* OpenAI-style choices[0]: mean logprob over logprobs.content[].logprob when present. */
void hu_helpers_openai_choice_apply_logprobs(hu_json_value_t *choice, hu_chat_response_t *out);

#endif /* HU_PROVIDERS_HELPERS_H */
