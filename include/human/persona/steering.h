#ifndef HU_PERSONA_STEERING_H
#define HU_PERSONA_STEERING_H

/* SOTA-2026 init-01 — activation steering / SAE persona control.
 *
 * S1 (prompt-half) ships a deterministic projection from persona +
 * personal-model state into a small abstract trait-coefficient
 * vector, plus a renderer that converts that vector into a
 * system-prompt directive snippet. S2 will plug the same vector
 * into on-device providers via the optional
 * `hu_provider_vtable_t.apply_steering` hook (residual-stream
 * addition); cloud providers fall back to the prompt-side
 * directive.
 *
 * Design properties (load-bearing):
 *
 *   - DETERMINISTIC. Same persona + same personal model + same
 *     `now` produce a bit-identical vector and a byte-identical
 *     directive snippet. No clock reads, no RNG, no I/O, no
 *     allocation in the projection path.
 *   - APPEND-ONLY ENUM. `hu_steering_axis_t` indices are stable
 *     across builds so on-device providers built against older
 *     SAE-decoder tables can safely ignore tail slots.
 *   - HARD CAP. Vector length is bounded by HU_STEERING_VEC_MAX_DIM
 *     so the vtable boundary can reject oversize requests in O(1)
 *     without dragging in a heap allocation.
 *
 * Design doc: docs/plans/2026-05-11-init-01-activation-steering.md.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stddef.h>

/* Forward decls — persona.h and personal_model.h both pull a lot
 * of dependencies; this header stays lightweight by referring to
 * the structs by tag. */
struct hu_persona;
struct hu_personal_model;

/* Fixed abstract-vector dimension. Persona + style produce exactly
 * this many [-1.0, +1.0] coefficients. Choose 32 because facet-
 * level persona control (arXiv:2602.19157) maps 30 Big-Five facets
 * cleanly into 32 slots; we have headroom without paying for it. */
#define HU_STEERING_VEC_DIM 32

/* Hard upper bound for the vtable boundary. Providers (and the
 * `hu_provider_apply_steering` helper) reject `dim` above this so
 * the binary-budget delta stays knowable. Matches the cap on the
 * design doc §1.1. */
#ifndef HU_STEERING_VEC_MAX_DIM
#define HU_STEERING_VEC_MAX_DIM 64
#endif

/* Magnitude floor below which a coefficient produces no prompt-
 * side directive sentence. Keeps the directive quiet on weak /
 * uncalibrated signals — matches the configurable
 * `personalization.steering.directive_threshold` default in §1.5. */
#define HU_STEERING_DIRECTIVE_FLOOR 0.15f

/* Stable axis indices. APPEND-ONLY — never reorder, never remove.
 * Slot meaning is binary-format. The first NAMED_COUNT axes have
 * defined meaning today; slots [NAMED_COUNT .. HU_STEERING_VEC_DIM)
 * are reserved for future Big-Five / SAE-discovered features. */
typedef enum hu_steering_axis {
    HU_STEERING_AXIS_WARMTH = 0,
    HU_STEERING_AXIS_FORMALITY,
    HU_STEERING_AXIS_HUMOR_DENSITY,
    HU_STEERING_AXIS_HEDGING,
    HU_STEERING_AXIS_VERBOSITY,
    HU_STEERING_AXIS_EMOJI_AFFINITY,
    HU_STEERING_AXIS_LOWERCASE_AFFINITY,
    HU_STEERING_AXIS_ABBREVIATION_AFFINITY,
    HU_STEERING_AXIS__NAMED_COUNT
} hu_steering_axis_t;

/* Project persona + personal-model state into a steering vector.
 *
 * Contract:
 *   - `out` must point to at least `dim` floats; the function
 *     writes exactly `dim` floats. The vector is fully zeroed
 *     before any axis is written, so unknown / unhandled slots
 *     stay 0.0f.
 *   - `dim` MUST equal HU_STEERING_VEC_DIM. Other dims are
 *     HU_ERR_INVALID_ARGUMENT — keeps the wire format stable.
 *   - NULL `p` is allowed: persona-derived axes (0..3) stay zero.
 *   - NULL `m` is allowed: style-derived axes (4..7) stay zero.
 *   - `now` is a Unix timestamp used to compute
 *     `hu_personal_communication_style_freshness`. Style-derived
 *     axes are multiplied by freshness so a year-old fingerprint
 *     decays toward zero (half-life =
 *     HU_PM_STYLE_OBSERVATION_HALF_LIFE_SEC). Persona-derived
 *     axes are NOT freshness-gated — the persona file is the
 *     user's stated intent and doesn't decay.
 *   - All coefficients are clamped to [-1.0, +1.0]. */
hu_error_t hu_persona_steering_vector(const struct hu_persona *p,
                                      const struct hu_personal_model *m,
                                      long long now,
                                      float *out,
                                      size_t dim);

/* Render a steering vector as a prompt-side directive snippet.
 *
 * Contract:
 *   - `alloc` allocates the returned NUL-terminated buffer; caller
 *     owns it and frees via
 *     `alloc->free(alloc->ctx, *out, *out_len + 1)`.
 *   - `boost` is the retry-loop scaling factor. The verifier-loop
 *     escalation path calls with `boost > 1.0f` to strengthen
 *     directive intensity adverbs on a low-fidelity retry. The
 *     base path uses `boost = 1.0f`. Effective magnitudes are
 *     clamped to [-1, +1] after scaling.
 *   - `out` is set to NULL and `*out_len` to 0 when no axis
 *     crosses HU_STEERING_DIRECTIVE_FLOOR after boost. Return
 *     value is still HU_OK in that quiet case.
 *   - `dim` MUST equal HU_STEERING_VEC_DIM.
 *   - Sentences are ordered by descending |effective coefficient|
 *     so the strongest signal appears first.
 *   - DETERMINISTIC. Same vec + same boost → byte-identical output. */
hu_error_t hu_persona_steering_directive(hu_allocator_t *alloc,
                                         const float *vec,
                                         size_t dim,
                                         float boost,
                                         char **out,
                                         size_t *out_len);

/* Stable axis label for telemetry / logging. Returns a static
 * string; never NULL. Unknown axes map to "unknown". */
const char *hu_persona_steering_axis_label(hu_steering_axis_t axis);

#endif /* HU_PERSONA_STEERING_H */
