#include "test_framework.h"

#ifdef HU_HAS_IMESSAGE

#include "human/channels/imessage_action_facts.h"
#include <string.h>

/* AC: snapshot fields map 1:1 to facts struct fields. */
static void snapshot_fields_pass_through_to_facts(void) {
    hu_conversation_snapshot_t snap = {
        .parent_seconds_ago = 300,
        .parent_position_from_bottom = 3,
        .parent_is_question = true,
        .parent_emotional_intensity = HU_EMOTION_THRESHOLD_LOW,
        .pending_questions_in_window = 2,
        .other_threaded_replies_recent = 4,
        .our_threaded_replies_recent = 1,
        .conv_density_msgs_per_min = 2.5f,
    };
    hu_persona_t p = {0};
    hu_reply_style_facts_t f = {0};
    hu_imessage_build_reply_facts(&snap, &p, &f);

    HU_ASSERT_EQ((long long)f.seconds_since_parent, 300LL);
    HU_ASSERT_EQ(f.parent_position_from_bottom, 3);
    HU_ASSERT(f.parent_was_a_question == true);
    HU_ASSERT_EQ(f.parent_emotional_intensity, HU_EMOTION_THRESHOLD_LOW);
    HU_ASSERT_EQ(f.pending_questions_in_window, 2);
    HU_ASSERT_EQ(f.other_threaded_replies_recent, 4);
    HU_ASSERT_EQ(f.our_threaded_replies_recent, 1);
    HU_ASSERT((int)(f.conv_density_msgs_per_min * 10) == 25);
}

/* AC: null snapshot zero-fills all snapshot-derived fields (safe default). */
static void null_snapshot_zeros_snapshot_fields(void) {
    hu_persona_t p = {0};
    hu_reply_style_facts_t f = {0};
    hu_imessage_build_reply_facts(NULL, &p, &f);

    HU_ASSERT_EQ((long long)f.seconds_since_parent, 0LL);
    HU_ASSERT_EQ(f.parent_position_from_bottom, 0);
    HU_ASSERT(f.parent_was_a_question == false);
    HU_ASSERT_EQ(f.parent_emotional_intensity, 0);
}

/* AC: null persona uses sensible defaults (formality=0.5, affinity=0.3). */
static void null_persona_uses_defaults(void) {
    hu_conversation_snapshot_t snap = {0};
    hu_reply_style_facts_t f = {0};
    hu_imessage_build_reply_facts(&snap, NULL, &f);

    HU_ASSERT((int)(f.persona_formality * 10) == 5);
    HU_ASSERT((int)(f.persona_thread_affinity * 10) == 3);
}

/* AC: null facts_out is safe (no-op, no crash). */
static void null_facts_out_no_crash(void) {
    hu_conversation_snapshot_t snap = {0};
    hu_persona_t p = {0};
    hu_imessage_build_reply_facts(&snap, &p, NULL);
    /* If we reach here, the function didn't crash. Test passes. */
    HU_ASSERT(1);
}

void run_imessage_action_facts_tests(void) {
    HU_TEST_SUITE("imessage_action_facts");
    HU_RUN_TEST(snapshot_fields_pass_through_to_facts);
    HU_RUN_TEST(null_snapshot_zeros_snapshot_fields);
    HU_RUN_TEST(null_persona_uses_defaults);
    HU_RUN_TEST(null_facts_out_no_crash);
}

#else /* !HU_HAS_IMESSAGE — stub runner. */
void run_imessage_action_facts_tests(void) {
    (void)0;
}
#endif
