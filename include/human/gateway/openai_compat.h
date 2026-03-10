#ifndef HU_OPENAI_COMPAT_H
#define HU_OPENAI_COMPAT_H

#include "human/core/allocator.h"
#include "human/gateway/control_protocol.h"
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * OpenAI-compatible API handlers for gateway
 * ────────────────────────────────────────────────────────────────────────── */

/* Handle POST /v1/chat/completions. Allocates *out_body (caller frees).
 * Sets *out_status and *out_body, *out_body_len.
 * When stream=true: returns SSE body, set *out_content_type to "text/event-stream" if non-NULL.
 * When stream=false: returns JSON body, set *out_content_type to "application/json" if non-NULL. */
void hu_openai_compat_handle_chat_completions(const char *body, size_t body_len,
                                              hu_allocator_t *alloc,
                                              const hu_app_context_t *app_ctx, int *out_status,
                                              char **out_body, size_t *out_body_len,
                                              const char **out_content_type);

/* Handle GET /v1/models. Allocates *out_body (caller frees). */
void hu_openai_compat_handle_models(hu_allocator_t *alloc, const hu_app_context_t *app_ctx,
                                    int *out_status, char **out_body, size_t *out_body_len);

#endif /* HU_OPENAI_COMPAT_H */
