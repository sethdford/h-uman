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
 * for the contact. Empty/NULL inputs are silently ignored so callers can
 * hand through optional eval flags without branching. */
void hu_world_model_merge_tom_scenario(hu_world_model_t *wm, const char *premise,
                                       size_t premise_len, const char *question,
                                       size_t question_len, const char *category,
                                       size_t category_len, int64_t now_ms);

/* Run the committed `eval_suites/tom/tom_synthetic.json` smoke checks:
 * each item must parse and produce the expected category tag in ToM output.
 * Returns HU_OK with *pass_out / *total_out set (total may be 0 if no items).
 */
hu_error_t hu_tom_b8_synthetic_pack_run_smoke(hu_allocator_t *alloc, const char *json_path,
                                              unsigned *pass_out, unsigned *total_out);

/* Stricter pack runner: each item's `gold_answer` must appear in the
 * synthesized ToM output via `hu_tom_scenario_gold_matches_response`
 * (segments separated by `_` are required as whole tokens). Counts a
 * pass when the gold answer matches; returns HU_OK with `*pass_out` /
 * `*total_out` populated. */
hu_error_t hu_tom_b8_synthetic_pack_score_gold(hu_allocator_t *alloc, const char *json_path,
                                               unsigned *pass_out, unsigned *total_out);

/* Case-insensitive bounded substring check used by the gold-scoring path.
 * If `gold_answer` contains `_`-separated segments, each segment of length
 * >= `min_token_len` must appear in `response`; otherwise the full
 * `gold_answer` must appear as a substring. Empty / NULL inputs return
 * false. */
bool hu_tom_scenario_gold_matches_response(const char *gold_answer, const char *response,
                                           size_t response_len, size_t min_token_len);

/* Gold rubric: every underscore-delimited segment of `gold_answer` with length
 * >= `min_token_len` must appear as a case-insensitive substring of
 * `response`[0,response_len). If none qualify, the full `gold_answer` must
 * appear instead. */
bool hu_tom_scenario_gold_matches_response(const char *gold_answer, const char *response,
                                           size_t response_len, size_t min_token_len);

/* Items in tom_synthetic.json that include `gold_answer`: count how many pass
 * the rubric against premise + question + synthesized ToM fields. */
hu_error_t hu_tom_b8_synthetic_pack_score_gold(hu_allocator_t *alloc, const char *json_path,
                                             unsigned *pass_out, unsigned *total_out);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_TOM_SCENARIO_H */
