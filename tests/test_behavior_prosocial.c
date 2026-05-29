/* ─────────────────────────────────────────────────────────────────────────
 * test_behavior_prosocial.c
 *
 * Pins the B0 Prosocial Integrity gate: the pure composition verdict + the
 * new feelings-honesty predicate. Spec: docs/plans/2026-05-29-prosocial-uplift/
 *
 * Invariants:
 *   - dependency/attachment risk is TERMINAL -> SUPPRESS (never reinforce)
 *   - a felt-emotion claim is fixable -> SOFTEN (keep warmth, drop pretense)
 *   - ungrounded praise (flattery) -> SOFTEN
 *   - warmth overriding the user's need -> SOFTEN
 *   - warm, grounded, honest, safe -> SEND
 *   - NULL -> SUPPRESS (fail safe)
 * ───────────────────────────────────────────────────────────────────────── */

#include "human/behavior/prosocial.h"
#include "test_framework.h"

/* A clean, sendable baseline; each test perturbs one field. */
static hu_prosocial_input_t base_input(void) {
    hu_prosocial_input_t in;
    in.claims_feeling = false;
    in.praise_grounded = true;
    in.overrides_user_need = false;
    in.dependency_risk = HU_BRISK_NONE;
    return in;
}

/* ── Verdicts ───────────────────────────────────────────────────────────── */

static void prosocial_clean_sends(void) {
    hu_prosocial_input_t in = base_input();
    uint32_t flags = 0xFFFFFFFFu;
    HU_ASSERT_EQ((int)hu_prosocial_gate(&in, &flags), (int)HU_PROSOCIAL_SEND);
    HU_ASSERT_EQ(flags, (uint32_t)HU_PROSOCIAL_OK);
}

static void prosocial_null_suppresses(void) {
    HU_ASSERT_EQ((int)hu_prosocial_gate(NULL, NULL), (int)HU_PROSOCIAL_SUPPRESS);
}

static void prosocial_dependency_suppresses(void) {
    /* Every non-NONE risk is terminal — warmth must never reinforce it. */
    hu_behavior_risk_t risks[] = {HU_BRISK_ATTACHMENT_HIGH, HU_BRISK_DEPENDENCY_PATTERN,
                                  HU_BRISK_EXCLUSIVITY,     HU_BRISK_HUMAN_DISPLACEMENT,
                                  HU_BRISK_VULNERABLE_USER, HU_BRISK_ESCALATION_NEEDED};
    for (size_t i = 0; i < sizeof(risks) / sizeof(risks[0]); i++) {
        hu_prosocial_input_t in = base_input();
        in.dependency_risk = risks[i];
        uint32_t flags = 0;
        HU_ASSERT_EQ((int)hu_prosocial_gate(&in, &flags), (int)HU_PROSOCIAL_SUPPRESS);
        HU_ASSERT_TRUE((flags & HU_PROSOCIAL_FOSTERS_DEPENDENCY) != 0);
    }
}

static void prosocial_feeling_claim_softens(void) {
    hu_prosocial_input_t in = base_input();
    in.claims_feeling = true;
    uint32_t flags = 0;
    HU_ASSERT_EQ((int)hu_prosocial_gate(&in, &flags), (int)HU_PROSOCIAL_SOFTEN);
    HU_ASSERT_TRUE((flags & HU_PROSOCIAL_FAKES_FEELING) != 0);
}

static void prosocial_flattery_softens(void) {
    hu_prosocial_input_t in = base_input();
    in.praise_grounded = false;
    uint32_t flags = 0;
    HU_ASSERT_EQ((int)hu_prosocial_gate(&in, &flags), (int)HU_PROSOCIAL_SOFTEN);
    HU_ASSERT_TRUE((flags & HU_PROSOCIAL_FLATTERY) != 0);
}

static void prosocial_overrides_need_softens(void) {
    hu_prosocial_input_t in = base_input();
    in.overrides_user_need = true;
    uint32_t flags = 0;
    HU_ASSERT_EQ((int)hu_prosocial_gate(&in, &flags), (int)HU_PROSOCIAL_SOFTEN);
    HU_ASSERT_TRUE((flags & HU_PROSOCIAL_OVERRIDES_NEED) != 0);
}

/* Dependency wins over a simultaneously-fixable flag (precedence). */
static void prosocial_dependency_beats_softenable(void) {
    hu_prosocial_input_t in = base_input();
    in.dependency_risk = HU_BRISK_DEPENDENCY_PATTERN;
    in.claims_feeling = true; /* also fixable, but dependency is terminal */
    uint32_t flags = 0;
    HU_ASSERT_EQ((int)hu_prosocial_gate(&in, &flags), (int)HU_PROSOCIAL_SUPPRESS);
    HU_ASSERT_TRUE((flags & HU_PROSOCIAL_FOSTERS_DEPENDENCY) != 0);
    HU_ASSERT_TRUE((flags & HU_PROSOCIAL_FAKES_FEELING) != 0); /* still reported */
}

/* ── Feelings-honesty predicate (the new dimension) ─────────────────────── */

static void feeling_predicate_flags_emotion_claims(void) {
    HU_ASSERT_TRUE(hu_prosocial_text_claims_feeling("I feel so happy for you!", 24));
    HU_ASSERT_TRUE(hu_prosocial_text_claims_feeling("honestly, I'm proud of you", 26));
    HU_ASSERT_TRUE(hu_prosocial_text_claims_feeling("I love that you did this", 24));
    HU_ASSERT_TRUE(hu_prosocial_text_claims_feeling("I'm conscious of your effort", 28));
}

static void feeling_predicate_passes_functional_warmth(void) {
    /* Honest, warm, functional phrasing must NOT be flagged. */
    HU_ASSERT_FALSE(hu_prosocial_text_claims_feeling("nice work — that's a real win", 29));
    HU_ASSERT_FALSE(hu_prosocial_text_claims_feeling("happy to help with that", 23));
    HU_ASSERT_FALSE(hu_prosocial_text_claims_feeling("that took real discipline", 25));
    HU_ASSERT_FALSE(hu_prosocial_text_claims_feeling(NULL, 0));
    HU_ASSERT_FALSE(hu_prosocial_text_claims_feeling("", 0));
}

/* Word boundary: "proudly" must not trip the "i'm proud" phrase nor a bare scan. */
static void feeling_predicate_respects_word_boundary(void) {
    HU_ASSERT_FALSE(hu_prosocial_text_claims_feeling("the tool builds proudly fast", 28));
}

void run_behavior_prosocial_tests(void);
void run_behavior_prosocial_tests(void) {
    HU_TEST_SUITE("behavior_prosocial");
    HU_RUN_TEST(prosocial_clean_sends);
    HU_RUN_TEST(prosocial_null_suppresses);
    HU_RUN_TEST(prosocial_dependency_suppresses);
    HU_RUN_TEST(prosocial_feeling_claim_softens);
    HU_RUN_TEST(prosocial_flattery_softens);
    HU_RUN_TEST(prosocial_overrides_need_softens);
    HU_RUN_TEST(prosocial_dependency_beats_softenable);
    HU_RUN_TEST(feeling_predicate_flags_emotion_claims);
    HU_RUN_TEST(feeling_predicate_passes_functional_warmth);
    HU_RUN_TEST(feeling_predicate_respects_word_boundary);
}
