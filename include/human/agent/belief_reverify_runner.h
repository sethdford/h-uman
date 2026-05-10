#ifndef HU_AGENT_BELIEF_REVERIFY_RUNNER_H
#define HU_AGENT_BELIEF_REVERIFY_RUNNER_H

/* W14 — belief re-verification scheduler runner.
 *
 * Periodic background sweep: pick up aging `relations` rows
 * (last_seen older than `max_age_ms`) and write back a refreshed
 * `confidence`. Today the recompute is a flat 5% multiplicative decay
 * (so old facts fade unless re-observed). Future revisions will plumb
 * a frontier-grounded verifier that can both decay AND boost based on
 * grounding evidence.
 *
 * `user_data` is a `hu_belief_reverify_ctx_t *`. NULL is allowed and
 * uses defaults: 30-day age, 64 rows/tick, all contacts. */

#include "human/agent/scheduler.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_provider hu_provider_t;

typedef struct hu_belief_reverify_ctx {
    hu_allocator_t *alloc;          /* optional; system allocator if NULL */
    const char *contact_id;         /* optional contact filter; NULL = all */
    int64_t max_age_ms;             /* relations older than this are eligible (default 30d) */
    size_t max_relations_per_tick;  /* hard cap per tick (default 64) */
    hu_provider_t *provider;        /* optional; enables LLM-judge semantic conflict detection */
    size_t *out_reverified;         /* optional — populated with rows touched */
    size_t *out_decayed;            /* optional — subset of `reverified` that lost confidence */
} hu_belief_reverify_ctx_t;

hu_error_t hu_belief_reverify_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec, int64_t budget_ms,
                                     void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_BELIEF_REVERIFY_RUNNER_H */
