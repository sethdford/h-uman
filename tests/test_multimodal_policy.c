/* Tests for L4 multimodal policy — C port of scripts/multimodal_policy.py
 * golden cases. Each test pins one rule firing (or NOT firing) so that
 * future maintainers see the exact intent. */
#include "human/agent/multimodal_policy.h"
#include "test_framework.h"
#include <string.h>

static hu_mm_decision_t decide(const char *s) {
    hu_mm_decision_t d = {0};
    hu_multimodal_decide(s, s ? strlen(s) : 0, &d);
    return d;
}

/* ── Acknowledgment shape — short "k"/"ok"/"cool got it" → tapback like ── */

static void mm_ack_k_routes_to_tapback_like(void) {
    hu_mm_decision_t d = decide("k");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TAPBACK);
    HU_ASSERT_EQ((int)d.tapback_kind, (int)HU_MM_TAPBACK_LIKE);
}

static void mm_ack_ok_routes_to_tapback_like(void) {
    HU_ASSERT_EQ((int)decide("ok").modality, (int)HU_MM_MODALITY_TAPBACK);
}

static void mm_ack_cool_routes_to_tapback_like(void) {
    HU_ASSERT_EQ((int)decide("cool").modality, (int)HU_MM_MODALITY_TAPBACK);
}

/* ── Explicit question — text only, never tapback ── */

static void mm_question_trailing_routes_to_text(void) {
    hu_mm_decision_t d = decide("you free saturday?");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TEXT);
    HU_ASSERT_STR_EQ(d.reason, "explicit-question");
}

/* ── Embedded laughter — text (build on the joke), not tapback ── */

static void mm_laughter_embedded_routes_to_text(void) {
    hu_mm_decision_t d = decide("haha that's wild");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TEXT);
}

/* ── Bare laughter — tapback laugh, never echo ── */

static void mm_bare_laughter_lol_routes_to_tapback_laugh(void) {
    hu_mm_decision_t d = decide("lol");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TAPBACK);
    HU_ASSERT_EQ((int)d.tapback_kind, (int)HU_MM_TAPBACK_LAUGH);
}

static void mm_bare_laughter_lmao_routes_to_tapback_laugh(void) {
    HU_ASSERT_EQ((int)decide("lmao").tapback_kind, (int)HU_MM_TAPBACK_LAUGH);
}

static void mm_bare_laughter_haha_with_trailing_punct(void) {
    HU_ASSERT_EQ((int)decide("haha!").tapback_kind, (int)HU_MM_TAPBACK_LAUGH);
}

/* ── Deep emotional content → voice ── */

static void mm_love_you_routes_to_voice(void) {
    hu_mm_decision_t d = decide("love you so much");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_VOICE);
}

/* ── Short appreciative incoming with love/proud → tapback love ── */

static void mm_proud_of_you_routes_to_tapback_love(void) {
    hu_mm_decision_t d = decide("I'm so proud of you");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TAPBACK);
    HU_ASSERT_EQ((int)d.tapback_kind, (int)HU_MM_TAPBACK_LOVE);
}

/* ── Hyped / celebratory → GIF ── */

static void mm_lfg_routes_to_gif(void) {
    hu_mm_decision_t d = decide("lfg let's go!!!");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_GIF);
}

/* ── Short emphatic → tapback emphasize ── */

static void mm_thats_insane_routes_to_tapback_emphasize(void) {
    hu_mm_decision_t d = decide("that's insane");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TAPBACK);
    HU_ASSERT_EQ((int)d.tapback_kind, (int)HU_MM_TAPBACK_EMPHASIZE);
}

/* ── Generic greeting → text ── */

static void mm_hey_whats_up_routes_to_text(void) {
    HU_ASSERT_EQ((int)decide("hey what's up").modality, (int)HU_MM_MODALITY_TEXT);
}

/* ── Empty string → text default, conf 1.0 ── */

static void mm_empty_routes_to_text(void) {
    hu_mm_decision_t d = decide("");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TEXT);
    HU_ASSERT_STR_EQ(d.reason, "empty-incoming");
}

/* ── Appreciation — added in v2 (was 0% tapback before) ── */

static void mm_thanks_mate_routes_to_tapback_love(void) {
    hu_mm_decision_t d = decide("thanks mate, that helped");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TAPBACK);
    HU_ASSERT_EQ((int)d.tapback_kind, (int)HU_MM_TAPBACK_LOVE);
}

static void mm_ty_routes_to_tapback_love(void) {
    hu_mm_decision_t d = decide("ty");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TAPBACK);
    HU_ASSERT_EQ((int)d.tapback_kind, (int)HU_MM_TAPBACK_LOVE);
}

/* ── Logistics arrival — added in v2 ── */

static void mm_omw_routes_to_tapback_like(void) {
    hu_mm_decision_t d = decide("omw, 8 min");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TAPBACK);
    HU_ASSERT_EQ((int)d.tapback_kind, (int)HU_MM_TAPBACK_LIKE);
}

static void mm_be_there_in_5_routes_to_tapback_like(void) {
    HU_ASSERT_EQ((int)decide("be there in 5").modality, (int)HU_MM_MODALITY_TAPBACK);
}

/* ── Venting — added in v2 ── */

static void mm_ugh_worst_day_routes_to_tapback_emphasize(void) {
    hu_mm_decision_t d = decide("ugh worst day ever");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TAPBACK);
    HU_ASSERT_EQ((int)d.tapback_kind, (int)HU_MM_TAPBACK_EMPHASIZE);
}

static void mm_fml_routes_to_tapback_emphasize(void) {
    HU_ASSERT_EQ((int)decide("fml").tapback_kind, (int)HU_MM_TAPBACK_EMPHASIZE);
}

/* ── Substring-classifier-pitfalls regression: word-boundary matching
 * must NOT trip on similar-prefix words ── */

static void mm_lukewarm_does_not_trigger_warm(void) {
    /* "lukewarm" contains "warm" but means cool, not affectionate.
     * If we ever add a "warm" rule that returns tapback love, this
     * test will catch the substring-misfire. Currently no "warm"
     * rule exists so this just confirms default text. */
    hu_mm_decision_t d = decide("the response was lukewarm");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TEXT);
}

static void mm_unfriendly_does_not_trigger_friend(void) {
    /* "unfriendly" → distant; "friend" → close. If we ever add a
     * "friend" rule, word-boundary matching must reject "unfriendly". */
    hu_mm_decision_t d = decide("she seemed unfriendly today");
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TEXT);
}

/* ── NULL safety ── */

static void mm_null_incoming_safe(void) {
    hu_mm_decision_t d = {0};
    HU_ASSERT_EQ((int)hu_multimodal_decide(NULL, 0, &d), (int)HU_OK);
    HU_ASSERT_EQ((int)d.modality, (int)HU_MM_MODALITY_TEXT);
}

static void mm_null_out_returns_invalid(void) {
    HU_ASSERT_EQ((int)hu_multimodal_decide("hey", 3, NULL), (int)HU_ERR_INVALID_ARGUMENT);
}

void run_multimodal_policy_tests(void) {
    HU_TEST_SUITE("multimodal_policy");

    HU_RUN_TEST(mm_ack_k_routes_to_tapback_like);
    HU_RUN_TEST(mm_ack_ok_routes_to_tapback_like);
    HU_RUN_TEST(mm_ack_cool_routes_to_tapback_like);
    HU_RUN_TEST(mm_question_trailing_routes_to_text);
    HU_RUN_TEST(mm_laughter_embedded_routes_to_text);
    HU_RUN_TEST(mm_bare_laughter_lol_routes_to_tapback_laugh);
    HU_RUN_TEST(mm_bare_laughter_lmao_routes_to_tapback_laugh);
    HU_RUN_TEST(mm_bare_laughter_haha_with_trailing_punct);
    HU_RUN_TEST(mm_love_you_routes_to_voice);
    HU_RUN_TEST(mm_proud_of_you_routes_to_tapback_love);
    HU_RUN_TEST(mm_lfg_routes_to_gif);
    HU_RUN_TEST(mm_thats_insane_routes_to_tapback_emphasize);
    HU_RUN_TEST(mm_hey_whats_up_routes_to_text);
    HU_RUN_TEST(mm_empty_routes_to_text);
    HU_RUN_TEST(mm_thanks_mate_routes_to_tapback_love);
    HU_RUN_TEST(mm_ty_routes_to_tapback_love);
    HU_RUN_TEST(mm_omw_routes_to_tapback_like);
    HU_RUN_TEST(mm_be_there_in_5_routes_to_tapback_like);
    HU_RUN_TEST(mm_ugh_worst_day_routes_to_tapback_emphasize);
    HU_RUN_TEST(mm_fml_routes_to_tapback_emphasize);
    HU_RUN_TEST(mm_lukewarm_does_not_trigger_warm);
    HU_RUN_TEST(mm_unfriendly_does_not_trigger_friend);
    HU_RUN_TEST(mm_null_incoming_safe);
    HU_RUN_TEST(mm_null_out_returns_invalid);
}
