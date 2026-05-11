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
                                         hu_communication_style_t *out_target,
                                         bool *out_synthetic);

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

#endif /* HU_ML_FIDELITY_H */
