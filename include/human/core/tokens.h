#ifndef HU_CORE_TOKENS_H
#define HU_CORE_TOKENS_H

#include <stddef.h>

/* Canonical byte->token estimate.
 *
 * Before 2026-07-27 this approximation existed as five public entry points
 * (hu_context_estimate_tokens, hu_estimate_tokens_text, hu_agent_estimate_tokens,
 * hu_directive_estimate_tokens, hu_estimate_tokens) plus ~23 inline `len / 4`
 * open-codings across src/agent/. Nothing named the constant, so there was no
 * place to put the evidence for it — and no place to change it.
 *
 * WHY 4, AND WHY NOT 4.5 (measured 2026-07-27)
 * -------------------------------------------
 * Measured directly against the serving tokenizer on 1,384 real strings
 * (125 KB: persona JSON prompt text from ~/.human/personas, plus the
 * blinded-A/B sheet's real messages and replies), via AutoTokenizer, with no
 * server involved:
 *
 *   GLM-4.5-Air-4bit (SERVING) : mean 4.506 B/tok, median 4.533, p10 3.28, p90 6.00
 *   gemma-4-31b-it-4bit (PRIOR): mean 4.514 B/tok, median 4.612, p10 3.28, p90 6.20
 *
 * Two conclusions, both load-bearing:
 *
 * 1. The 2026-07-26 base flip gemma -> GLM did NOT invalidate this constant.
 *    The two tokenizers land within 0.2% of each other on real h-uman text. If
 *    you are here because you assumed a base change broke token accounting:
 *    it did not. Re-measure before believing otherwise.
 *
 * 2. 4 is deliberately BELOW the mean, and must stay there. This estimate
 *    drives context budgeting and compaction triggers, where the error is
 *    asymmetric: over-estimating tokens costs a little context, while
 *    under-estimating overflows the model's window and fails the request.
 *    p10 = 3.28 means ~10% of real strings are DENSER than 4 bytes/token and
 *    are already under-estimated at 4. Moving to the measured mean of 4.5
 *    would under-estimate a materially larger share of the corpus and make
 *    budgeting less safe, not more accurate.
 *
 * So: do not "correct" this to 4.5. It is not a rounding of the mean; it is a
 * conservative quantile chosen for a budgeting estimator. Changing it is a
 * behavior change to compaction and trimming and needs its own measurement
 * (.claude/rules/feature-gate-requires-measurement.md), not just a better
 * average. Re-run the measurement if the serving base changes families:
 *   scripts/measure-bytes-per-token.py
 */
#define HU_TOKENS_BYTES_PER_TOKEN 4u

/* Estimate tokens for a single text run, rounding UP so a non-empty string is
 * never estimated at zero tokens. NULL text -> 0. This is the shape three of
 * the five prior entry points already had ((len + 3) / 4); they now delegate
 * here. Callers that accumulate over a message array keep their own
 * per-message overhead — those overheads legitimately differ (chat formatting
 * costs ~4 tokens/message, compaction's history accounting ~0.75) and are not
 * part of this ratio. */
size_t hu_tokens_estimate_text(const char *text, size_t len);

/* Length-only form, for call sites that have a byte count but no pointer (the
 * `.response_length_tokens_est = ...` designated initializers in agent_stream.c
 * and agent_turn.c). Identical arithmetic to hu_tokens_estimate_text.
 *
 * Those sites previously open-coded `len / 4`, which rounds DOWN: a 3-byte
 * reply was recorded as 0 tokens in the M3 outcome row. Rounding up fixes that
 * — the change is bounded at +1 token per record and only ever moves a value
 * that was too low. */
size_t hu_tokens_estimate_len(size_t len);

#endif /* HU_CORE_TOKENS_H */
