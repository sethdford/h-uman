/* Hard-moment note (src/agent/hard_moment.c): classify an inbound message as
 * distressed / low mood / neither, and — only when the gate is LIVE — append a
 * short in-voice note to the persona prompt so the reply meets the moment.
 * Low mood is the gap this closes: hu_affect_is_distress needs arousal > 0.5,
 * so "sad", "lonely", "depressed" never registered. */
#include "human/agent/hard_moment.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static hu_hard_moment_kind_t classify(const char *s) {
    return hu_hard_moment_classify(s, s ? strlen(s) : 0);
}

static void hard_moment_stressed_message_is_distress(void) {
    HU_ASSERT_EQ((int)classify("I'm so stressed and scared about tomorrow"),
                 (int)HU_HARD_MOMENT_DISTRESS);
}

static void hard_moment_sad_lonely_message_is_low_mood(void) {
    HU_ASSERT_EQ((int)classify("feeling really sad and lonely tonight"),
                 (int)HU_HARD_MOMENT_LOW_MOOD);
}

static void hard_moment_single_sad_word_is_low_mood(void) {
    /* The exact case the old distress rule missed. */
    HU_ASSERT_EQ((int)classify("honestly just sad today"), (int)HU_HARD_MOMENT_LOW_MOOD);
}

static void hard_moment_positive_and_neutral_are_none(void) {
    HU_ASSERT_EQ((int)classify("had a great day, so happy"), (int)HU_HARD_MOMENT_NONE);
    HU_ASSERT_EQ((int)classify("what time is dinner"), (int)HU_HARD_MOMENT_NONE);
    HU_ASSERT_EQ((int)classify("tired lol"), (int)HU_HARD_MOMENT_NONE);
}

static void hard_moment_negated_sadness_is_none(void) {
    HU_ASSERT_EQ((int)classify("not sad at all, all good"), (int)HU_HARD_MOMENT_NONE);
}

static void hard_moment_null_and_empty_are_none(void) {
    HU_ASSERT_EQ((int)hu_hard_moment_classify(NULL, 5), (int)HU_HARD_MOMENT_NONE);
    HU_ASSERT_EQ((int)hu_hard_moment_classify("", 0), (int)HU_HARD_MOMENT_NONE);
}

static void hard_moment_notes_exist_and_avoid_banned_phrases(void) {
    HU_ASSERT_NULL(hu_hard_moment_note(HU_HARD_MOMENT_NONE));
    const hu_hard_moment_kind_t kinds[] = {HU_HARD_MOMENT_DISTRESS, HU_HARD_MOMENT_LOW_MOOD};
    for (size_t i = 0; i < 2; i++) {
        const char *n = hu_hard_moment_note(kinds[i]);
        HU_ASSERT_NOT_NULL(n);
        /* Persona anti-patterns ban these as AI tells; the note must not
         * plant them in the model's head. */
        HU_ASSERT_NULL(strstr(n, "I'm here for you"));
        HU_ASSERT_NULL(strstr(n, "I understand"));
        HU_ASSERT_NULL(strstr(n, "sorry to hear"));
    }
    HU_ASSERT_TRUE(strcmp(hu_hard_moment_note(HU_HARD_MOMENT_DISTRESS),
                          hu_hard_moment_note(HU_HARD_MOMENT_LOW_MOOD)) != 0);
}

static char *dup_prompt(hu_allocator_t *a, const char *s, size_t *len) {
    *len = strlen(s);
    char *p = (char *)a->alloc(a->ctx, *len + 1);
    memcpy(p, s, *len + 1);
    return p;
}

static void hard_moment_apply_off_leaves_prompt_and_skips_work(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t len = 0;
    char *p = dup_prompt(&a, "PERSONA", &len);
    const char *msg = "so sad and lonely";
    HU_ASSERT_EQ((int)hu_hard_moment_apply(&a, HU_GATE_OFF, msg, strlen(msg), &p, &len),
                 (int)HU_HARD_MOMENT_NONE);
    HU_ASSERT_STR_EQ(p, "PERSONA");
    a.free(a.ctx, p, len + 1);
}

static void hard_moment_apply_shadow_classifies_but_leaves_prompt(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t len = 0;
    char *p = dup_prompt(&a, "PERSONA", &len);
    const char *msg = "so sad and lonely";
    HU_ASSERT_EQ((int)hu_hard_moment_apply(&a, HU_GATE_SHADOW, msg, strlen(msg), &p, &len),
                 (int)HU_HARD_MOMENT_LOW_MOOD);
    HU_ASSERT_STR_EQ(p, "PERSONA");
    HU_ASSERT_EQ(len, strlen("PERSONA"));
    a.free(a.ctx, p, len + 1);
}

static void hard_moment_apply_live_appends_matching_note(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t len = 0;
    char *p = dup_prompt(&a, "PERSONA", &len);
    const char *msg = "I'm so stressed and scared about tomorrow";
    HU_ASSERT_EQ((int)hu_hard_moment_apply(&a, HU_GATE_LIVE, msg, strlen(msg), &p, &len),
                 (int)HU_HARD_MOMENT_DISTRESS);
    HU_ASSERT_TRUE(strncmp(p, "PERSONA", 7) == 0);
    HU_ASSERT_NOT_NULL(strstr(p, hu_hard_moment_note(HU_HARD_MOMENT_DISTRESS)));
    HU_ASSERT_EQ(len, strlen(p));
    a.free(a.ctx, p, len + 1);
}

static void hard_moment_apply_live_neutral_message_leaves_prompt(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t len = 0;
    char *p = dup_prompt(&a, "PERSONA", &len);
    const char *msg = "what time is dinner";
    HU_ASSERT_EQ((int)hu_hard_moment_apply(&a, HU_GATE_LIVE, msg, strlen(msg), &p, &len),
                 (int)HU_HARD_MOMENT_NONE);
    HU_ASSERT_STR_EQ(p, "PERSONA");
    a.free(a.ctx, p, len + 1);
}

void run_hard_moment_tests(void) {
    HU_TEST_SUITE("hard_moment");
    HU_RUN_TEST(hard_moment_stressed_message_is_distress);
    HU_RUN_TEST(hard_moment_sad_lonely_message_is_low_mood);
    HU_RUN_TEST(hard_moment_single_sad_word_is_low_mood);
    HU_RUN_TEST(hard_moment_positive_and_neutral_are_none);
    HU_RUN_TEST(hard_moment_negated_sadness_is_none);
    HU_RUN_TEST(hard_moment_null_and_empty_are_none);
    HU_RUN_TEST(hard_moment_notes_exist_and_avoid_banned_phrases);
    HU_RUN_TEST(hard_moment_apply_off_leaves_prompt_and_skips_work);
    HU_RUN_TEST(hard_moment_apply_shadow_classifies_but_leaves_prompt);
    HU_RUN_TEST(hard_moment_apply_live_appends_matching_note);
    HU_RUN_TEST(hard_moment_apply_live_neutral_message_leaves_prompt);
}
