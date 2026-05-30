#ifndef HU_EVAL_REGISTER_H
#define HU_EVAL_REGISTER_H

/* 2026-05-29 — A4 relationship-calibration axis for the humanness north-star
 * metric (docs/plans/2026-05-29-humanness-north-star-metric/).
 *
 * The humanness composite asks four questions of a generated reply; this file
 * answers the fourth: "does the reply's REGISTER (how formal, how warm) match
 * what THIS contact warrants?" A perfectly Seth-voiced, perfectly un-AI reply
 * can still be wrong if it's chummy to a stranger or stiff to a partner.
 *
 * These are pure deterministic predicates — no allocation, no I/O, NULL-safe —
 * so they're unit-testable in isolation and safe to call from the eval CLI's
 * `score` mode or from any context. They reuse the SAME relationship vocabulary
 * the render path already trusts (warmth tiers in follow_up.h, the formality
 * notion behind hu_persona_effective_formality) so the measurer and the
 * generator share one definition of "casual" / "warm".
 *
 * Scores are all in [0.0, 1.0]:
 *   formality: 0.0 = very casual (lowercase textisms), 1.0 = very formal
 *              (salutations, full punctuation, no contractions).
 *   warmth:    0.0 = distant/transactional, 1.0 = warm (greetings, endearments).
 *
 * NULL / empty text is UNMEASURABLE, not "bad" — estimators return the neutral
 * 0.5 for it. Emptiness is the shape classifier's job (HU_SHAPE_FAIL_EMPTY),
 * not this axis's; keeping these neutral-on-empty makes them cleanly composable.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Estimate how FORMAL a piece of text reads, in [0,1]. Deterministic heuristic
 * over casual markers (contractions, textisms like "lol"/"u"/"rn", all-lowercase,
 * slang) vs formal markers (salutations/closings, full capitalization +
 * terminal punctuation, absence of contractions). NULL/empty → 0.5 (neutral). */
double hu_register_formality_estimate(const char *text, size_t len);

/* Estimate how WARM a piece of text reads, in [0,1]. Deterministic heuristic
 * over warm markers (greetings, endearments, emoji, enthusiasm, warm sign-offs)
 * vs distant markers (terse/transactional phrasing, imperative with no greeting).
 * NULL/empty → 0.5 (neutral). */
double hu_register_warmth_estimate(const char *text, size_t len);

/* The A4 relationship-calibration score in [0,1]: how close the reply's measured
 * register is to the target register this contact warrants. Computed as
 *   1 - (0.5*|formality_measured - target_formality| + 0.5*|warmth_measured - target_warmth|)
 * clamped to [0,1]. 1.0 = perfectly calibrated; lower = mismatched.
 *
 * target_formality / target_warmth are the fixture's per-contact expectations,
 * each in [0,1]; out-of-range targets are clamped. NULL/empty text yields the
 * score against the neutral 0.5 estimates. */
double hu_relationship_axis_score(const char *text, size_t len, double target_formality,
                                  double target_warmth);

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_REGISTER_H */
