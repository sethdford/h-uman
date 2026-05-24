#ifndef HU_ML_M3_AB_FIDELITY_GATE_H
#define HU_ML_M3_AB_FIDELITY_GATE_H

/* Spec 2026-05-19 M3 closure / AC-M3-5 — fidelity-based A/B gate.
 *
 * Compares a candidate adapter against a baseline by SCORING actual
 * outputs (not safetensors structure) on a held-out prompt set, then
 * returning PASS iff the candidate's mean fidelity exceeds baseline's
 * by the configured threshold. Uses
 * `hu_communication_style_fidelity_score` from
 * src/memory/personal_model.c — the same scorer the dashboard tile
 * + metrics.fidelity gateway method already use, so all surfaces agree
 * on what "fidelity" means.
 *
 * The gate is INPUT-driven: the caller supplies pre-computed responses
 * for each adapter in the candidate / baseline JSONL files. This keeps
 * the C side free of any HTTP / subprocess concerns; the live-fire
 * script (scripts/m3-live-fire.sh) is responsible for producing those
 * JSONLs by hitting the MLX server with each adapter swapped in.
 *
 * Why we score pre-computed outputs rather than running inference here:
 *   1. Provider boundary: this is the ML layer, not the agent layer.
 *      Spinning up a provider just to test an adapter is over-reach.
 *   2. Determinism: tests can feed canned outputs and pin the verdict.
 *   3. Composability: the bash live-fire script orchestrates HTTP
 *      requests; the C harness just judges. Single responsibility.
 *
 * JSONL shape per row (both `candidate_path` and `baseline_path`):
 *
 *     {"response": "...", "prompt": "..."}
 *
 * The `prompt` field is ignored by the scorer (the score is content-
 * shape-based, not prompt-conditional). It's still included so callers
 * can audit which prompt produced which row.
 *
 * Verdict shape:
 *   - `pass = true` iff (candidate_mean - baseline_mean) >= threshold
 *     AND both sets have at least 1 scored response.
 *   - `pass = false` otherwise.
 *
 * Threshold default and rationale (per D-M3-5):
 *   - Default 0.05 absolute on the [0,1] fidelity scale.
 *   - Configurable; pass 0 to disable the threshold (PASS iff candidate
 *     is strictly better than baseline by any margin).
 *   - At very high baselines (>0.95) the threshold may be hard to clear;
 *     by design — at near-ceiling fidelity, further improvement is
 *     dubious.
 *
 * Per ~/.claude/rules/security-predicate-extraction.md, the threshold
 * decision is extracted into `hu_m3_ab_fidelity_pass` (pure predicate
 * over the two means + threshold) so unit tests can pin the
 * threshold-crossing semantic without needing JSONL fixtures. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/personal_model.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_m3_ab_fidelity_report {
    /* Per-adapter aggregate scores. */
    hu_communication_style_set_summary_t baseline;
    hu_communication_style_set_summary_t candidate;
    /* candidate.mean - baseline.mean. May be negative. */
    float delta;
    /* Threshold used for the pass decision (mirror of the input). */
    float threshold;
    /* Final verdict. */
    bool pass;
    /* When pass=false, names the failure reason (NULL when pass=true). */
    const char *reason;
} hu_m3_ab_fidelity_report_t;

/* Pure predicate (D-M3-5). Returns true iff:
 *   - `threshold >= 0` AND
 *   - (candidate_mean - baseline_mean) >= threshold
 *
 * NaN / inf inputs return false. This is the same predicate the
 * report-emitter calls; extracting it keeps the threshold semantic
 * testable in isolation. */
bool hu_m3_ab_fidelity_pass(float baseline_mean, float candidate_mean, float threshold);

/* Score a held-out prompt set against a target communication style.
 *
 * `responses_jsonl_path` is a JSONL file with one row per held-out
 * prompt; rows must contain a "response" string. Rows that fail to
 * parse, are missing the "response" field, or have empty responses
 * are skipped (recorded in `out_summary->skipped`).
 *
 * `target` must be a non-zero-sample style (the user's fingerprint).
 * Returns HU_OK on success even when zero responses scored (summary's
 * `scored == 0` flags that the caller should ignore the mean).
 *
 * Returns HU_ERR_INVALID_ARGUMENT on NULL pointers.
 * Returns HU_ERR_IO if the JSONL file can't be opened.
 */
hu_error_t hu_m3_ab_score_responses_jsonl(hu_allocator_t *alloc,
                                          const hu_communication_style_t *target,
                                          const char *responses_jsonl_path,
                                          hu_communication_style_set_summary_t *out_summary);

/* Run the full A/B fidelity gate.
 *
 * Loads `baseline_responses_jsonl` and `candidate_responses_jsonl`,
 * scores each against `target`, computes the delta, and emits a
 * structured verdict via `*out_report`.
 *
 * The threshold default (0.05) is applied when `threshold < 0`; pass
 * `threshold >= 0` to override.
 *
 * Returns HU_OK and writes a populated report even on a FAIL verdict.
 * Returns HU_ERR_INVALID_ARGUMENT on NULL pointers / NULL paths /
 * target->sample_count == 0.
 * Returns HU_ERR_IO if either JSONL file can't be opened.
 */
hu_error_t hu_m3_ab_run_fidelity_gate(hu_allocator_t *alloc, const hu_communication_style_t *target,
                                      const char *baseline_responses_jsonl,
                                      const char *candidate_responses_jsonl, float threshold,
                                      hu_m3_ab_fidelity_report_t *out_report);

/* Default fidelity threshold (absolute delta on [0,1] scale).
 * Mirrors design D-M3-5. */
#define HU_M3_AB_FIDELITY_THRESHOLD_DEFAULT 0.05f

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_M3_AB_FIDELITY_GATE_H */
