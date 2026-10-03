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
 *   D. Capitalize a known entity mention mid-sentence (2026-09-22). At
 *      MATCHED reply length the served adapter names 229 insider entities
 *      against the persona's 252 (91% — parity, so retrieval is not the
 *      gap) but capitalizes them 3.1% of the time against the persona's
 *      26.6%: an 8.6x deficit that is 61% of the length-matched specificity
 *      gap. As with action C the prompt rule cannot enforce a distribution,
 *      and as with action C the tell is unnatural CONSISTENCY (97/3
 *      lowercase vs the persona's 73/27) rather than a missing capability.
 *      Per-token rates come from the style card's entity_casing block, which
 *      doubles as an ALLOWLIST — a token the persona never capitalizes is
 *      never in the table and is never touched, so this cannot overshoot
 *      into capitalizing words the persona writes lowercase. Length is
 *      never changed (a case flip is one byte in place), preserving the
 *      apply_inplace shrink-only invariant below.
 *      Gate: HU_STYLE_GOVERNOR_ENTITY_CASING = off (DEFAULT) | live. Off by
 *      default per the feature-gate rule — flipping it changes what gets
 *      sent, so it needs its own measurement before it goes live.
 *
 * STYLE_GOVERNOR activation is gated on the blind A/B rating-drip
 * measurement (docs/evaluation/blind_ab_gate.json): do not flip to
 * default-LIVE without a human-tier verdict showing the shaped output is
 * judged more Seth-like. Env gate HU_STYLE_GOVERNOR = off (default) |
 * shadow (log would-do, send unchanged) | live.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona/style_card.h"
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
#define HU_STYLE_GOV_ACTION_PERIOD_STRIPPED    (1u << 0)
#define HU_STYLE_GOV_ACTION_QUESTION_STRIPPED  (1u << 1)
#define HU_STYLE_GOV_ACTION_START_CAPITALIZED  (1u << 2)
#define HU_STYLE_GOV_ACTION_ENTITY_CAPITALIZED (1u << 3)

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

/* Full shaping core including action D. `entities` may be NULL (action D
 * off), otherwise it is `count` entries of the style card's entity table.
 * hu_style_governor_shape_ex is this with entities = NULL, so every existing
 * caller keeps its exact behaviour. */
hu_error_t hu_style_governor_shape_full(hu_allocator_t *alloc, const char *text, size_t len,
                                        unsigned period_roll, unsigned casing_roll,
                                        unsigned lowercase_start_pct,
                                        const hu_style_entity_token_t *entities, unsigned count,
                                        char **out, size_t *out_len, unsigned *actions);

/* Second, independent 0-99 roll for action C (different hash basis, so the
 * casing decision is not correlated with the period decision). */
unsigned hu_style_governor_casing_roll(const char *text, size_t len);

/* Action C for one BUBBLE, in place (length never changes). Shaping runs on
 * the whole reply before it is split into bubbles, so a bubble cut mid-line
 * kept a lowercase start (2026-09-30: 32% of follow-on bubbles vs Seth's 9%).
 * Capitalizes the first letter unless `casing_roll` < `lowercase_start_pct`
 * (which keeps the card's measured share); URLs are left alone. Returns true
 * when it changed the buffer. */
bool hu_style_governor_case_bubble_pct(char *buf, size_t len, unsigned lowercase_start_pct,
                                       unsigned casing_roll);

/* Per-token 0-99 roll for action D. Mixes the token bytes, the message's
 * casing roll and the token's byte offset, so: the same message always
 * shapes identically (reproducible), two mentions of the same token in one
 * message can differ, and the same token differs across messages — which is
 * the point, since the defect being fixed is uniformity. */
unsigned hu_style_governor_entity_roll(const char *token, size_t len, unsigned casing_roll,
                                       size_t offset);

struct hu_persona;

/* Action D's entity table, resolved once per process from the persona's
 * style card and cached. Returns NULL (and *out_count 0) when
 * HU_STYLE_GOVERNOR_ENTITY_CASING is not "live", when the card predates the
 * entity_casing axis, or when the table is empty. `persona` may be NULL. */
const hu_style_entity_token_t *hu_style_governor_entity_table(const struct hu_persona *persona,
                                                              unsigned *out_count);

/* Card-derived lowercase-start percentage for action C, resolved once per
 * process from the persona's style card (compiled default when absent) and
 * cached; 100 when HU_STYLE_GOVERNOR_CASING=off. `persona` may be NULL. */
struct hu_persona;
unsigned hu_style_governor_lowercase_start_pct(const struct hu_persona *persona);

/* hu_style_governor_case_bubble_pct with the persona's card rate and the
 * bubble's own casing roll; a no-op unless the governor is LIVE. */
bool hu_style_governor_case_bubble(const struct hu_persona *persona, char *buf, size_t len);

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
/* Drop the cached action-C percentage and action-D entity table so a test
 * can change the env or the card and re-resolve. */
void hu_style_governor_reset_casing_for_test(void);
void hu_style_governor_reset_entities_for_test(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_STYLE_GOVERNOR_H */
