#ifndef HU_PROVIDERS_LOCAL_ONLY_H
#define HU_PROVIDERS_LOCAL_ONLY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

/* Local-only (private) prompt spans — the one place that strips them.
 *
 * Owner rule: real message text never reaches a cloud model without explicit
 * opt-in. Prompt sections built from that text may be shown to the on-device
 * model but are removed from any request that leaves the machine. The
 * reliable provider applies this to every attempt — primary or extra, first
 * try or fallback — whose provider is not local OR whose model name is a
 * cloud model's. The second half covers routes that switch model BY NAME on
 * the same provider (agent_turn.c: the analytical tier -> gemini-3.1-pro-
 * preview, S3 -> fallback_model, on-device failure -> the reflexive cloud
 * model, and the degradation retry).
 *
 * A span opens at a line starting with a registered heading and closes after
 * its end line, or — for a kind with no end line — at the first blank line.
 * A span with no close (the positional prompt cap cut it) fails closed: the
 * rest of that message is removed. To protect a new section, give it a unique
 * heading and add one row to k_span_kinds in src/providers/local_only.c. */

/* HU_THREAD_CONTEXT's "## Recent thread" block (daemon/thread_context.h). */
#define HU_LOCAL_ONLY_THREAD_BEGIN "## Recent thread"
#define HU_LOCAL_ONLY_THREAD_END   "## End of recent thread\n"

typedef struct hu_local_only_span_kind {
    const char *begin; /* line prefix that opens the span */
    const char *end;   /* full line that closes it (inclusive); NULL = first blank line */
} hu_local_only_span_kind_t;

/* The registered kinds (read-only). */
const hu_local_only_span_kind_t *hu_local_only_span_kinds(size_t *count);

/* True for provider names that serve from this machine. NULL / unknown names
 * are NOT local (fail closed). */
bool hu_local_only_provider_name_is_local(const char *name);

/* True when `prov` reports a local name via get_name. */
bool hu_local_only_provider_is_local(const hu_provider_t *prov);

/* True for model names that only a cloud API serves (gemini-*, gpt-*,
 * claude-*, ...): sending one to a local provider still marks the attempt
 * non-local. */
bool hu_local_only_model_is_cloud(const char *model, size_t model_len);

/* The attempt decision: local provider AND not a cloud model name. */
bool hu_local_only_attempt_is_local(bool provider_local, const char *model, size_t model_len);

/* True when `s` holds at least one span. */
bool hu_local_only_has_span(const char *s, size_t len);

/* Copy of `s` with every span removed. `*out` is NULL (and `*out_len` 0)
 * when `s` holds no span — the caller keeps the original. Returns
 * HU_ERR_OUT_OF_MEMORY when a span exists but the copy failed. */
hu_error_t hu_local_only_strip(hu_allocator_t *alloc, const char *s, size_t len, char **out,
                               size_t *out_len);

/* Turn-local, stripped shallow copy of a chat request. */
typedef struct hu_local_only_request {
    hu_chat_request_t req;
    hu_chat_message_t *msgs; /* NULL when nothing was stripped */
    char **owned;            /* per-message stripped content, NULL entries untouched */
    size_t *owned_lens;
    size_t count;
    size_t stripped; /* messages that lost a span */
} hu_local_only_request_t;

/* Point `*use` at a request safe for a non-local attempt: `in` itself when it
 * carries no span, else `scratch->req` (spans removed, prompt_cache_id
 * cleared so a cloud cache keyed on the full prompt is never reused). Always
 * pair with hu_local_only_request_release. On error `*use` is NULL and the
 * caller must not send. */
hu_error_t hu_local_only_request_prepare(hu_allocator_t *alloc, const hu_chat_request_t *in,
                                         hu_local_only_request_t *scratch,
                                         const hu_chat_request_t **use);

void hu_local_only_request_release(hu_allocator_t *alloc, hu_local_only_request_t *scratch);

#endif /* HU_PROVIDERS_LOCAL_ONLY_H */
