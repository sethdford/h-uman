#ifndef HU_CONTEXT_H
#define HU_CONTEXT_H

#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

char *hu_context_build_system_prompt(hu_allocator_t *alloc, const char *base, size_t base_len,
                                     const char *workspace_dir, size_t workspace_dir_len);

/* Format history messages for provider request. Allocates messages array.
 * Message content/tool_call pointers are BORROWED from history — do NOT free them.
 * Caller must free only the returned array itself (not individual message contents).
 * include_mask: if non-NULL, length must be >= history_count; false skips that entry. */
hu_error_t hu_context_format_messages(hu_allocator_t *alloc, const hu_owned_message_t *history,
                                      size_t history_count, size_t max_messages,
                                      const bool *include_mask, hu_chat_message_t **out_messages,
                                      size_t *out_count);

uint32_t hu_context_estimate_tokens(const hu_chat_message_t *messages, size_t messages_count);

/* Estimate the serialized byte weight of one chat message INCLUDING multimodal
 * content parts (image/audio base64). Unlike a content_len-only sum, this
 * accounts for the multi-MB base64 payloads that dominate a multimodal request
 * body. Returns 0 for a NULL message. */
size_t hu_chat_message_estimate_bytes(const hu_chat_message_t *m);

/* Drop oversized multimodal content parts (base64/url payload > max_part_bytes)
 * from a TURN-LOCAL message array. Only NULLs each offending message's
 * content_parts pointer/count on the caller's copy; never frees the underlying
 * (history-aliased) buffers. Idempotent. Returns the number of messages
 * trimmed. */
size_t hu_chat_messages_drop_oversized_parts(hu_chat_message_t *msgs, size_t count,
                                             size_t max_part_bytes);

/* Estimate tokens for a single text string (rough: ~4 chars per token). */
size_t hu_estimate_tokens_text(const char *text, size_t len);

/* Context pressure tracking for agent loop */
typedef struct hu_context_pressure {
    size_t current_tokens;
    size_t max_tokens;
    float pressure;
    bool warning_85_emitted;
    bool warning_95_emitted;
} hu_context_pressure_t;

/* Update pressure from current_tokens and max_tokens. Emit warnings at configured
 * thresholds. Returns true if auto-compact should be triggered (>= pressure_compact). */
bool hu_context_check_pressure(hu_context_pressure_t *p, float pressure_warn,
                               float pressure_compact);

#endif /* HU_CONTEXT_H */
