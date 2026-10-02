/* Response-guard repair policy — what to send after a guard REJECT + retry.
 *
 * Since 2026-09-17 every guard repair shrank the reply below half its
 * original length (median 767 -> 29 chars), and on 2026-09-30 a repair sent
 * "Wait, did we actually lock" — a sentence cut mid-clause — in place of a
 * 94-char answer. The repair retry was trusted blindly. This module decides
 * between the original and the retry instead:
 *
 *   - the retry is kept when it is a complete reply and is not a collapse
 *     (< 40% of the original) of an original whose only violation was length;
 *   - otherwise, when the original's only violation was length, the original
 *     is kept — cut to a sentence end under the guard's length cap when it is
 *     over the cap, so the sent text is one the guard itself would accept;
 *   - otherwise (the original leaked something: director/persona echo, CoT,
 *     repetition loop ...) it is never sent; a fragment retry is cut back to
 *     its last complete sentence or dangling-word-free form, else nothing is
 *     kept and the caller installs its canned fallback.
 *
 * Never sends a fragment from the repair path. Logs one aggregate line per
 * repair: "[guard_repair] kept=<retry|original|trimmed|retry_trimmed|none>
 * reason=<...> orig_len=N retry_len=N sent_len=N" — lengths and enums only. */
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
    HU_GUARD_REPAIR_KEPT_ORIGINAL,
    HU_GUARD_REPAIR_KEPT_TRIMMED,       /* original cut to a sentence end under the cap */
    HU_GUARD_REPAIR_KEPT_RETRY_TRIMMED, /* retry with its cut-off tail removed */
    HU_GUARD_REPAIR_KEPT_NONE,          /* nothing sendable: caller installs its fallback */
} hu_guard_repair_kept_t;

typedef struct {
    hu_guard_repair_kept_t kept;
    /* Bytes of the chosen source (original for ORIGINAL/TRIMMED, retry for
     * RETRY/RETRY_TRIMMED) to send, from its start. 0 for NONE. */
    size_t len;
    /* Static enum-like slug: "ok", "retry_fragment", "retry_collapsed". */
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
 * retry, or a fresh copy of (a prefix of) the original — or NULL/0 for NONE.
 * Text taken from the original is re-checked with hu_response_guard_check_ex
 * under `ctx` and only kept when the guard passes it. Logs the aggregate
 * [guard_repair] line. On OOM leaves the retry untouched. */
hu_guard_repair_kept_t hu_guard_repair_resolve(hu_allocator_t *alloc, hu_observer_t *obs,
                                               const char *original, size_t original_len,
                                               const hu_guard_report_t *original_report,
                                               const hu_guard_context_t *ctx, char **retry,
                                               size_t *retry_len);

/* Is the inbound message a question or a request ("walk me through it",
 * "can you", "whats the plan")? Such a message licenses a longer answer than
 * the conversation's recent average; see hu_guard_context_t.inbound_is_ask. */
bool hu_guard_inbound_is_ask(const char *msg, size_t len);

const char *hu_guard_repair_kept_name(hu_guard_repair_kept_t kept);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_GUARD_REPAIR_H */
