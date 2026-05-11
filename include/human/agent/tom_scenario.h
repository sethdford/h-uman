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

/* Run the committed `eval_suites/tom/tom_synthetic.json` smoke checks:
 * each item must parse and produce the expected category tag in ToM output.
 * Returns HU_OK with *pass_out / *total_out set (total may be 0 if no items).
 */
hu_error_t hu_tom_b8_synthetic_pack_run_smoke(hu_allocator_t *alloc, const char *json_path,
                                              unsigned *pass_out, unsigned *total_out);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_TOM_SCENARIO_H */
