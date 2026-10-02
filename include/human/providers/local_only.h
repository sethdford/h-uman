#ifndef HU_PROVIDERS_LOCAL_ONLY_H
#define HU_PROVIDERS_LOCAL_ONLY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

/* Local-only prompt spans.
 *
 * Owner rule: real message text never reaches a cloud model without explicit
 * opt-in. Some prompt sections (the HU_THREAD_CONTEXT "## Recent thread"
 * block: verbatim chat.db lines) may be shown to the on-device model but must
 * be removed before any attempt on a cloud provider — including the reliable
 * provider's cloud extras, which a failing or circuit-open local primary hands
 * the SAME request to (prod: mlx_local primary, gemini extra).
 *
 * A span starts at a line beginning with HU_LOCAL_ONLY_BEGIN and ends after
 * the HU_LOCAL_ONLY_END line. A BEGIN with no END after it (the positional
 * prompt cap cut the block's tail) fails closed: everything from BEGIN to the
 * end of that message is removed. */
#define HU_LOCAL_ONLY_BEGIN "## Recent thread"
#define HU_LOCAL_ONLY_END   "## End of recent thread\n"

/* True for provider names that serve from this machine (doctor.c's list plus
 * the on-device backends). NULL / unknown names are NOT local (fail closed). */
bool hu_local_only_provider_name_is_local(const char *name);

/* True when `prov` reports a local name via get_name. */
bool hu_local_only_provider_is_local(const hu_provider_t *prov);

/* Copy of `s` with every local-only span removed. `*out` is NULL (and
 * `*out_len` 0) when `s` holds no span — the caller keeps the original.
 * Returns HU_ERR_OUT_OF_MEMORY when a span exists but the copy failed. */
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

/* Point `*use` at a request safe for a non-local provider: `in` itself when
 * it carries no span, else `scratch->req` (spans removed, prompt_cache_id
 * cleared so a cloud cache keyed on the full prompt is never reused). Always
 * pair with hu_local_only_request_release. On error `*use` is NULL and the
 * caller must not send. */
hu_error_t hu_local_only_request_prepare(hu_allocator_t *alloc, const hu_chat_request_t *in,
                                         hu_local_only_request_t *scratch,
                                         const hu_chat_request_t **use);

void hu_local_only_request_release(hu_allocator_t *alloc, hu_local_only_request_t *scratch);

#endif /* HU_PROVIDERS_LOCAL_ONLY_H */
