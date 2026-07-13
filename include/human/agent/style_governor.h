#ifndef HU_AGENT_STYLE_GOVERNOR_H
#define HU_AGENT_STYLE_GOVERNOR_H

/* Style governor — deterministic outbound shape enforcement driven by the
 * MEASURED style card (scripts/persona_style_card.py, 2026-07-12, n=1488):
 *
 *   no-terminal-punct = 79%   (model baseline: 10%)
 *   ?-endings         =  9%   (model baseline: 31%)
 *
 * Terminal punctuation is the single strongest "this is AI" tell for this
 * persona; the reciprocal trailing question ("What's up with you?") is the
 * second. Prompt rules alone cannot enforce a distribution — this stage
 * does it deterministically at egress.
 *
 * Two actions:
 *   A. Strip a single terminal '.' — hash-gated so ~90% of period-ending
 *      messages lose it (combined with naturally unpunctuated output this
 *      lands near the measured 79% no-punct rate). Ellipses ("...", "…"),
 *      '?', '!' are never touched by this action.
 *   B. Strip a trailing reciprocal-question boilerplate sentence ("What
 *      about you?", "How was your day?") when there is real content before
 *      it. Genuine content-bearing questions never match (exact-phrase
 *      match, not substring — substring-classifier-pitfalls discipline).
 *
 * STYLE_GOVERNOR activation is gated on the blind A/B rating-drip
 * measurement (docs/evaluation/blind_ab_gate.json): do not flip to
 * default-LIVE without a human-tier verdict showing the shaped output is
 * judged more Seth-like. Env gate HU_STYLE_GOVERNOR = off (default) |
 * shadow (log would-do, send unchanged) | live.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hu_style_governor_mode {
    HU_STYLE_GOVERNOR_OFF = 0,
    HU_STYLE_GOVERNOR_SHADOW = 1,
    HU_STYLE_GOVERNOR_LIVE = 2,
} hu_style_governor_mode_t;

/* Action bits reported by hu_style_governor_shape. */
#define HU_STYLE_GOV_ACTION_PERIOD_STRIPPED   (1u << 0)
#define HU_STYLE_GOV_ACTION_QUESTION_STRIPPED (1u << 1)

/* Pure shaping core (security-predicate-extraction pattern: testable
 * without the pipeline).
 *
 * `period_roll` is 0-99; a terminal '.' is stripped when
 * period_roll < HU_STYLE_GOV_PERIOD_STRIP_PCT. Callers derive it
 * deterministically from the message hash so the same text always
 * shapes the same way (no Math.random-style flakiness).
 *
 * On change: *out is a freshly allocated NUL-terminated string
 * (caller frees via alloc, size *out_len + 1), *actions has the bits.
 * On no change: *out is NULL, *out_len 0, *actions 0. */
#define HU_STYLE_GOV_PERIOD_STRIP_PCT 90u

hu_error_t hu_style_governor_shape(hu_allocator_t *alloc, const char *text, size_t len,
                                   unsigned period_roll, char **out, size_t *out_len,
                                   unsigned *actions);

/* FNV-1a based 0-99 roll for a message — exposed so tests and the stage
 * derive identical values. */
unsigned hu_style_governor_roll(const char *text, size_t len);

/* Resolve mode from HU_STYLE_GOVERNOR (cached after first call). */
hu_style_governor_mode_t hu_style_governor_mode(void);

/* In-place apply for call sites that BYPASS the outbound pipeline — i.e.
 * the reactive daemon send path, which runs its own inline chain instead
 * of hu_outbound_pipeline_run and therefore never reaches the
 * style_governor stage. Resolves the mode, shapes `buf` in place when
 * LIVE (the governor only ever shrinks, so `buf` needs no extra capacity),
 * logs the would-do in SHADOW, and is a no-op when OFF. Returns the new
 * length. Same gate + shaping as the pipeline stage — so promoting
 * HU_STYLE_GOVERNOR to live shapes reactive AND pipeline paths uniformly. */
size_t hu_style_governor_apply_inplace(hu_allocator_t *alloc, char *buf, size_t len);

#if HU_IS_TEST
/* Override the cached mode (tests only). Pass -1 to re-read the env. */
void hu_style_governor_set_mode_for_test(int mode);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_STYLE_GOVERNOR_H */
