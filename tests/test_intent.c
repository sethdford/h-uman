/* Tests for the intent-aware response-type classifier (src/agent/intent.c).
 * Positive contracts per intent + the word-boundary guard (a substring match
 * would misclassify "wonder" as GOOD_NEWS via "won"). */
#include "human/agent/intent.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static hu_intent_t classify(const char *m) {
    hu_intent_analysis_t a;
    hu_intent_analyze(m, m ? strlen(m) : 0, &a);
    return a.intent;
}

static void intent_logistics(void) {
    HU_ASSERT_EQ((int)classify("you around this weekend?"), (int)HU_INTENT_LOGISTICS);
    HU_ASSERT_EQ((int)classify("what time works for dinner tomorrow"), (int)HU_INTENT_LOGISTICS);
}

static void intent_seeking_advice(void) {
    HU_ASSERT_EQ((int)classify("should i take the job? what do you think"),
                 (int)HU_INTENT_SEEKING_ADVICE);
}

static void intent_just_venting(void) {
    HU_ASSERT_EQ((int)classify("ugh they ALWAYS do this!! so annoyed"),
                 (int)HU_INTENT_JUST_VENTING);
}

static void intent_vulnerable_share(void) {
    HU_ASSERT_EQ((int)classify("honestly i've never told anyone this but i feel like a fraud"),
                 (int)HU_INTENT_VULNERABLE_SHARE);
}

static void intent_good_news(void) {
    HU_ASSERT_EQ((int)classify("guess what i finally got the job!!"), (int)HU_INTENT_GOOD_NEWS);
}

static void intent_needs_to_be_heard(void) {
    /* long + emotional, no question / advice ask */
    const char *m =
        "i've been feeling so overwhelmed lately and i just keep going and going, work has been "
        "exhausting and i barely sleep, i feel like i'm carrying everything on my own and i don't "
        "even know how to start unwinding any of it, it's just a lot right now";
    HU_ASSERT_EQ((int)classify(m), (int)HU_INTENT_NEEDS_TO_BE_HEARD);
}

/* Word-boundary guard: "won" (a GOOD_NEWS keyword) is a substring of "wonder".
 * A naive str_contains would misfire here; hu_str_contains_word_ci must not. */
static void intent_word_boundary_guard(void) {
    HU_ASSERT_NEQ((int)classify("i wonder how that even works"), (int)HU_INTENT_GOOD_NEWS);
}

/* Neutral / low-signal -> default, and the builder injects nothing. */
static void intent_low_confidence_default(void) {
    hu_intent_analysis_t a;
    hu_intent_analyze("ok", 2, &a);
    HU_ASSERT_EQ((int)a.intent, (int)HU_INTENT_PROCESSING_ALOUD);
    /* HU_ASSERT_LT casts to long long (truncates doubles); compare as double. */
    HU_ASSERT_TRUE(a.confidence < HU_INTENT_CONFIDENCE_THRESHOLD);

    hu_allocator_t alloc = hu_system_allocator();
    char *dir = (char *)0x1;
    size_t dir_len = 99;
    HU_ASSERT_EQ((int)hu_intent_build_directive(&alloc, &a, &dir, &dir_len), (int)HU_OK);
    HU_ASSERT_NULL(dir); /* below threshold -> nothing injected */
    HU_ASSERT_EQ((int)dir_len, 0);
}

/* High-confidence intent -> a non-empty, intent-named directive. */
static void intent_build_directive_shape(void) {
    hu_intent_analysis_t a;
    hu_intent_analyze("you around this weekend?", 24, &a);
    HU_ASSERT_EQ((int)a.intent, (int)HU_INTENT_LOGISTICS);
    HU_ASSERT_TRUE(a.confidence >= HU_INTENT_CONFIDENCE_THRESHOLD);

    hu_allocator_t alloc = hu_system_allocator();
    char *dir = NULL;
    size_t dir_len = 0;
    HU_ASSERT_EQ((int)hu_intent_build_directive(&alloc, &a, &dir, &dir_len), (int)HU_OK);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_TRUE(dir_len > 0);
    HU_ASSERT_STR_CONTAINS(dir, "logistics");
    alloc.free(alloc.ctx, dir, dir_len + 1);
}

static void intent_name_roundtrip(void) {
    HU_ASSERT_STR_EQ(hu_intent_name(HU_INTENT_LOGISTICS), "logistics");
    HU_ASSERT_STR_EQ(hu_intent_name(HU_INTENT_VULNERABLE_SHARE), "vulnerable_share");
}

static void intent_null_safe(void) {
    hu_intent_analysis_t a;
    hu_intent_analyze(NULL, 0, &a);
    HU_ASSERT_EQ((int)a.intent, (int)HU_INTENT_PROCESSING_ALOUD);
    HU_ASSERT_FLOAT_EQ(a.confidence, 0.0, 1e-9);
}

void run_intent_tests(void) {
    HU_TEST_SUITE("intent");
    HU_RUN_TEST(intent_logistics);
    HU_RUN_TEST(intent_seeking_advice);
    HU_RUN_TEST(intent_just_venting);
    HU_RUN_TEST(intent_vulnerable_share);
    HU_RUN_TEST(intent_good_news);
    HU_RUN_TEST(intent_needs_to_be_heard);
    HU_RUN_TEST(intent_word_boundary_guard);
    HU_RUN_TEST(intent_low_confidence_default);
    HU_RUN_TEST(intent_build_directive_shape);
    HU_RUN_TEST(intent_name_roundtrip);
    HU_RUN_TEST(intent_null_safe);
}
