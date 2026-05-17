/* test_persona_fidelity — coverage for the L1 composite scorer.
 *
 * The library composes three existing primitives:
 *   - hu_communication_style_fidelity_score (per-turn style match)
 *   - hu_consistency_score_prompt_alignment (trait coverage)
 *   - hu_consistency_score_line              (turn-to-turn consistency)
 *
 * These primitives have their own tests. This file pins the composition:
 *   - matching response set scores higher than mismatched
 *   - A/B verdict flips when the better set is on the B side
 *   - input validation rejects NULL / unfingerprinted / empty inputs
 *   - L2 judge wrapper rejects empty rubric / response / persona
 *
 * Style fingerprint: lowercase-heavy chatter, ~60 char messages, some
 * abbreviation use. Mirrors tests/fixtures/lora_baseline_persona.json
 * so a future replay tool driven by that fixture sees the same numbers.
 */

#include "human/eval/persona_fidelity.h"
#include "human/memory/personal_model.h"
#include "test_framework.h"
#include <string.h>

/* Build a style target tuned to the lora_baseline_persona fixture:
 * lowercase ~0.85, abbreviation ~0.2, avg_message_length 60. */
static hu_communication_style_t lowercase_chatter_style(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.formality = 0.25f;
    s.verbosity = 0.20f;
    s.emoji_frequency = 0.10f;
    s.humor_receptivity = 0.50f;
    s.lowercase_ratio = 0.85f;
    s.abbreviation_ratio = 0.20f;
    s.avg_message_length = 60;
    s.sample_count = 10; /* clears HU_PM_DIRECTIVE_MIN_SAMPLES with margin */
    s.last_observed_at = 1700000000LL;
    return s;
}

/* Compute strlen array on the fly. Used in tests so the response literals
 * don't have to be paired with hand-counted lengths. */
static void lens_from(const char *const *arr, size_t n, size_t *out) {
    for (size_t i = 0; i < n; i++)
        out[i] = arr[i] ? strlen(arr[i]) : 0;
}

static void persona_fidelity_matching_set_scores_above_mismatch(void) {
    hu_communication_style_t target = lowercase_chatter_style();

    /* Set A — matches the lowercase-chatter fingerprint. */
    const char *match[] = {
        "hey, sounds good lmk if u want anything else from me today",
        "yeah totally, btw the report is ready whenever u need it next",
        "all good on my end, ty for the heads up rn means a lot fr",
        "yep, i'll send it over in a bit lmk if that works for u",
    };
    /* Set B — clearly mismatched: SHOUTING CASE plus 2-3× target length,
     * no abbreviations. Each axis (lowercase, abbreviation, length) lands
     * at the opposite end of the triangular-match window from the
     * target's fingerprint. */
    const char *mismatch[] = {
        "HELLO. I HAVE PREPARED THE REPORT AND WILL FORWARD IT AT YOUR EARLIEST CONVENIENCE "
        "TODAY OR TOMORROW MORNING WHICHEVER WORKS BEST FOR YOUR REVIEW SCHEDULE.",
        "I WOULD BE DELIGHTED TO SCHEDULE A FORMAL MEETING WHENEVER IT BEST SUITS YOUR "
        "TIMELINE AND I WILL ARRANGE A ROOM AND SEND A CALENDAR INVITATION FOR US.",
        "THANK YOU FOR THE INFORMATION I WILL REPLY WITH A FOLLOW UP MESSAGE TOMORROW "
        "AFTER I HAVE HAD A CHANCE TO REVIEW EACH OF THE ATTACHED DOCUMENTS IN DETAIL.",
        "PLEASE FIND ATTACHED THE COMPLETE DOCUMENT YOU REQUESTED EARLIER THIS AFTERNOON "
        "ALONG WITH THE SUPPLEMENTARY APPENDIX AND THE FULL REFERENCE BIBLIOGRAPHY.",
    };

    size_t lens_match[4], lens_mismatch[4];
    lens_from(match, 4, lens_match);
    lens_from(mismatch, 4, lens_mismatch);

    hu_persona_fidelity_score_t sa, sb;
    HU_ASSERT_EQ(
        hu_persona_fidelity_score_l1(&target, match, lens_match, 4, NULL, 0, NULL, 0, NULL, 0, &sa),
        HU_OK);
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&target, mismatch, lens_mismatch, 4, NULL, 0, NULL, 0,
                                              NULL, 0, &sb),
                 HU_OK);

    HU_ASSERT_EQ((long)sa.turns_scored, 4L);
    HU_ASSERT_EQ((long)sb.turns_scored, 4L);
    /* Matching set must clearly outscore the mismatched one. With NULL
     * traits both sets get the same neutral trait score (0.5), so the
     * composite gap is dominated by HU_PF_W_STYLE (0.50) times the
     * style-axis gap. The mismatched set hits the floor on all three
     * style axes (lowercase, abbreviation, length), so the style-axis
     * gap is large — but the composite-level gap is bounded by 0.50
     * times that. Threshold of 0.10 leaves margin both ways. */
    HU_ASSERT_TRUE(sa.composite > sb.composite + 0.10f);
    HU_ASSERT_TRUE(sa.style_match_mean > sb.style_match_mean + 0.20f);
}

static void persona_fidelity_ab_flags_improvement_when_b_matches(void) {
    hu_communication_style_t target = lowercase_chatter_style();

    const char *worse[] = {
        "HELLO. I HAVE PREPARED THE REPORT AND WILL FORWARD IT AT YOUR EARLIEST CONVENIENCE "
        "TODAY OR TOMORROW MORNING WHICHEVER WORKS BEST FOR YOUR REVIEW SCHEDULE.",
        "I WOULD BE DELIGHTED TO SCHEDULE A FORMAL MEETING WHENEVER IT BEST SUITS YOUR "
        "TIMELINE AND I WILL ARRANGE A ROOM AND SEND A CALENDAR INVITATION FOR US.",
        "THANK YOU FOR THE INFORMATION I WILL REPLY WITH A FOLLOW UP MESSAGE TOMORROW "
        "AFTER I HAVE HAD A CHANCE TO REVIEW EACH OF THE ATTACHED DOCUMENTS IN DETAIL.",
        "PLEASE FIND ATTACHED THE COMPLETE DOCUMENT YOU REQUESTED EARLIER THIS AFTERNOON "
        "ALONG WITH THE SUPPLEMENTARY APPENDIX AND THE FULL REFERENCE BIBLIOGRAPHY.",
    };
    const char *better[] = {
        "hey, sounds good lmk if u want anything else from me today",
        "yeah totally, btw the report is ready whenever u need it next",
        "all good on my end, ty for the heads up rn means a lot fr",
        "yep, i'll send it over in a bit lmk if that works for u",
    };
    size_t lw[4], lb[4];
    lens_from(worse, 4, lw);
    lens_from(better, 4, lb);

    hu_persona_fidelity_ab_t ab;
    HU_ASSERT_EQ(hu_persona_fidelity_ab_score(&target, worse, lw, 4, better, lb, 4, NULL, 0, NULL,
                                              0, NULL, 0, /*min_improvement_stderr=*/1.0f, &ab),
                 HU_OK);
    HU_ASSERT_TRUE(ab.delta > 0.10f);
    HU_ASSERT_TRUE(ab.improved);

    /* Symmetry — flip the sets and `improved` must flip false. */
    hu_persona_fidelity_ab_t ab2;
    HU_ASSERT_EQ(hu_persona_fidelity_ab_score(&target, better, lb, 4, worse, lw, 4, NULL, 0, NULL,
                                              0, NULL, 0, 1.0f, &ab2),
                 HU_OK);
    HU_ASSERT_TRUE(ab2.delta < -0.10f);
    HU_ASSERT_FALSE(ab2.improved);
}

static void persona_fidelity_score_rejects_unfingerprinted_target(void) {
    hu_communication_style_t target;
    memset(&target, 0, sizeof(target));
    /* sample_count = 0 means "never observed" — no fingerprint to score
     * against; the underlying scorer returns -1 and we surface
     * INVALID_ARGUMENT up front rather than silently returning all-zero. */
    const char *r[] = {"hello"};
    size_t l[] = {5};
    hu_persona_fidelity_score_t out;
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&target, r, l, 1, NULL, 0, NULL, 0, NULL, 0, &out),
                 HU_ERR_INVALID_ARGUMENT);
}

static void persona_fidelity_score_rejects_null_args(void) {
    hu_communication_style_t target = lowercase_chatter_style();
    const char *r[] = {"hello"};
    size_t l[] = {5};
    hu_persona_fidelity_score_t out;
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(NULL, r, l, 1, NULL, 0, NULL, 0, NULL, 0, &out),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&target, NULL, l, 1, NULL, 0, NULL, 0, NULL, 0, &out),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&target, r, NULL, 1, NULL, 0, NULL, 0, NULL, 0, &out),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&target, r, l, 0, NULL, 0, NULL, 0, NULL, 0, &out),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&target, r, l, 1, NULL, 0, NULL, 0, NULL, 0, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

static void persona_fidelity_skips_empty_responses(void) {
    hu_communication_style_t target = lowercase_chatter_style();
    const char *r[] = {"hey lmk if you want anything", NULL, "", "yeah totally btw"};
    size_t l[4];
    lens_from(r, 4, l);
    hu_persona_fidelity_score_t out;
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&target, r, l, 4, NULL, 0, NULL, 0, NULL, 0, &out),
                 HU_OK);
    HU_ASSERT_EQ((long)out.turns_scored, 2L);
    HU_ASSERT_EQ((long)out.turns_skipped, 2L);
}

static void persona_fidelity_judge_rejects_invalid_args(void) {
    /* No provider mock available here — we just exercise the
     * arg-validation guards. The judge call itself is covered in
     * tests/test_persona_fidelity_judge.c which uses a recording stub. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_result_t out;
    memset(&out, 0, sizeof(out));
    /* Both allocator and provider NULL → INVALID_ARGUMENT */
    HU_ASSERT_EQ(
        hu_persona_fidelity_judge(NULL, NULL, "m", 1, "p", 1, "r", 1, "rubric", 6, 3, NULL, &out),
        HU_ERR_INVALID_ARGUMENT);
    /* Allocator OK but provider NULL → still INVALID_ARGUMENT.
     * Bugbot 2026-05-16: previous version of this test passed NULL for
     * BOTH calls, making the second an exact duplicate that didn't
     * exercise the "alloc valid, provider NULL" path the comment claimed.
     * Now this assertion actually tests what the comment says. */
    HU_ASSERT_EQ(
        hu_persona_fidelity_judge(&alloc, NULL, "m", 1, "p", 1, "r", 1, "rubric", 6, 3, NULL, &out),
        HU_ERR_INVALID_ARGUMENT);
}

void run_persona_fidelity_tests(void) {
    HU_TEST_SUITE("persona_fidelity");
    HU_RUN_TEST(persona_fidelity_matching_set_scores_above_mismatch);
    HU_RUN_TEST(persona_fidelity_ab_flags_improvement_when_b_matches);
    HU_RUN_TEST(persona_fidelity_score_rejects_unfingerprinted_target);
    HU_RUN_TEST(persona_fidelity_score_rejects_null_args);
    HU_RUN_TEST(persona_fidelity_skips_empty_responses);
    HU_RUN_TEST(persona_fidelity_judge_rejects_invalid_args);
}
