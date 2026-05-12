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
 * when HU_ENABLE_CURL is defined (gemini, then openai).
 *
 * `persona_hint` (optional, may be NULL) is spliced into the repair system
 * prompt as the persona/style anchor (typically built via
 * `hu_persona_build_retry_hint` so it includes the active channel overlay's
 * formality / avg_length / emoji / style_notes). Without a hint the retry
 * collapses onto the model's baseline polite-assistant register — this is
 * the failure mode documented in the 2026-05-12 Jordan-iMessage incident.
 * Capped at HU_PERSONA_RETRY_HINT_MAX bytes by the implementation. */

hu_error_t hu_response_guard_retry_slim(hu_allocator_t *alloc, hu_observer_t *obs,
                                         const struct hu_config *cfg, hu_provider_t *primary,
                                         const char *model, size_t model_len, const char *user_msg,
                                         size_t user_msg_len, const char *persona_hint,
                                         size_t persona_hint_len, char **out, size_t *out_len,
                                         hu_guard_report_t *guard_report);

#endif /* HU_AGENT_RESPONSE_GUARD_RETRY_H */
