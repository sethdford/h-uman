/* Response-guard repair policy — what to send after a guard REJECT + retry.
 *
 * 2026-09-30 a repair retry cut off mid-clause was sent in place of a
 * rejected answer. The retry was trusted blindly. The floor this module adds
 * is: never send a cut-off reply from the repair path.
 *
 *   - a complete retry is sent as before;
 *   - a fragment retry loses to the original only when the guard has nothing
 *     against the original (within the length cap, no other violation) —
 *     a length-only (G5 context-dump) reject is never sent, whole or sliced;
 *   - otherwise the fragment retry is cut back to its last complete sentence
 *     or its lone dangling word dropped;
 *   - otherwise nothing is sent (kept=none). Silence, not a canned line.
 *
 * Logs one aggregate line per repair: "[guard_repair] kept=<retry|original|
 * retry_trimmed|none> reason=<ok|retry_fragment|retry_failed> orig_len=N
 * retry_len=N sent_len=N" — lengths and enums only. */
#ifndef HU_AGENT_GUARD_REPAIR_H
#define HU_AGENT_GUARD_REPAIR_H

#include "human/agent/response_guard.h"
#include "human/core/allocator.h"
#include "human/observer.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HU_GUARD_REPAIR_KEPT_RETRY = 0,
    HU_GUARD_REPAIR_KEPT_ORIGINAL,      /* guard has nothing against it but the retry's cut */
    HU_GUARD_REPAIR_KEPT_RETRY_TRIMMED, /* retry with its cut-off tail removed */
    HU_GUARD_REPAIR_KEPT_NONE,          /* nothing sendable: the caller sends nothing */
} hu_guard_repair_kept_t;

typedef struct {
    hu_guard_repair_kept_t kept;
    /* Bytes of the chosen source (original for ORIGINAL, retry for
     * RETRY/RETRY_TRIMMED) to send, from its start. 0 for NONE. */
    size_t len;
    /* Static enum-like slug: "ok", "retry_fragment", "retry_failed". */
    const char *reason;
} hu_guard_repair_decision_t;

/* Pure policy. `length_cap` is hu_guard_length_cap() for the turn's guard
 * context (SIZE_MAX when no length check applies). */
hu_guard_repair_decision_t hu_guard_repair_decide(const char *original, size_t original_len,
                                                  const hu_guard_report_t *original_report,
                                                  const char *retry, size_t retry_len,
                                                  size_t length_cap);

/* Applies the policy at a guard-repair call site. On entry `*retry` is the
 * allocator-owned retry text (may be NULL/empty when the retry failed). On
 * return `*retry`/`*retry_len` hold the allocator-owned text to send — the
 * retry, a trimmed copy of it, or a copy of the original — or NULL/0 for NONE,
 * which means send nothing. An original is re-checked with
 * hu_response_guard_check_ex under `ctx` and kept only when the guard passes
 * it. `original_report`/`ctx` NULL (validator-chain path) never keeps the
 * original. Logs the aggregate [guard_repair] line. */
hu_guard_repair_kept_t hu_guard_repair_resolve(hu_allocator_t *alloc, hu_observer_t *obs,
                                               const char *original, size_t original_len,
                                               const hu_guard_report_t *original_report,
                                               const hu_guard_context_t *ctx, char **retry,
                                               size_t *retry_len);

/* Is the inbound message a question: a "?", or a clause with interrogative
 * structure — a fronted wh-word ("whats the plan") or subject-auxiliary
 * inversion ("did you end up renting that kayak"). "have fun tonight" is not.
 * An answer to one is judged against the contact's long-reply length, not the
 * conversation's recent average; see hu_guard_context_t.inbound_is_ask. */
bool hu_guard_inbound_is_ask(const char *msg, size_t len);

const char *hu_guard_repair_kept_name(hu_guard_repair_kept_t kept);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_GUARD_REPAIR_H */
