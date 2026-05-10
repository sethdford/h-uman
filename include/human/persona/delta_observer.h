#ifndef HU_PERSONA_DELTA_OBSERVER_H
#define HU_PERSONA_DELTA_OBSERVER_H

/* W5 producer: scans an inbound user message for explicit corrections of
 * agent behavior and turns each match into a hu_persona_delta proposal.
 *
 * The evolver consumer (hu_persona_evolver_run) is wired into the daemon's
 * 3 AM housekeeping (FIX 3), but it had no producer outside tests. Without
 * this observer the daily run always sees an empty proposal table.
 *
 * The pattern set is deliberately small and explicit; we only match phrases
 * that are unambiguous user requests (e.g. "be more casual", "stop saying
 * sorry"). Inferred or ambient deltas are out of scope here -- they belong
 * in a separate observation pipeline that fans through the verifier and
 * AutoDream output. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

struct hu_graph;

/* Forward decl — keeps this header light for callers that don't link the
 * learner. The W13 sibling below dereferences it through learner_bridge.h. */
struct hu_learner;

/* Scan `msg` for explicit-correction patterns. For each match, propose a
 * delta on `graph` for `contact_id` with the configured `channel` as the
 * delta key. Returns HU_OK even when nothing is matched; *out_proposed
 * receives the count.
 *
 * `graph` may be NULL -- in that case the function returns HU_OK with
 * *out_proposed = 0 (lets callers wire the observer unconditionally without
 * branching on the daemon's graph handle).
 *
 * `now_ms` is the proposed_at_ms passed through to hu_persona_delta_propose;
 * pass 0 to mean "now". */
hu_error_t hu_persona_observe_user_correction(struct hu_graph *graph, const char *contact_id,
                                              size_t contact_id_len, const char *channel,
                                              size_t channel_len, const char *msg,
                                              size_t msg_len, int64_t now_ms,
                                              size_t *out_proposed);

/* W13 wire-up: same as hu_persona_observe_user_correction, but ALSO emits
 * each detected correction as a pending learner signal via the bridge in
 * src/ml/learner_bridge.c. Callers that hold an agent should use this
 * variant when `agent->learner != NULL`. The original function above is
 * preserved for callers that don't yet own a learner reference, and is
 * equivalent to passing learner=NULL here.
 *
 * `learner` may be NULL (silent no-op for the signal path). */
hu_error_t hu_persona_observe_user_correction_with_learner(
    struct hu_graph *graph, struct hu_learner *learner, const char *contact_id, size_t contact_id_len,
    const char *channel, size_t channel_len, const char *msg, size_t msg_len, int64_t now_ms,
    size_t *out_proposed);

#endif /* HU_PERSONA_DELTA_OBSERVER_H */
