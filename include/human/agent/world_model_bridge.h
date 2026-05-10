#ifndef HU_AGENT_WORLD_MODEL_BRIDGE_H
#define HU_AGENT_WORLD_MODEL_BRIDGE_H

/* W9 wire bridge (FIX 12).
 *
 * The W7 memory facade and the legacy `hu_memory_t` from `human/memory.h`
 * use the same struct tag (`struct hu_memory`), so any translation unit that
 * already pulls in legacy `human/memory.h` (e.g. anything that includes
 * `human/agent.h`) cannot also include `human/memory/memory.h` or
 * `human/agent/world_model.h` -- the C compiler sees a redefinition.
 *
 * This bridge gives `agent_turn.c` and `daemon.c` a way to use the W7 facade
 * + `hu_world_model_load` without paying that include cost. The bridge owns
 * its own translation unit (`world_model_bridge.c`) where ONLY the W7
 * headers are pulled in; everyone else talks to the bridge through the
 * unique opaque tag `struct hu_w7_facade`.
 *
 * Same pattern as `agent->verifier_graph` (FIX 2): isolate the type
 * collision behind a fresh forward declaration. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/graph.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque W7 facade handle. Define lives in world_model_bridge.c. */
struct hu_w7_facade;
typedef struct hu_w7_facade hu_w7_facade_t;

/* Open a W7 facade backed by `graph` (the v1 backends). Caller owns the
 * returned pointer and must close with hu_w7_facade_close. */
hu_error_t hu_w7_facade_open(hu_graph_t *graph, hu_allocator_t *alloc, hu_w7_facade_t **out);

void hu_w7_facade_close(hu_w7_facade_t *facade, hu_allocator_t *alloc);

/* Render the cached world model for `contact_id` into a prompt-ready text
 * block. Returns HU_OK with `*out_text == NULL`, `*out_len == 0` when there
 * is no information worth surfacing -- callers should treat that as "no
 * world model context available" and skip injection.
 *
 * The text format mirrors the persona/personal_model sections in the system
 * prompt (FIX 1): a labeled markdown block with subsections for goals,
 * negatives, theory-of-mind, and recent topics. Caller owns the returned
 * pointer and must free with `alloc->free`.
 *
 * `now_ms == 0` means "use OS clock". */
hu_error_t hu_w7_render_world_model(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                    const char *contact_id, size_t contact_id_len,
                                    int64_t now_ms, char **out_text, size_t *out_len);

/* W11 self-RAG outcome enum, mirrored at the bridge layer so callers don't
 * have to include `human/agent/self_rag.h` (which pulls in W7). */
typedef enum hu_w11_outcome {
    HU_W11_OUTCOME_SUPPORTED = 0,
    HU_W11_OUTCOME_HEDGED = 1,
    HU_W11_OUTCOME_REWRITTEN = 2,
    HU_W11_OUTCOME_ABSTAINED = 3,
} hu_w11_outcome_t;

/* W11 self-RAG verification (FIX 12b). Atomic-claim decomposition + scoring
 * of `draft` against the W7 facade and the W9 world model for `contact_id`.
 *
 * `mode` mirrors the response_verifier modes:
 *   0 = OFF (returns SUPPORTED, no work)
 *   1 = TELEMETRY (extract claims, do NOT modify draft)
 *   2 = SOFT (prepend hedges to flagged claims)
 *   3 = STRICT (rewrite via corrective-RAG)
 *
 * Returns HU_OK on success. Outputs:
 *   `*out_outcome` -- which branch fired
 *   `*out_claims_total` -- claims extracted (for telemetry parity with W4)
 *   `*out_claims_flagged` -- claims that scored below the abstain threshold
 *   `*out_modified` -- when non-NULL and the backend rewrote the draft, the
 *       new buffer is allocated via `alloc` and ownership transfers to caller.
 *       Pass NULL to skip modification entirely.
 *   `*out_modified_len` -- length of the new buffer when allocated.
 *
 * `now_ms == 0` means "use OS clock". */
hu_error_t hu_w11_self_rag_verify(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                  const char *contact_id, size_t contact_id_len,
                                  const char *draft, size_t draft_len, int mode, int64_t now_ms,
                                  hu_w11_outcome_t *out_outcome, size_t *out_claims_total,
                                  size_t *out_claims_flagged, char **out_modified,
                                  size_t *out_modified_len);

/* ── W14 sleep-time compute scheduler bridge (FIX 13) ─────────────────────
 *
 * The W14 scheduler is the same shape problem: `human/agent/scheduler.h`
 * pulls in `human/memory/memory.h`, which redefines `struct hu_memory`
 * for any TU that already has the legacy `human/memory.h` (i.e. anything
 * including `human/agent.h`). This bridge gives the daemon a way to
 * open / tick / close the scheduler without paying that include cost. */

struct hu_w14_scheduler;
typedef struct hu_w14_scheduler hu_w14_scheduler_t;

/* Open a scheduler over the same `hu_memory_t` the W7 facade owns. The
 * scheduler does not take ownership of the facade — both must outlive
 * the lifetime of the daemon main loop. Returns HU_OK and `*out_sched`
 * is non-NULL on success; on failure `*out_sched` is NULL. */
hu_error_t hu_w14_scheduler_open(hu_w7_facade_t *facade, hu_allocator_t *alloc,
                                 hu_w14_scheduler_t **out_sched);

/* Close the scheduler. Safe with NULL. Must be called BEFORE
 * hu_w7_facade_close because the scheduler borrows the facade's memory
 * handle. */
void hu_w14_scheduler_close(hu_w14_scheduler_t *s, hu_allocator_t *alloc);

/* Run one tick of the scheduler. `now_ms` is unix-ms; pass 0 to use the
 * OS clock. The daemon main loop should call this once per minute (or
 * more often — the scheduler's own per-tick budget bounds the work). */
hu_error_t hu_w14_scheduler_tick(hu_w14_scheduler_t *s, int64_t now_ms);

/* Enqueue a counterfactual-rehearsal job for `contact_id`. The runner
 * is registered automatically at open(); this is just an enqueue
 * helper that hides the hu_job_spec_t shape from the daemon. */
hu_error_t hu_w14_scheduler_enqueue_counterfactual(hu_w14_scheduler_t *s,
                                                   const char *contact_id,
                                                   size_t contact_id_len,
                                                   int budget_ms);

/* Status snapshot for `human ml status` and friends. Always populates
 * the out fields even on partial probe failure. NULL output pointers
 * are tolerated (any subset may be queried). */
hu_error_t hu_w14_scheduler_status(hu_w14_scheduler_t *s, size_t *out_jobs_pending,
                                   size_t *out_jobs_completed_today, int *out_battery_pct,
                                   int *out_on_ac_power);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_WORLD_MODEL_BRIDGE_H */
