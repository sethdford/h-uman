#include "human/channels/imessage_action.h"
#include "test_framework.h"

static hu_reply_style_facts_t neutral_facts(void) {
    hu_reply_style_facts_t f = {0};
    f.persona_thread_affinity = 0.3f;
    f.persona_formality = 0.5f;
    f.conv_density_msgs_per_min = 2.0f;
    f.parent_emotional_intensity = HU_EMOTION_THRESHOLD_LOW;
    return f;
}

static void enum_values_are_stable(void) {
    HU_ASSERT_EQ((int)HU_REPLY_STYLE_FLAT, 0);
    HU_ASSERT_EQ((int)HU_REPLY_STYLE_THREADED, 1);
    HU_ASSERT_EQ((int)HU_REPLY_STYLE_TAPBACK, 2);
    HU_ASSERT_EQ((int)HU_REPLY_STYLE_TAPBACK_PLUS_FLAT, 3);
}

/* Case 1: fresh inbound, low density → mostly FLAT. */
static void fresh_low_density_scores_low_thread(void) {
    hu_reply_style_facts_t f = neutral_facts();
    f.seconds_since_parent = 5;
    hu_reply_style_scores_t s = hu_imessage_score_reply_style(&f);
    HU_ASSERT(s.p_thread < 0.20f);
    HU_ASSERT(s.p_flat > 0.50f);
}

/* Case 2: stale (sec=900), 1 pending Q → THREADED high prob. */
static void stale_with_pending_question_scores_high_thread(void) {
    hu_reply_style_facts_t f = neutral_facts();
    f.seconds_since_parent = 900;
    f.pending_questions_in_window = 1;
    hu_reply_style_scores_t s = hu_imessage_score_reply_style(&f);
    HU_ASSERT(s.p_thread > 0.50f);
}

/* Case 3: rapid-fire (density=15) → FLAT high prob. */
static void rapid_fire_density_scores_low_thread(void) {
    hu_reply_style_facts_t f = neutral_facts();
    f.conv_density_msgs_per_min = 15.0f;
    hu_reply_style_scores_t s = hu_imessage_score_reply_style(&f);
    HU_ASSERT(s.p_thread < 0.15f);
}

/* Case 4: 3 other-threaded recent → THREAD nudge (compare two facts,
 * mirror count differs; thread prob strictly higher). */
static void other_threaded_nudges_thread_probability(void) {
    hu_reply_style_facts_t f0 = neutral_facts();
    f0.other_threaded_replies_recent = 0;
    hu_reply_style_scores_t s0 = hu_imessage_score_reply_style(&f0);

    hu_reply_style_facts_t f3 = neutral_facts();
    f3.other_threaded_replies_recent = 3;
    hu_reply_style_scores_t s3 = hu_imessage_score_reply_style(&f3);

    HU_ASSERT(s3.p_thread > s0.p_thread);
}

/* Case 5: parent was Q + persona_thread_affinity=0.6 → THREADED majority. */
static void parent_question_with_high_affinity_prefers_thread(void) {
    hu_reply_style_facts_t f = neutral_facts();
    f.parent_was_a_question = true;
    f.persona_thread_affinity = 0.6f;
    int count = 0;
    for (uint64_t seed = 1; seed <= 100; seed++) {
        hu_reply_style_t s = hu_imessage_choose_reply_style(&f, seed);
        if (s == HU_REPLY_STYLE_THREADED)
            count++;
    }
    HU_ASSERT(count > 50);
}

/* Case 6: emotional_intensity=HIGH + density=2 → NEVER TAPBACK solo. */
static void emotional_high_never_tapback_solo(void) {
    hu_reply_style_facts_t f = neutral_facts();
    f.parent_emotional_intensity = HU_EMOTION_THRESHOLD_HIGH;
    f.conv_density_msgs_per_min = 2.0f;
    for (uint64_t seed = 1; seed <= 200; seed++) {
        hu_reply_style_t s = hu_imessage_choose_reply_style(&f, seed);
        HU_ASSERT(s != HU_REPLY_STYLE_TAPBACK);
    }
}

/* Case 7: emotional_intensity=HIGH → TAPBACK_PLUS_FLAT possible. */
static void emotional_high_enables_tapback_plus_flat(void) {
    hu_reply_style_facts_t f = neutral_facts();
    f.parent_emotional_intensity = HU_EMOTION_THRESHOLD_HIGH;
    int count = 0;
    for (uint64_t seed = 1; seed <= 200; seed++) {
        hu_reply_style_t s = hu_imessage_choose_reply_style(&f, seed);
        if (s == HU_REPLY_STYLE_TAPBACK_PLUS_FLAT)
            count++;
    }
    HU_ASSERT(count >= 1);
}

/* Case 8: parent_position=10 + sec=300 → THREADED high prob. */
static void deep_position_stale_parent_prefers_thread(void) {
    hu_reply_style_facts_t f = neutral_facts();
    f.parent_position_from_bottom = 10;
    f.seconds_since_parent = 300;
    hu_reply_style_scores_t s = hu_imessage_score_reply_style(&f);
    HU_ASSERT(s.p_thread > 0.4f);
}

/* Case 9: persona_thread_affinity=0.05 → THREADED rarely. */
static void low_thread_affinity_discourages_thread(void) {
    hu_reply_style_facts_t f = neutral_facts();
    f.persona_thread_affinity = 0.05f;
    int count = 0;
    for (uint64_t seed = 1; seed <= 100; seed++) {
        hu_reply_style_t s = hu_imessage_choose_reply_style(&f, seed);
        if (s == HU_REPLY_STYLE_THREADED)
            count++;
    }
    HU_ASSERT(count < 15);
}

/* Case 10: persona_thread_affinity=0.9 → THREADED majority. */
static void high_thread_affinity_encourages_thread(void) {
    hu_reply_style_facts_t f = neutral_facts();
    f.persona_thread_affinity = 0.9f;
    int count = 0;
    for (uint64_t seed = 1; seed <= 100; seed++) {
        hu_reply_style_t s = hu_imessage_choose_reply_style(&f, seed);
        if (s == HU_REPLY_STYLE_THREADED)
            count++;
    }
    HU_ASSERT(count > 60);
}

/* Case 11: mirror=0 + density=4 + sec=30 → FLAT majority. */
static void no_mirror_low_density_fresh_prefers_flat(void) {
    hu_reply_style_facts_t f = neutral_facts();
    f.other_threaded_replies_recent = 0;
    f.conv_density_msgs_per_min = 4.0f;
    f.seconds_since_parent = 30;
    int count = 0;
    for (uint64_t seed = 1; seed <= 100; seed++) {
        hu_reply_style_t s = hu_imessage_choose_reply_style(&f, seed);
        if (s == HU_REPLY_STYLE_FLAT)
            count++;
    }
    HU_ASSERT(count > 50);
}

/* Case 12: formality=1.0 + sec=120 → THREAD nudged UP vs formality=0.0. */
static void high_formality_increases_thread_probability(void) {
    hu_reply_style_facts_t f0 = neutral_facts();
    f0.persona_formality = 0.0f;
    f0.seconds_since_parent = 120;
    hu_reply_style_scores_t s0 = hu_imessage_score_reply_style(&f0);

    hu_reply_style_facts_t f1 = neutral_facts();
    f1.persona_formality = 1.0f;
    f1.seconds_since_parent = 120;
    hu_reply_style_scores_t s1 = hu_imessage_score_reply_style(&f1);

    HU_ASSERT(s1.p_thread > s0.p_thread);
}

void run_imessage_reply_style_tests(void) {
    HU_TEST_SUITE("imessage_reply_style");
    HU_RUN_TEST(enum_values_are_stable);
    HU_RUN_TEST(fresh_low_density_scores_low_thread);
    HU_RUN_TEST(stale_with_pending_question_scores_high_thread);
    HU_RUN_TEST(rapid_fire_density_scores_low_thread);
    HU_RUN_TEST(other_threaded_nudges_thread_probability);
    HU_RUN_TEST(parent_question_with_high_affinity_prefers_thread);
    HU_RUN_TEST(emotional_high_never_tapback_solo);
    HU_RUN_TEST(emotional_high_enables_tapback_plus_flat);
    HU_RUN_TEST(deep_position_stale_parent_prefers_thread);
    HU_RUN_TEST(low_thread_affinity_discourages_thread);
    HU_RUN_TEST(high_thread_affinity_encourages_thread);
    HU_RUN_TEST(no_mirror_low_density_fresh_prefers_flat);
    HU_RUN_TEST(high_formality_increases_thread_probability);
}
