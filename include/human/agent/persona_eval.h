/* PersonaEval v2 speaker-ID classifier — C port.
 *
 * In-process P(Seth) scorer. Loads the v2 model JSON written by
 * scripts/personaeval_speaker_id_v2.py, computes features, runs a
 * logistic regression. Same features + weights as Python, so the C
 * output matches the Python output exactly (to ~6 decimal places —
 * parity test in tests/test_persona_eval.c).
 *
 * Why in-process: the daemon needs P(Seth) per inference for:
 *   - Round 5 production_outcomes.p_seth_at_send (no more -1.0
 *     placeholder)
 *   - Round 6 meta-cognitive uncertainty routing (defer / best-of-N /
 *     ship single-shot depending on P(Seth))
 *   - L5 in production: argmax-P(Seth) over best-of-N candidates
 *
 * Subprocess-Python would add 100-500 ms per call. C is ~10 µs.
 *
 * Pure function — no I/O after model load. Thread-safe (no shared
 * mutable state). See docs/plans/2026-05-19-vision-better-than-human.md
 * Round 5.
 */
#ifndef HU_AGENT_PERSONA_EVAL_H
#define HU_AGENT_PERSONA_EVAL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque model handle. The interior holds feature names, means/stds
 * for standardization, weights, bias. Build via hu_persona_eval_load.
 * Free via hu_persona_eval_free. */
typedef struct hu_persona_eval_model hu_persona_eval_model_t;

/* Load a v2 model from a JSON file written by personaeval_speaker_id_v2.py.
 *
 * Path resolution:
 *   path != NULL  → use path
 *   path == NULL  → /tmp/seth_speaker_id.json (the canonical location
 *                   used by the rest of the pipeline)
 *
 * Returns HU_OK on success. *out is owned by the caller; free with
 * hu_persona_eval_free.
 *
 * Errors:
 *   HU_ERR_INVALID_ARGUMENT — out is NULL
 *   HU_ERR_IO               — file missing or unreadable
 *   HU_ERR_INVALID_INPUT    — JSON malformed or wrong version
 */
hu_error_t hu_persona_eval_load(hu_allocator_t *alloc, const char *path,
                                hu_persona_eval_model_t **out);

/* Free a model loaded with hu_persona_eval_load. */
void hu_persona_eval_free(hu_allocator_t *alloc, hu_persona_eval_model_t *model);

/* Score a response. Returns P(Seth) ∈ [0.0, 1.0]. */
double hu_persona_eval_score(const hu_persona_eval_model_t *model, const char *text,
                             size_t text_len);

/* True iff P(Seth) >= threshold. Convenience for the uncertainty
 * router; same as (score >= threshold) but pulls the threshold
 * comparison into one place. */
bool hu_persona_eval_is_seth(const hu_persona_eval_model_t *model, const char *text,
                             size_t text_len, double threshold);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_PERSONA_EVAL_H */
