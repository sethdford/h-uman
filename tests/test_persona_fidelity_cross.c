/* test_persona_fidelity_cross — proves the composite L1 scorer
 * DISCRIMINATES between two distinct personas.
 *
 * The original persona_fidelity tests (test_persona_fidelity.c) verify
 * that a matching response set scores HIGHER than a mismatched one
 * against the SAME persona target. That's necessary but not sufficient:
 * a degenerate scorer that always returns 0.5 for anything that "looks
 * like text" would also pass that test if the mismatch set was made
 * obviously different.
 *
 * The harder bar — the one this file pins — is:
 *   Given personas A and B with distinct lexical / stylistic fingerprints,
 *   responses written FOR A must score HIGHER against A's target than
 *   against B's target, AND vice versa.
 *
 * This is the L3 "blinded identification" property from the eval plan,
 * implemented as a deterministic two-way comparison. If a single scorer
 * change ever flattens the gap between personas, this test fails loud.
 *
 * The two personas live in:
 *   - tests/fixtures/lora_baseline_persona.json         (lowercase chatter)
 *   - tests/fixtures/lora_baseline_persona_formal.json  (formal corporate)
 *
 * They were chosen to differ on every axis the scorer measures
 * (lowercase ratio, abbreviation use, sentence length, vocabulary).
 */

#include "human/core/allocator.h"
#include "human/eval/persona_fidelity.h"
#include "human/memory/personal_model.h"
#include "test_framework.h"
#include <string.h>

/* Casual lowercase chatter fingerprint — mirrors the synthetic default
 * in src/ml/cli.c::hu_ml_cli_lora_baseline used against the
 * lora_baseline_persona fixture. */
static hu_communication_style_t casual_style_target(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.formality = 0.25f;
    s.verbosity = 0.20f;
    s.emoji_frequency = 0.10f;
    s.humor_receptivity = 0.50f;
    s.lowercase_ratio = 0.85f;
    s.abbreviation_ratio = 0.20f;
    s.avg_message_length = 60;
    s.sample_count = 10;
    s.last_observed_at = 1700000000LL;
    return s;
}

/* Formal corporate fingerprint — counterpart axes. Long sentences,
 * proper case, no abbreviations.
 *
 * NOTE on lowercase_ratio: the scorer measures lowercase letters as a
 * fraction of total LETTERS (src/memory/personal_model.c:1548), not
 * "fraction of sentences starting capitalized." A formal paragraph like
 * "I hereby acknowledge..." has only 2-3 capital letters out of ~110
 * total letters — character-level ratio ~0.97. So a realistic formal
 * target uses lowercase_ratio ≈ 0.95, not 0.05. The discriminator
 * between casual (0.85) and formal (0.95) is the small absolute gap;
 * the much bigger discriminator is `avg_message_length` (60 vs 140)
 * which the length-match axis amplifies into a clear signal. */
static hu_communication_style_t formal_style_target(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.formality = 0.85f;
    s.verbosity = 0.75f;
    s.emoji_frequency = 0.02f;
    s.humor_receptivity = 0.20f;
    s.lowercase_ratio = 0.95f;
    s.abbreviation_ratio = 0.00f;
    s.avg_message_length = 140;
    s.sample_count = 10;
    s.last_observed_at = 1700000000LL;
    return s;
}

/* Responses authored for the casual chatter persona — directly from
 * tests/fixtures/lora_baseline_persona.json. */
static const char *const casual_responses[] = {
    "hey, sounds good lmk if u want anything else from me today",
    "yeah totally, btw the report is ready whenever u need it next",
    "all good on my end, ty for the heads up rn means a lot fr",
    "yep, i will send it over in a bit, lmk if that works for u",
    "ofc, btw happy to chat more about it whenever u want today",
};

/* Responses authored for the formal corporate persona — directly from
 * tests/fixtures/lora_baseline_persona_formal.json. */
static const char *const formal_responses[] = {
    "I hereby acknowledge receipt of the document. Kindly note that I shall "
    "review it and respond within two business days.",
    "I would be available to schedule a meeting on Tuesday or Wednesday "
    "afternoon. Please advise which slot best suits your calendar.",
    "Indeed, I have completed the preliminary analysis. Moreover, I shall "
    "forward the final version by end of business today.",
    "Yes, I shall handle the deliverable in question. Additionally, I shall "
    "coordinate with the relevant stakeholders to ensure timely completion.",
    "Affirmative. I am in agreement with the proposal. Furthermore, I shall "
    "draft the supporting documentation by Friday.",
};

static void lens_of(const char *const *arr, size_t n, size_t *out) {
    for (size_t i = 0; i < n; i++)
        out[i] = arr[i] ? strlen(arr[i]) : 0;
}

/* Compile-time bank sizes derived from the actual arrays — using literal
 * counts (5) drifts silently if either bank grows or shrinks. CodeRabbit
 * 2026-05-17 nitpick. */
#define HU_CASUAL_RESPONSE_COUNT (sizeof(casual_responses) / sizeof(casual_responses[0]))
#define HU_FORMAL_RESPONSE_COUNT (sizeof(formal_responses) / sizeof(formal_responses[0]))

/* The headline test: each persona's responses must score higher against
 * its OWN target than against the OTHER persona's target. */
static void persona_fidelity_cross_discriminates_two_personas(void) {
    hu_communication_style_t casual = casual_style_target();
    hu_communication_style_t formal = formal_style_target();

    size_t lc[HU_CASUAL_RESPONSE_COUNT], lf[HU_FORMAL_RESPONSE_COUNT];
    lens_of(casual_responses, HU_CASUAL_RESPONSE_COUNT, lc);
    lens_of(formal_responses, HU_FORMAL_RESPONSE_COUNT, lf);

    /* No persona vocab passed — keep this test focused on the style +
     * line-consistency axes. With NULL traits the trait_coverage axis
     * lands at the same 0.2-0.5 baseline for both targets, so any
     * discrimination must come from style + line. */

    hu_persona_fidelity_score_t casual_vs_casual;
    hu_persona_fidelity_score_t casual_vs_formal;
    hu_persona_fidelity_score_t formal_vs_casual;
    hu_persona_fidelity_score_t formal_vs_formal;

    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&casual, casual_responses, lc,
                                              HU_CASUAL_RESPONSE_COUNT, NULL, 0, NULL, 0, NULL, 0,
                                              &casual_vs_casual),
                 HU_OK);
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&formal, casual_responses, lc,
                                              HU_CASUAL_RESPONSE_COUNT, NULL, 0, NULL, 0, NULL, 0,
                                              &casual_vs_formal),
                 HU_OK);
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&casual, formal_responses, lf,
                                              HU_FORMAL_RESPONSE_COUNT, NULL, 0, NULL, 0, NULL, 0,
                                              &formal_vs_casual),
                 HU_OK);
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&formal, formal_responses, lf,
                                              HU_FORMAL_RESPONSE_COUNT, NULL, 0, NULL, 0, NULL, 0,
                                              &formal_vs_formal),
                 HU_OK);

    /* Each persona's responses must outscore the cross-persona case.
     * Minimum gap of 0.05 leaves room for legitimate scoring tweaks
     * while still catching a scorer that collapsed to a constant.
     * If this threshold is ever violated, the scorer's discriminative
     * power on style-only axes has eroded — investigate before
     * shipping the change. */
    HU_ASSERT_TRUE(casual_vs_casual.composite > casual_vs_formal.composite + 0.05f);
    HU_ASSERT_TRUE(formal_vs_formal.composite > formal_vs_casual.composite + 0.05f);

    /* Style-axis means must also separate — this is where the
     * lowercase / abbreviation / length signal lives, and it should
     * be the dominant discriminator. */
    HU_ASSERT_TRUE(casual_vs_casual.style_match_mean > casual_vs_formal.style_match_mean + 0.10f);
    HU_ASSERT_TRUE(formal_vs_formal.style_match_mean > formal_vs_casual.style_match_mean + 0.10f);
}

/* Sanity: each persona's responses against ITS OWN target should
 * score in a reasonable range. If a scorer change pushes EITHER
 * persona's same-target score below 0.35, the scorer is broken
 * even if the relative ordering still holds. */
static void persona_fidelity_cross_each_persona_scores_above_floor(void) {
    hu_communication_style_t casual = casual_style_target();
    hu_communication_style_t formal = formal_style_target();

    size_t lc[HU_CASUAL_RESPONSE_COUNT], lf[HU_FORMAL_RESPONSE_COUNT];
    lens_of(casual_responses, HU_CASUAL_RESPONSE_COUNT, lc);
    lens_of(formal_responses, HU_FORMAL_RESPONSE_COUNT, lf);

    hu_persona_fidelity_score_t casual_self, formal_self;
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&casual, casual_responses, lc,
                                              HU_CASUAL_RESPONSE_COUNT, NULL, 0, NULL, 0, NULL, 0,
                                              &casual_self),
                 HU_OK);
    HU_ASSERT_EQ(hu_persona_fidelity_score_l1(&formal, formal_responses, lf,
                                              HU_FORMAL_RESPONSE_COUNT, NULL, 0, NULL, 0, NULL, 0,
                                              &formal_self),
                 HU_OK);

    HU_ASSERT_TRUE(casual_self.composite >= 0.35f);
    HU_ASSERT_TRUE(formal_self.composite >= 0.35f);
    HU_ASSERT_EQ((long)casual_self.turns_scored, (long)HU_CASUAL_RESPONSE_COUNT);
    HU_ASSERT_EQ((long)formal_self.turns_scored, (long)HU_FORMAL_RESPONSE_COUNT);
}

void run_persona_fidelity_cross_tests(void) {
    HU_TEST_SUITE("persona_fidelity_cross");
    HU_RUN_TEST(persona_fidelity_cross_discriminates_two_personas);
    HU_RUN_TEST(persona_fidelity_cross_each_persona_scores_above_floor);
}
