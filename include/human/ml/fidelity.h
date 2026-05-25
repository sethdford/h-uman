#ifndef HU_ML_FIDELITY_H
#define HU_ML_FIDELITY_H

/* Persona-fidelity computation primitives shared by the CLI
 * (`human ml fidelity-status`) and the gateway (`metrics.fidelity`).
 *
 * Both surfaces return the same JSON shape — keeping the
 * compute path in one place ensures the dashboard tile, the
 * CLI output, and any future telemetry agree on what
 * "baseline mean" actually means. The gateway and CLI differ
 * only in (a) where the JSON gets written and (b) how the
 * persona name is resolved (CLI from `--persona`, gateway from
 * `params.persona` or the active agent's persona).
 *
 * No allocations escape these helpers — out_summary is a value
 * type, populated by the function. The caller owns the persona
 * lifecycle (must be loaded before, deinit'd after). */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include <stdbool.h>

/* Resolve the communication-style fingerprint used as the target
 * for fidelity scoring. Tries to load `~/.human/personal_model.bin`
 * first; on failure (file missing, parse error, zero-sample style),
 * falls back to a deterministic synthetic profile (casual lowercase,
 * moderate emoji, ~60-char average — the same defaults
 * `human ml lora-baseline` uses).
 *
 * Out:
 *   - `*out_target`: filled with a non-zero-sample style.
 *   - `*out_synthetic`: true when the synthetic fallback fired.
 *
 * Always returns HU_OK because the synthetic path is infallible.
 * NULL args set `out_synthetic = true` and `out_target` to the
 * synthetic defaults. */
hu_error_t hu_ml_fidelity_resolve_target(hu_allocator_t *alloc,
                                         hu_communication_style_t *out_target, bool *out_synthetic);

/* Score every non-empty `response` in every example bank against
 * `target` and accumulate a per-persona summary (scored / skipped /
 * mean / min / max). Empty / over-short / score-of-(-1) responses
 * are counted as `skipped` and don't drag the mean — same shape as
 * the comparator's per-set summaries.
 *
 * `target->sample_count == 0` is rejected with
 * HU_ERR_INVALID_ARGUMENT — a zero-sample target produces meaningless
 * scores. Use `hu_ml_fidelity_resolve_target` to guarantee a valid
 * target. */
hu_error_t hu_ml_fidelity_score_baseline(const hu_persona_t *persona,
                                         const hu_communication_style_t *target,
                                         hu_communication_style_set_summary_t *out_summary);

/* ── US-7 — fidelity delta scorer ──────────────────────────────────
 *
 * Compute the fidelity delta for a single pair of responses.
 * Scores both `baseline` and `adapted` against `target`, returning
 * the difference (adapted - baseline).
 *
 * Inputs:
 *   - `baseline_score`: fidelity of baseline response, ∈ [0, 1]
 *   - `adapted_score`:  fidelity of adapted response, ∈ [0, 1]
 *   - `target`:         reserved for future per-target weighting;
 *                       ignored in v1
 *
 * Returns:
 *   - Float ∈ [-1, 1] representing the delta
 *   - Positive delta indicates adapted is closer to target
 *   - Returns 0.0 if inputs are out of range or NULL (with warn log)
 *
 * Note: This function is intended for single-pair scoring; for
 * set-level aggregation use `hu_communication_style_compare_response_sets`. */
double hu_communication_style_fidelity_score_delta(double baseline_score, double adapted_score,
                                                   const hu_communication_style_t *target);

/* ── US-7.6 — judgment-fidelity (INS-A) ───────────────────────────────
 *
 * The judgment-fidelity scorer measures mean negative log-likelihood
 * (NLL) of real held-out continuations under whatever inference
 * backend is currently registered. This is the load-bearing
 * semantic check that catches adapters which pass lexical-surface
 * fidelity gates but produce un-Seth-like decisions.
 *
 * Sprint 7 ships this seam DORMANT (decision D3 in
 * `sprints/sprint-7/decisions.md`): no production NLL backend is
 * registered, the default returns HU_ERR_NOT_SUPPORTED, and the
 * CLI / shell gate emit a visible "SKIP" / "not_supported" status
 * so downstream automation cannot silently treat the inactive gate
 * as a pass. Follow-on US-7.6.1 (deferred) wires `src/ml/gpt.c` as
 * the real NLL backend. */

/* Compute mean negative log-likelihood of `continuation` conditioned
 * on `prompt`.
 *
 * Returns:
 *   HU_OK                — `*out_nll` populated with a finite double
 *                          >= 0.0 (scored row).
 *   HU_ERR_NOT_SUPPORTED — no inference backend wired (production
 *                          default). Caller treats this as
 *                          "judgment scoring unavailable", NOT a
 *                          failure: the row counts as `skipped`.
 *   any other error      — hard failure; caller propagates.
 *
 * MUST NOT allocate memory the caller has to free. MUST be reentrant.
 * In HU_IS_TEST builds with a registered mock, MUST NOT touch the
 * filesystem, network, or model weights — the verifier asserts on
 * this. */
typedef hu_error_t (*hu_ml_nll_compute_fn_t)(const char *prompt, size_t prompt_len,
                                             const char *continuation, size_t continuation_len,
                                             void *ctx, double *out_nll);

/* Replace the process-wide NLL computer. Pass NULL fn to restore the
 * default (which returns HU_ERR_NOT_SUPPORTED). Thread-safety: not
 * thread-safe; call from main thread only during setup. */
void hu_ml_fidelity_set_nll_compute_fn(hu_ml_nll_compute_fn_t fn, void *ctx);

/* One held-out (prompt, continuation) row loaded from a JSONL
 * fixture. All strings are alloc-owned and NUL-terminated. */
typedef struct {
    char *prompt;
    size_t prompt_len;
    char *continuation;
    size_t continuation_len;
} hu_ml_judgment_holdout_row_t;

typedef struct {
    hu_ml_judgment_holdout_row_t *rows;
    size_t rows_count;
    size_t rows_capacity;
} hu_ml_judgment_holdout_t;

/* Default fixture path: `$HU_JUDGMENT_HOLDOUT` env var if set,
 * otherwise `tests/fixtures/judgment_fidelity_holdout.jsonl`
 * relative to the current working directory. Returns a static /
 * borrowed pointer; do not free. */
const char *hu_ml_fidelity_default_holdout_path(void);

/* Load a JSONL holdout fixture. Each non-empty, non-comment line
 * must be a JSON object containing string fields `prompt` and
 * `continuation`. Malformed lines and lines missing required
 * fields are silently dropped (counted as `skipped` later by
 * `hu_ml_fidelity_score_judgment`). On success `*out` owns its
 * memory; release with `hu_ml_fidelity_free_holdout`. */
hu_error_t hu_ml_fidelity_load_holdout(hu_allocator_t *alloc, const char *path,
                                       hu_ml_judgment_holdout_t *out);

void hu_ml_fidelity_free_holdout(hu_allocator_t *alloc, hu_ml_judgment_holdout_t *holdout);

typedef struct {
    size_t scored;   /* rows where the registered NLL fn returned HU_OK */
    size_t skipped;  /* rows where the NLL fn returned HU_ERR_NOT_SUPPORTED
                        or where the row was malformed at score time */
    double mean_nll; /* arithmetic mean over scored rows; 0.0 if scored == 0 */
    double min_nll;  /* over scored rows; 0.0 if scored == 0 */
    double max_nll;  /* over scored rows; 0.0 if scored == 0 */
    bool available;  /* true iff scored > 0; false maps to
                        judgment_ppl_status="not_supported_no_local_inference"
                        in CLI / JSON / shell-gate output */
} hu_ml_judgment_summary_t;

/* Score every row in `holdout` by invoking the registered NLL fn.
 * Rows that return HU_ERR_NOT_SUPPORTED are counted as skipped
 * (not failed). Any other error from the NLL fn aborts and is
 * propagated.
 *
 * `holdout->rows_count == 0` is legal (yields available=false). */
hu_error_t hu_ml_fidelity_score_judgment(const hu_ml_judgment_holdout_t *holdout,
                                         hu_ml_judgment_summary_t *out_summary);

#endif /* HU_ML_FIDELITY_H */
