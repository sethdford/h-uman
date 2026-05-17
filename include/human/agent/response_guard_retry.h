#ifndef HU_AGENT_RESPONSE_GUARD_RETRY_H
#define HU_AGENT_RESPONSE_GUARD_RETRY_H

#include "human/agent/response_guard.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"

#include <stddef.h>

#include "human/observer.h"

struct hu_config;

/* After `hu_response_guard_check` returns HU_GUARD_REJECT on streamed or
 * batched model output, call this instead of replaying the full 100+ message
 * context through `chat()`. Builds a minimal 2-message request (repair
 * system + latest user text only) so local MLX/OpenAI-compatible servers
 * do not choke on ~20KB bodies (HTTP 52 empty reply). If the primary
 * provider still fails or the guarded output is rejected again, and
 * `cfg` is non-NULL, tries a short cloud completion via `hu_provider_create_from_config`
 * when HU_ENABLE_CURL is defined (gemini, then openai). */

hu_error_t hu_response_guard_retry_slim(hu_allocator_t *alloc, hu_observer_t *obs,
                                        const struct hu_config *cfg, hu_provider_t *primary,
                                        const char *model, size_t model_len, const char *user_msg,
                                        size_t user_msg_len, char **out, size_t *out_len,
                                        hu_guard_report_t *guard_report);

/* 2026-05-17 identity-anchored variant. Prepends `identity_anchor` (typically
 * the persona's core_anchor, e.g. "I am Seth. Not an AI...") to the slim repair
 * system prompt so the retry path does NOT lose the persona's identity context
 * the way the bare repair instruction would. When `identity_anchor` is NULL or
 * `identity_anchor_len` is 0, behaves exactly like `hu_response_guard_retry_slim`. */
hu_error_t hu_response_guard_retry_slim_with_identity(
    hu_allocator_t *alloc, hu_observer_t *obs, const struct hu_config *cfg, hu_provider_t *primary,
    const char *model, size_t model_len, const char *user_msg, size_t user_msg_len,
    const char *identity_anchor, size_t identity_anchor_len, char **out, size_t *out_len,
    hu_guard_report_t *guard_report);

#endif /* HU_AGENT_RESPONSE_GUARD_RETRY_H */
