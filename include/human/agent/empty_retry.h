#ifndef HU_AGENT_EMPTY_RETRY_H
#define HU_AGENT_EMPTY_RETRY_H

/* One retry for a reply the model wrote and the serving layer then threw away.
 *
 * The local MLX server returns HTTP 200 with empty content when a post-generation
 * guard rejects the draft (echo of the system prompt, pure scaffolding, pure
 * deliberation). The daemon sees HU_OK + no text and sends the contact nothing.
 * The tell that separates this from "the model produced nothing" is usage:
 * completion_tokens > 0 with an empty body.
 *
 * Gate: HU_EMPTY_REPLY_RETRY = off | shadow | live (default off). Shadow makes
 * the retry call and logs whether it came back non-empty, but keeps the empty
 * reply; live uses the retry's text. The retry goes to the same provider and
 * model as the first attempt, so it never adds a cloud hop. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/provider.h"
#include <stdbool.h>

/* Appended as a final system message on the retry. */
#define HU_EMPTY_RETRY_NUDGE                                                                 \
    "Your previous draft was discarded because it repeated text from these instructions or " \
    "the conversation. Write a fresh reply to the latest message, in your own words."

hu_gate_mode_t hu_empty_retry_mode(void);

/* True when `resp` is the generated-then-stripped shape and the gate is not off.
 * At most one retry per model call: already_retried short-circuits. */
bool hu_empty_retry_applies(hu_gate_mode_t mode, const hu_chat_response_t *resp,
                            bool already_retried);

/* Re-issue `req` once with HU_EMPTY_RETRY_NUDGE appended as a system message.
 * `req` is not modified. On HU_OK, *out is the retry's response (which may itself
 * be empty) and the caller frees it with hu_chat_response_free. */
hu_error_t hu_empty_retry_chat(hu_provider_t *provider, hu_allocator_t *alloc,
                               const hu_chat_request_t *req, const char *model, size_t model_len,
                               double temperature, hu_chat_response_t *out);

#endif /* HU_AGENT_EMPTY_RETRY_H */
