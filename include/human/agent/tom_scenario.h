#ifndef HU_AGENT_TOM_SCENARIO_H
#define HU_AGENT_TOM_SCENARIO_H

/* B8 — Text-only theory-of-mind scenario synthesis for benchmarks and smoke
 * packs (no memory facade). Fills `hu_theory_of_mind_t` from premise +
 * question + category strings. Separate from `hu_world_model_build` ToM
 * (which synthesizes from graph + negatives); this path is for eval JSON.
 */

#include "human/agent/world_model.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deterministic stub: copies truncated premise/question and appends a
 * category-tagged planner hint to `user_expects_we_cannot`. */
void hu_tom_scenario_synthesize(const char *premise, size_t premise_len, const char *question,
                                size_t question_len, const char *category, size_t category_len,
                                int64_t now_ms, hu_theory_of_mind_t *out);

/* Merge a synthesized B8 ToM scenario INTO an existing `hu_world_model_t`'s
 * ToM block. Used by `hu_w7_render_world_model` so eval JSON paths can
 * inject scenario-derived ToM on top of whatever the W7 facade produced
 * for the contact. Empty / NULL inputs are silently ignored so callers can
 * hand through optional eval flags without branching. */
void hu_world_model_merge_tom_scenario(hu_world_model_t *wm, const char *premise,
                                       size_t premise_len, const char *question,
                                       size_t question_len, const char *category,
                                       size_t category_len, int64_t now_ms);

/* Run the committed `eval_suites/tom/tom_synthetic.json` smoke checks: each
 * item must parse and produce the expected category tag in ToM output.
 * Returns HU_OK with `*pass_out` / `*total_out` set (total may be 0 if no
 * items). */
hu_error_t hu_tom_b8_synthetic_pack_run_smoke(hu_allocator_t *alloc, const char *json_path,
                                              unsigned *pass_out, unsigned *total_out);

/* Case-insensitive bounded substring check used by the gold-scoring path.
 * If `gold_answer` contains `_`-separated segments of length >=
 * `min_token_len`, every such segment must appear in `response`; otherwise
 * the full `gold_answer` must appear as a substring. Empty / NULL inputs
 * return false. */
bool hu_tom_scenario_gold_matches_response(const char *gold_answer, const char *response,
                                           size_t response_len, size_t min_token_len);

/* Self-test path: walk `tom_synthetic.json` items and count how many pass
 * the rubric against premise + question + the synthesized ToM fields (no
 * external response). Useful as a CI smoke that the rubric + JSON pack stay
 * in sync; not a measure of model quality. */
hu_error_t hu_tom_b8_synthetic_pack_score_gold(hu_allocator_t *alloc, const char *json_path,
                                               unsigned *pass_out, unsigned *total_out);

/* CLI / model-eval hook: score `responses[]` against `tom_synthetic.json`.
 * Each element pairs an item `id` with the model's free-form `response`.
 * Items without a matching id are counted in `*total_out` only when
 * `count_unanswered_as_failed` is non-zero (so a benchmark CLI can decide
 * whether missing answers degrade the score or are skipped).
 *
 * On HU_OK: `*pass_out` is the number of matched items, `*total_out` the
 * number of scored items per the policy above. Caller owns `responses[]`
 * (no copy taken). */
typedef struct hu_tom_b8_response {
    const char *id;
    const char *response;
    size_t response_len;
} hu_tom_b8_response_t;

hu_error_t hu_tom_b8_synthetic_pack_score_responses(hu_allocator_t *alloc, const char *json_path,
                                                    const hu_tom_b8_response_t *responses,
                                                    size_t responses_count,
                                                    int count_unanswered_as_failed,
                                                    unsigned *pass_out, unsigned *total_out);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_TOM_SCENARIO_H */
