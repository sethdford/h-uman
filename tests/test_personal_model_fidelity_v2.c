/* Phase 5 Task 1 (RL SOTA) — opt-in 4-axis fidelity scorer (v2).
 *
 * Pins the round-1 BLOCKER-1 contract: the v1 scorer
 * (`hu_communication_style_fidelity_score`) and its existing call
 * sites are NEVER modified. The new v2 surface
 * (`hu_communication_style_fidelity_score_v2` +
 *  `hu_communication_style_compare_response_sets_v2`) is additive,
 * opt-in only, and adds a 4th composite "decision-style" axis
 * derived from the response's hedging / question / imperative
 * framing against the target's EWMA-tracked decision-style ratios.
 *
 * The five tests below cover:
 *   1. v2 rewards a response that mirrors the target's
 *      direct/imperative framing and penalises a hedge-heavy
 *      mismatch (the "higher score for matching decision style").
 *   2. v2 collapses the 4th axis to the neutral 0.5 when the
 *      target has no decision-style fingerprint yet (all three
 *      sub-axes are zero).
 *   3. v2 returns -1.0 on zero `sample_count` (same boundary
 *      semantics as v1, so the eval gate / competitive harness
 *      can treat -1.0 uniformly across versions).
 *   4. v1 stays byte-stable on a known-good input AFTER v2 lands —
 *      this is the regression guard for BLOCKER-1: any future
 *      change that accidentally re-wires v1 to the v2 body fails
 *      here long before reaching the production call sites.
 *   5. The v2 batch comparator returns a finite, correctly-signed
 *      delta on two response sets — the contract the Phase 5
 *      Task 9 competitive harness will rely on.
 *
 * No persona dependency: the 4th axis reads its target from
 * `hu_communication_style_t` (extended additively at the END of
 * the struct), not from `hu_persona_t.decision_style`. This keeps
 * the scorer pure-CPU and side-effect-free, mirrors how the v1
 * scorer already gets every axis from `hu_communication_style_t`,
 * and avoids cross-subsystem coupling between memory/ and
 * persona/. */

#include "test_framework.h"
#include "human/memory/personal_model.h"

#include <math.h>
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────
 * Test 1: v2 rewards a response that mirrors the target's
 *         imperative / low-hedging framing, and penalises a
 *         hedge-heavy mismatch by at least 0.10.
 * ────────────────────────────────────────────────────────────────────── */
static void test_fidelity_v2_returns_higher_score_for_matching_decision_style(void) {
    hu_communication_style_t target;
    memset(&target, 0, sizeof(target));
    target.lowercase_ratio = 1.0f;
    target.abbreviation_ratio = 0.0f;
    target.avg_message_length = 50;
    target.sample_count = 100;
    target.last_observed_at = 1000;
    /* Direct, imperative target persona — high imperative_ratio,
     * low hedging_ratio, low question_ratio. */
    target.hedging_ratio = 0.05f;
    target.question_ratio = 0.10f;
    target.imperative_ratio = 0.80f;

    const char *matching = "do this now. check the logs. fix the bug.";
    const char *mismatched =
        "maybe we could perhaps try to see if it might be possible to consider doing this?";

    float score_match =
        hu_communication_style_fidelity_score_v2(&target, matching, strlen(matching));
    float score_mismatch =
        hu_communication_style_fidelity_score_v2(&target, mismatched, strlen(mismatched));

    HU_ASSERT_TRUE(score_match >= 0.f && score_match <= 1.f);
    HU_ASSERT_TRUE(score_mismatch >= 0.f && score_mismatch <= 1.f);
    HU_ASSERT_TRUE(score_match > 0.5f);
    /* Mismatched must score visibly lower; the 0.10 gap is the
     * minimum signal the eval gate is allowed to rely on. */
    HU_ASSERT_TRUE(score_mismatch < score_match - 0.10f);
}

/* ──────────────────────────────────────────────────────────────────────
 * Test 2: when the target's three decision-style ratios are all
 *         zero (no fingerprint observed yet), the v2 composite
 *         axis collapses to the neutral 0.5 — so an
 *         un-fingerprinted target neither rewards nor penalises
 *         the response on the 4th axis. The first three axes
 *         still drive the score.
 * ────────────────────────────────────────────────────────────────────── */
static void test_fidelity_v2_decision_style_neutral_when_target_axes_absent(void) {
    /* All decision-style axes are zero (default zero-init). The
     * first three axes are tuned so v1 and v2 share the same
     * mean over them, and the v2 score then differs from v1
     * exactly by the (decision_match - mean3) / 4 redistribution. */
    hu_communication_style_t target;
    memset(&target, 0, sizeof(target));
    target.lowercase_ratio = 1.0f;
    target.abbreviation_ratio = 0.0f;
    target.avg_message_length = 20;
    target.sample_count = 10;
    target.last_observed_at = 1000;
    /* hedging_ratio / question_ratio / imperative_ratio stay 0. */

    const char *resp = "hey friend how are you";

    float v1 = hu_communication_style_fidelity_score(&target, resp, strlen(resp));
    float v2 = hu_communication_style_fidelity_score_v2(&target, resp, strlen(resp));

    /* v1 = (1.0 + 1.0 + length_match) / 3
     * v2 = (1.0 + 1.0 + length_match + 0.5)        / 4
     *
     * Derivation: when the target's decision axes are all zero, the
     * composite collapses to the constant 0.5 — so v2 is the same
     * three v1 terms plus 0.5 averaged across 4 bins.
     *
     * Numerically: length_match = 1 - |22 - 20| / 20 = 0.9
     *   v1 = (1.0 + 1.0 + 0.9) / 3        = 0.96667
     *   v2 = (1.0 + 1.0 + 0.9 + 0.5) / 4  = 0.85
     *
     * Allow a small float-eps tolerance. */
    HU_ASSERT_TRUE(fabsf(v1 - 0.96667f) < 1e-3f);
    HU_ASSERT_TRUE(fabsf(v2 - 0.85f) < 1e-3f);
    /* Sanity: the two MUST differ — that's the whole point of the
     * 4th axis. */
    HU_ASSERT_TRUE(fabsf(v1 - v2) > 0.001f);
}

/* ──────────────────────────────────────────────────────────────────────
 * Test 3: v2 returns -1.0 on `sample_count == 0` and on
 *         NULL / zero-length response, matching the v1 boundary
 *         contract. The eval gate / competitive harness treat
 *         -1.0 as "no comparison possible" uniformly across
 *         versions; v2 must not silently drift to 0.0 or 0.5
 *         in these cases.
 * ────────────────────────────────────────────────────────────────────── */
static void test_fidelity_v2_returns_neg_one_on_invalid_inputs(void) {
    hu_communication_style_t target;
    memset(&target, 0, sizeof(target));
    target.lowercase_ratio = 1.0f;
    target.imperative_ratio = 0.5f;
    /* sample_count == 0 → no fingerprint, refuse to compare. */
    HU_ASSERT_TRUE(hu_communication_style_fidelity_score_v2(&target, "do it", 5) == -1.f);

    target.sample_count = 5;
    /* NULL target. */
    HU_ASSERT_TRUE(hu_communication_style_fidelity_score_v2(NULL, "do it", 5) == -1.f);
    /* NULL / empty response. */
    HU_ASSERT_TRUE(hu_communication_style_fidelity_score_v2(&target, NULL, 0) == -1.f);
    HU_ASSERT_TRUE(hu_communication_style_fidelity_score_v2(&target, "x", 0) == -1.f);
}

/* ──────────────────────────────────────────────────────────────────────
 * Test 4: BLOCKER-1 regression guard.
 *
 * After v2 lands, the v1 scorer's body MUST still produce the
 * same byte-stable score on a known-good input as before. The
 * pinned value below was hand-derived from v1's documented 3-axis
 * mean formula:
 *
 *   v1 = (lower_match + abbrev_match + length_match) / 3
 *
 * For:
 *   target = {lowercase=1.0, abbreviation=0.3, avg_len=20,
 *             sample_count=10, decision-axes all 0}
 *   resp   = "hey friend how are you"   (22 bytes, all lowercase,
 *                                         no abbrevs)
 *
 *   lower_match  = 1 - |1.0 - 1.0|          = 1.000
 *   abbrev_match = 1 - |0.0 - 0.3|          = 0.700
 *   length_match = 1 - |22 - 20| / 20       = 0.900
 *   v1           = (1.000 + 0.700 + 0.900)/3 = 0.86667
 *
 * If a future change ever inadvertently rewires v1 to read the new
 * decision-style fields or to call into the v2 body, this test
 * fails — long before any production caller in personal_model.c,
 * ml/cli.c, or ml/fidelity.c is affected.
 *
 * The struct's new fields (hedging_ratio / question_ratio /
 * imperative_ratio) sit at the END of the struct AFTER
 * last_observed_at, so zero-init via memset on a stack
 * `hu_communication_style_t` still defaults them to 0.0f, exactly
 * as a pre-Phase-5 binary would have observed. */
static void test_fidelity_v1_unchanged_after_v2_lands(void) {
    hu_communication_style_t target;
    memset(&target, 0, sizeof(target));
    target.lowercase_ratio = 1.0f;
    target.abbreviation_ratio = 0.3f;
    target.avg_message_length = 20;
    target.sample_count = 10;
    target.last_observed_at = 1000;
    /* The three new fields are 0.0f via memset — pre-Phase-5
     * binaries read 0 here too, so v1's score must be identical
     * before and after this commit. */
    HU_ASSERT_TRUE(fabsf(target.hedging_ratio) < 1e-6f);
    HU_ASSERT_TRUE(fabsf(target.question_ratio) < 1e-6f);
    HU_ASSERT_TRUE(fabsf(target.imperative_ratio) < 1e-6f);

    const char *resp = "hey friend how are you";
    float v1 = hu_communication_style_fidelity_score(&target, resp, strlen(resp));
    HU_ASSERT_TRUE(fabsf(v1 - 0.86667f) < 1e-3f);

    /* And v2 on the same input MUST differ — the 4th axis is
     * non-trivially contributing (here collapsing to 0.5 because
     * the target's decision-style fingerprint is empty). */
    float v2 = hu_communication_style_fidelity_score_v2(&target, resp, strlen(resp));
    HU_ASSERT_TRUE(fabsf(v1 - v2) > 0.001f);
}

/* ──────────────────────────────────────────────────────────────────────
 * Test 5: v2 batch comparator returns a finite, correctly-signed
 *         delta when one set is closer to the target style than
 *         the other.
 *
 * This is the contract the Phase 5 Task 9 competitive harness
 * will rely on to score baseline vs RL-policy response sets
 * under the 4-axis metric. We don't go through bootstrap p-value
 * here — that belongs to the harness layer (it composes this
 * comparator with `hu_bootstrap_ci_compute` from Task 2). What
 * THIS test pins:
 *   - Both summaries populate (scored, mean, min, max).
 *   - delta = mean_b - mean_a, finite.
 *   - The sign matches the per-set means.
 * ────────────────────────────────────────────────────────────────────── */
static void test_fidelity_v2_compare_response_sets_returns_finite_delta(void) {
    /* Target: direct, imperative, casual. */
    hu_communication_style_t target;
    memset(&target, 0, sizeof(target));
    target.lowercase_ratio = 1.0f;
    target.abbreviation_ratio = 0.1f;
    target.avg_message_length = 30;
    target.sample_count = 50;
    target.last_observed_at = 1000;
    target.hedging_ratio = 0.05f;
    target.question_ratio = 0.05f;
    target.imperative_ratio = 0.70f;

    /* set_a: hedge-heavy, longer than target, formal. Should score
     *         lower on the 4-axis metric. */
    const char *set_a[] = {
        "perhaps we could maybe consider possibly doing this if it might work somehow?",
        "I would respectfully suggest that you possibly might want to maybe look into this matter.",
    };
    /* set_b: imperative, short, lowercase, no hedges. Should score
     *         higher on the 4-axis metric. */
    const char *set_b[] = {
        "do it now. ship the fix.",
        "check the logs. fix the bug.",
    };

    hu_communication_style_set_summary_t a;
    hu_communication_style_set_summary_t b;
    float delta = 42.f; /* sentinel that MUST be overwritten */
    hu_error_t e = hu_communication_style_compare_response_sets_v2(
        &target, set_a, NULL, 2, set_b, NULL, 2, &a, &b, &delta);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ(a.scored, (size_t)2);
    HU_ASSERT_EQ(b.scored, (size_t)2);
    HU_ASSERT_TRUE(a.mean >= 0.f && a.mean <= 1.f);
    HU_ASSERT_TRUE(b.mean >= 0.f && b.mean <= 1.f);
    /* delta finite, non-NaN. */
    HU_ASSERT_TRUE(delta == delta); /* NaN check (NaN != NaN). */
    HU_ASSERT_TRUE(isfinite((double)delta));
    /* Sign sanity: imperative direct set should score higher than
     * hedge-heavy formal set against an imperative casual target.
     * delta = mean_b - mean_a > 0. */
    HU_ASSERT_TRUE(b.mean > a.mean);
    HU_ASSERT_TRUE(delta > 0.f);
    /* Argument-validation paths: NULL target / NULL out / zero
     * sample_count target all reject. */
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets_v2(NULL, set_a, NULL, 2, set_b, NULL,
                                                                 2, &a, &b, &delta),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets_v2(&target, set_a, NULL, 2, set_b,
                                                                 NULL, 2, NULL, &b, &delta),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets_v2(&target, set_a, NULL, 2, set_b,
                                                                 NULL, 2, &a, NULL, &delta),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets_v2(&target, set_a, NULL, 2, set_b,
                                                                 NULL, 2, &a, &b, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    hu_communication_style_t empty;
    memset(&empty, 0, sizeof(empty));
    HU_ASSERT_EQ(hu_communication_style_compare_response_sets_v2(&empty, set_a, NULL, 2, set_b,
                                                                 NULL, 2, &a, &b, &delta),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_personal_model_fidelity_v2_tests(void) {
    HU_TEST_SUITE("personal-model-fidelity-v2");
    HU_RUN_TEST(test_fidelity_v2_returns_higher_score_for_matching_decision_style);
    HU_RUN_TEST(test_fidelity_v2_decision_style_neutral_when_target_axes_absent);
    HU_RUN_TEST(test_fidelity_v2_returns_neg_one_on_invalid_inputs);
    HU_RUN_TEST(test_fidelity_v1_unchanged_after_v2_lands);
    HU_RUN_TEST(test_fidelity_v2_compare_response_sets_returns_finite_delta);
}
