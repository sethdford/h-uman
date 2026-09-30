#ifndef HU_MEMORY_PROSPECTIVE_V2_H
#define HU_MEMORY_PROSPECTIVE_V2_H
/*
 * Prospective memory v2 — one pass of Filter -> Decide over the typed store
 * (spec docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4).
 *
 * The judge is injected: a scripted judge in tests and in the probe, the
 * agent's provider (thinking off) in the daemon. `apply=false` is SHADOW:
 * it reads and judges but writes nothing and renders nothing.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/prospective_policy.h"
#include "human/memory/prospective_repo.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE

/* One short answer for (system, user). On HU_OK, *out is heap from `alloc`
 * (freed by the caller with *out_len + 1) or NULL for an empty answer. */
typedef hu_error_t (*hu_prospective_judge_fn)(void *ctx, hu_allocator_t *alloc, const char *system,
                                              size_t system_len, const char *user, size_t user_len,
                                              char **out, size_t *out_len);

typedef struct hu_prospective_judge {
    hu_prospective_judge_fn fn;
    void *ctx;
} hu_prospective_judge_t;

typedef struct hu_prospective_turn {
    const char *contact; /* the contact key rows are stored under */
    size_t contact_len;
    const char *inbound; /* the text keyword cues are matched in; NULL for time */
    size_t inbound_len;
    const char *history; /* "them: …\nme: …\n", oldest first, <= 20 lines */
    size_t history_len;
    bool is_group;
    bool is_self;
    int64_t now;
    int64_t day_start; /* local midnight: the per-day cap's day */
} hu_prospective_turn_t;

typedef struct hu_prospective_item_verdict {
    int64_t id;
    hu_prospective_verdict_t verdict;
    bool judge_ok; /* false: the judge call itself failed */
} hu_prospective_item_verdict_t;

typedef struct hu_prospective_counts {
    size_t candidates; /* eligible and judged (<= HU_PROSPECTIVE_JUDGE_CAP) */
    size_t fire, resolved, cancel, not_now, parse_fail, judge_err;
    size_t expired; /* past their window this pass */
    size_t capped;  /* time cues over the per-day cap */
    hu_prospective_item_verdict_t items[HU_PROSPECTIVE_JUDGE_CAP];
    size_t item_count;
    char fire_actions[HU_PROSPECTIVE_RENDER_CAP][256]; /* would-fire actions (SHADOW uptake) */
    size_t fire_action_count;
} hu_prospective_counts_t;

typedef struct hu_prospective_delivery_counts {
    size_t surfaced, used, ignored, expired;
} hu_prospective_delivery_counts_t;

/* One pass for `turn` over the contact's pending `kind` intentions (KEYWORD
 * or TIME):
 *   1. apply: settle intentions still `surfaced` from an earlier pass that
 *      no delivered reply confirmed (an attempt that did not land);
 *   2. Filter each pending intention (hu_prospective_filter) — EXPIRE retires
 *      it, SKIP/CAPPED leave it;
 *   3. Decide up to HU_PROSPECTIVE_JUDGE_CAP eligible intentions through
 *      `judge` (hu_prospective_decide) — RESOLVED -> done, CANCEL ->
 *      canceled (outcome 'suppressed'), FIRE -> surfaced if rendered;
 *   4. apply: render the fired ones (SOFT for keyword, DUE_LIST for time)
 *      into *directive (heap, free with *directive_len + 1; NULL if none).
 * A settled time intention also retires its ledger twins. Every decision is
 * counted in *counts. `apply=false` performs 2–3 read-only. */
hu_error_t hu_prospective_v2_run(hu_allocator_t *alloc, sqlite3 *db, hu_prospective_cue_kind_t kind,
                                 const hu_prospective_turn_t *turn,
                                 const hu_prospective_judge_t *judge, bool apply,
                                 hu_prospective_counts_t *counts, char **directive,
                                 size_t *directive_len);

/* Done only after evidence (§4.3): each intention of `kind` that is
 * `surfaced` for `contact` becomes DONE (outcome 'used') when `reply`
 * carries its key terms; otherwise attempts+1 and back to PENDING, or
 * EXPIRED at HU_PROSPECTIVE_MAX_ATTEMPTS (outcome 'ignored'). `reply` NULL
 * means nothing was delivered. `out` may be NULL. */
hu_error_t hu_prospective_v2_after_delivery(hu_allocator_t *alloc, sqlite3 *db,
                                            hu_prospective_cue_kind_t kind, const char *contact,
                                            size_t contact_len, const char *reply, size_t reply_len,
                                            int64_t now, hu_prospective_delivery_counts_t *out);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_MEMORY_PROSPECTIVE_V2_H */
