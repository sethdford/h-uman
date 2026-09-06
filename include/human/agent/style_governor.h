#ifndef HU_AGENT_STYLE_GOVERNOR_H
#define HU_AGENT_STYLE_GOVERNOR_H

/* Style governor — deterministic outbound shape enforcement driven by the
 * MEASURED style card (~/.human/personas/<persona>.style-card.json, written
 * by scripts/measure_style_card.py — the single source for style numbers;
 * see include/human/persona/style_card.h). At the 2026-07-12 measurement
 * the persona ended ~4 in 5 texts with no terminal punctuation and ~1 in 10
 * with '?', against a model baseline of 10% / 31%.
 *
 * Terminal punctuation is the single strongest "this is AI" tell for this
 * persona; the reciprocal trailing question ("What's up with you?") is the
 * second. Prompt rules alone cannot enforce a distribution — this stage
 * does it deterministically at egress.
 *
 * Two actions:
 *   A. Strip a single terminal '.' — hash-gated so ~90% of period-ending
 *      messages lose it (combined with naturally unpunctuated output this
 *      lands near the card's no-punct rate). Ellipses ("...", "…"),
 *      '?', '!' are never touched by this action.
 *   B. Strip a trailing reciprocal-question boilerplate sentence ("What
 *      about you?", "How was your day?") when there is real content before
 *      it. Genuine content-bearing questions never match (exact-phrase
 *      match, not substring — substring-classifier-pitfalls discipline).
 *   C. Capitalize a lowercase start (2026-09-06). The card says the persona
 *      starts lowercase 8.6% of the time (a phone autocapitalizes; the rest
 *      are deliberate overrides); the served adapter starts lowercase 80%
 *      (production_outcomes, last 3 days) and the prompt rule alone did not
 *      move it. Hash-gated so lowercase starts land at the card's rate, not
 *      0%. Applies to the first letter and to the first letter after each
 *      newline (each line is a bubble). URL starts and non-letters are left
 *      alone. HU_STYLE_GOVERNOR_CASING=off disables only this action.
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
#define HU_STYLE_GOV_ACTION_START_CAPITALIZED (1u << 2)

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

/* Full shaping core: actions A, B and C. `casing_roll` is 0-99; a lowercase
 * start is capitalized when casing_roll >= lowercase_start_pct, so exactly
 * lowercase_start_pct% of lowercase-starting messages keep it. 100 never
 * capitalizes (the casing kill switch); 0 always does.
 * hu_style_governor_shape is this with casing disabled (pct 100). */
hu_error_t hu_style_governor_shape_ex(hu_allocator_t *alloc, const char *text, size_t len,
                                      unsigned period_roll, unsigned casing_roll,
                                      unsigned lowercase_start_pct, char **out, size_t *out_len,
                                      unsigned *actions);

/* Second, independent 0-99 roll for action C (different hash basis, so the
 * casing decision is not correlated with the period decision). */
unsigned hu_style_governor_casing_roll(const char *text, size_t len);

/* Card-derived lowercase-start percentage for action C, resolved once per
 * process from the persona's style card (compiled default when absent) and
 * cached; 100 when HU_STYLE_GOVERNOR_CASING=off. `persona` may be NULL. */
struct hu_persona;
unsigned hu_style_governor_lowercase_start_pct(const struct hu_persona *persona);

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
