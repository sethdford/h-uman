#ifndef HU_ML_LEARNER_BRIDGE_H
#define HU_ML_LEARNER_BRIDGE_H

/* W13 — Learner signal bridge.
 *
 * The learner (`hu_learner_t`) ships with a deterministic backend and a
 * vtable for `train()`, but until this bridge existed the agent had no way
 * to *get* signals into it. The bridge is the thin glue between two real
 * signal sources we already collect for other purposes:
 *
 *   1. Persona delta proposals from `src/persona/delta_observer.c` —
 *      every "be more concise", "stop saying sorry", etc. detected in an
 *      inbound user message becomes a `hu_persona_delta_t` that gets
 *      stored in the graph. Each one is also a labeled style-adaptation
 *      signal for the personalisation loop.
 *
 *   2. Outcome tracker entries from `src/agent/outcomes.c` — tool
 *      successes, tool failures, user corrections, and explicit positive
 *      reactions. Each one is a labeled case-outcome signal.
 *
 * The bridge converts these into `hu_training_signal_t` values and queues
 * them on the learner's pending buffer. The W14 sleep-time scheduler
 * later drains the buffer and invokes `hu_learner_train()`. We do NOT
 * train inline — training is expensive and must happen during scheduled
 * sleep windows.
 *
 * Idempotency contract: each emitter advances a watermark on the learner
 * (delta id or outcome timestamp). Replaying the same input — e.g. the
 * outcome tracker's whole circular buffer scanned once per minute —
 * produces signals exactly once.
 *
 * Determinism contract: same input + same watermark state ⇒ identical
 * pending buffer contents. The bridge does no I/O, no clock reads, no
 * randomness — the test suite verifies replays are no-ops. */

#include "human/agent/outcomes.h"
#include "human/core/error.h"
#include "human/ml/learner.h"
#include "human/persona/persona_deltas.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert persona-delta proposals into pending learner signals.
 *
 * Each delta with `id > learner->pending_persona_delta_id_high` becomes
 * one `HU_TRAIN_PERSONA_DELTA` signal carrying the delta verbatim. The
 * watermark advances to the max id seen on this call. Deltas with id <=
 * the watermark (already consumed) are silently skipped.
 *
 * Returns:
 *   HU_OK              — success (or no-op when n == 0 or learner is NULL)
 *   HU_ERR_OUT_OF_MEMORY — pending buffer alloc failed (rare)
 *
 * NULL learner is treated as a silent no-op so callers (e.g. delta_observer)
 * can wire this in unconditionally without branching.
 *
 * Naming note: the existing `hu_learner_signals_from_persona_deltas` in
 * learner.h reads APPLIED deltas from a graph and returns a signal array
 * for batch ingestion. This bridge function is the in-memory, push-style
 * sibling — different inputs, different ownership model, hence the
 * `bridge_emit_*` prefix. */
hu_error_t hu_learner_bridge_emit_persona_deltas(hu_learner_t *learner,
                                                 const hu_persona_delta_t *deltas, size_t n);

/* Drain recent outcomes into pending learner signals.
 *
 * Each outcome entry with `timestamp_ms > learner->pending_outcome_ts_high`
 * is mapped to a training signal:
 *   HU_OUTCOME_TOOL_SUCCESS   → HU_TRAIN_CASE_OUTCOME, reward 1.0
 *   HU_OUTCOME_TOOL_FAILURE   → HU_TRAIN_CASE_OUTCOME, reward 0.0
 *   HU_OUTCOME_USER_POSITIVE  → HU_TRAIN_CASE_OUTCOME, reward 1.0
 *   HU_OUTCOME_USER_CORRECTION→ HU_TRAIN_CASE_OUTCOME, reward 0.0
 * The case_id is synthesised from the outcome's timestamp so the learner
 * can deduplicate on its end if it needs to.
 *
 * The watermark advances to the max timestamp seen, even when the
 * pending buffer was full and entries were dropped — this is intentional
 * so a long-offline scheduler can't get stuck replaying the same wrap.
 *
 * Returns:
 *   HU_OK              — success (or no-op when tracker is NULL)
 *   HU_ERR_OUT_OF_MEMORY — pending buffer alloc failed
 *
 * NULL learner OR NULL tracker is treated as a silent no-op. */
hu_error_t hu_learner_bridge_emit_outcomes(hu_learner_t *learner, hu_outcome_tracker_t *tracker);

/* Drain the learner's pending signal buffer into a caller-owned array.
 *
 * Transfers ownership: on success `*out` points to a freshly-allocated
 * array of `*out_count` signals (free with `hu_learner_signals_free`),
 * and the learner's pending buffer is reset to empty. The watermark
 * fields are NOT reset — they continue to provide replay-idempotency on
 * subsequent emit calls.
 *
 * Returns HU_OK with *out = NULL and *out_count = 0 when the pending
 * buffer is already empty. */
hu_error_t hu_learner_pending_drain(hu_learner_t *learner, hu_training_signal_t **out,
                                    size_t *out_count);

/* Read-only inspection — current pending count without draining. Useful
 * for tests and observability. Returns 0 if learner is NULL. */
size_t hu_learner_pending_count(const hu_learner_t *learner);

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_LEARNER_BRIDGE_H */
