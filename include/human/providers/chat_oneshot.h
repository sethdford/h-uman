#ifndef HU_PROVIDERS_CHAT_ONESHOT_H
#define HU_PROVIDERS_CHAT_ONESHOT_H
/* One system message, one user message, one short answer, thinking OFF.
 *
 * The request small classifiers need and chat_with_system() cannot express:
 * an explicit max_tokens and thinking_budget = 0. Gemini 3.x and GLM
 * otherwise spend the output budget reasoning (CLAUDE.md "Gemini 3.x
 * thinking-token budget gotcha"; init_proposer's 2026-05-26 err=42
 * truncations). Extracted unchanged from init_proposer.c so the prospective
 * Decide judge reuses the same request instead of a copy. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_chat_oneshot_opts {
    double temperature;
    uint32_t max_tokens;
    bool json_object; /* response_format = "json_object" */
} hu_chat_oneshot_opts_t;

/* Prefers the structured chat() vtable. Falls back to chat_with_system()
 * (no max_tokens / thinking control there) for providers and mocks without
 * it, and returns HU_ERR_NOT_SUPPORTED when neither exists. On HU_OK with a
 * non-empty answer *out is heap (alloc), freed with
 * alloc->free(ctx, *out, *out_len + 1); an empty answer is HU_OK with *out
 * NULL. `model` may be NULL (sent as ""). */
hu_error_t hu_provider_chat_oneshot(hu_allocator_t *alloc, hu_provider_t *provider,
                                    const char *model, size_t model_len, const char *system,
                                    size_t system_len, const char *user, size_t user_len,
                                    const hu_chat_oneshot_opts_t *opts, char **out,
                                    size_t *out_len);

#endif /* HU_PROVIDERS_CHAT_ONESHOT_H */
