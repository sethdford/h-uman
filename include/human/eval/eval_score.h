#ifndef HU_EVAL_SCORE_H
#define HU_EVAL_SCORE_H

/* 2026-05-29 — `human eval score` core: turn a JSONL of generated replies into
 * per-axis humanness scores, using the C scorers as ground truth.
 * Part of the humanness north-star metric
 * (docs/plans/2026-05-29-humanness-north-star-metric/, Phase 2 / T4).
 *
 * The Python nightly harness shells out to `human eval score` and parses the
 * emitted JSON — the cross-language boundary is a process + JSON contract, not
 * FFI (see ~/.claude/rules/cross-language-via-http.md). This header exposes the
 * pure string→string core so it is unit-testable without any file I/O or argv
 * plumbing.
 *
 * Input: one JSON object per line, e.g.
 *   {"prompt":"...", "reply":"hey sounds good", "channel":"imessage",
 *    "contact_id":"partner", "target_register":{"formality":0.1,"warmth":0.9}}
 * Blank lines are skipped. `reply` is required; `channel` defaults to imessage
 * (strictest) when absent/unknown. `target_register` is optional — rows without
 * it are excluded from the relationship axis only.
 *
 * Output (axes present only when ≥1 row contributed; "available":false otherwise):
 *   {"n":N,"axes":{
 *      "anti_ai":     {"mean":..,"stderr":..,"n":..},
 *      "relationship":{"mean":..,"stderr":..,"n":..},
 *      "fidelity":    {"mean":..,"stderr":..,"n":..,"available":bool}}}
 *
 * A1 fidelity is computed only when a non-NULL target style with sample_count>0
 * is supplied (it needs a fingerprint to score against); otherwise the fidelity
 * axis is emitted with "available":false. A2 anti-AI (shape) and A4 relationship
 * never need external state.
 */

#include "human/core/error.h"
#include <stddef.h>

/* Forward-declared to avoid pulling personal_model.h into this header. */
typedef struct hu_communication_style hu_communication_style_t;
typedef struct hu_allocator hu_allocator_t;

/* Score a JSONL buffer. `target_style` may be NULL (fidelity axis omitted).
 * On HU_OK, *out_json is an allocated NUL-terminated JSON string the caller
 * frees via the same allocator; *out_json_len (optional) gets its length.
 * Returns HU_ERR_INVALID_ARGUMENT on NULL jsonl/out_json/alloc. Malformed
 * individual lines are skipped (counted), not fatal. */
hu_error_t hu_eval_score_jsonl(hu_allocator_t *alloc, const char *jsonl, size_t jsonl_len,
                               const hu_communication_style_t *target_style, char **out_json,
                               size_t *out_json_len);

#endif /* HU_EVAL_SCORE_H */
