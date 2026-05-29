#include "human/cognition/humor.h"
#include "human/core/allocator.h"
#include "human/persona.h"
#include "test_framework.h"
#include <string.h>

/* ================================================================
 * Original tests — persona directive builder
 * ================================================================ */

static void playful_with_humor_config_returns_directive_with_style(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_profile_t humor = {0};
    strncpy(humor.style[0], "observational", sizeof(humor.style[0]) - 1);
    humor.style_count = 1;
    humor.frequency = "medium";
    humor.signature_phrases_count = 0;
    humor.self_deprecation_count = 0;
    humor.never_during_count = 0;

    size_t out_len = 0;
    char *d = hu_humor_build_persona_directive(&alloc, &humor, "content", 7, true, &out_len);

    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(d, "observational") != NULL);
    HU_ASSERT_TRUE(strstr(d, "HUMOR") != NULL);

    alloc.free(alloc.ctx, d, out_len + 1);
}

static void emotion_grief_in_never_during_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_profile_t humor = {0};
    strncpy(humor.style[0], "deadpan", sizeof(humor.style[0]) - 1);
    humor.style_count = 1;
    strncpy(humor.never_during[0], "grief", sizeof(humor.never_during[0]) - 1);
    humor.never_during_count = 1;

    size_t out_len = 0;
    char *d = hu_humor_build_persona_directive(&alloc, &humor, "grief", 5, true, &out_len);

    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(out_len, 0u);
}

static void no_humor_config_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_profile_t humor = {0};
    humor.style_count = 0;
    humor.signature_phrases_count = 0;
    humor.self_deprecation_count = 0;

    size_t out_len = 0;
    char *d = hu_humor_build_persona_directive(&alloc, &humor, "content", 7, true, &out_len);

    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(out_len, 0u);
}

static void not_playful_low_frequency_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_profile_t humor = {0};
    strncpy(humor.style[0], "observational", sizeof(humor.style[0]) - 1);
    humor.style_count = 1;
    humor.frequency = "low";

    size_t out_len = 0;
    char *d = hu_humor_build_persona_directive(&alloc, &humor, "content", 7, false, &out_len);

    HU_ASSERT_NULL(d);
    HU_ASSERT_EQ(out_len, 0u);
}

static void directive_includes_signature_phrases_and_self_deprecation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_profile_t humor = {0};
    strncpy(humor.style[0], "self_deprecating", sizeof(humor.style[0]) - 1);
    humor.style_count = 1;
    strncpy(humor.signature_phrases[0], "as per usual", sizeof(humor.signature_phrases[0]) - 1);
    humor.signature_phrases_count = 1;
    strncpy(humor.self_deprecation_topics[0], "tech fails",
            sizeof(humor.self_deprecation_topics[0]) - 1);
    humor.self_deprecation_count = 1;

    size_t out_len = 0;
    char *d = hu_humor_build_persona_directive(&alloc, &humor, "content", 7, true, &out_len);

    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_TRUE(strstr(d, "as per usual") != NULL);
    HU_ASSERT_TRUE(strstr(d, "tech fails") != NULL);
    HU_ASSERT_TRUE(strstr(d, "Rule of three") != NULL);

    alloc.free(alloc.ctx, d, out_len + 1);
}

/* ================================================================
 * Humor type names
 * ================================================================ */

static void type_name_returns_known_names(void) {
    HU_ASSERT_STR_EQ(hu_humor_type_name(HU_HUMOR_CALLBACK), "callback");
    HU_ASSERT_STR_EQ(hu_humor_type_name(HU_HUMOR_MISDIRECTION), "misdirection");
    HU_ASSERT_STR_EQ(hu_humor_type_name(HU_HUMOR_UNDERSTATEMENT), "understatement");
    HU_ASSERT_STR_EQ(hu_humor_type_name(HU_HUMOR_SELF_DEPRECATION), "self_deprecation");
    HU_ASSERT_STR_EQ(hu_humor_type_name(HU_HUMOR_WORDPLAY), "wordplay");
    HU_ASSERT_STR_EQ(hu_humor_type_name(HU_HUMOR_OBSERVATIONAL), "observational");
}

static void type_from_name_roundtrips(void) {
    for (int i = 0; i < HU_HUMOR_TYPE_COUNT; i++) {
        const char *name = hu_humor_type_name((hu_humor_type_t)i);
        HU_ASSERT_EQ((int)hu_humor_type_from_name(name), i);
    }
}

static void type_from_name_unknown_returns_observational(void) {
    HU_ASSERT_EQ((int)hu_humor_type_from_name("nonsense"), (int)HU_HUMOR_OBSERVATIONAL);
    HU_ASSERT_EQ((int)hu_humor_type_from_name(NULL), (int)HU_HUMOR_OBSERVATIONAL);
}

/* ================================================================
 * Task 1: Audience model (SQLite)
 * ================================================================ */

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

static sqlite3 *open_humor_test_db(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    return db;
}

static void audience_init_creates_table(void) {
    sqlite3 *db = open_humor_test_db();
    HU_ASSERT_EQ(hu_humor_audience_init(db), HU_OK);
    /* Verify table exists by inserting */
    HU_ASSERT_EQ(hu_humor_audience_record(db, "contact_a", HU_HUMOR_WORDPLAY, true), HU_OK);
    sqlite3_close(db);
}

static void audience_record_success_increments_counts(void) {
    sqlite3 *db = open_humor_test_db();
    HU_ASSERT_EQ(hu_humor_audience_init(db), HU_OK);

    HU_ASSERT_EQ(hu_humor_audience_record(db, "user_a", HU_HUMOR_WORDPLAY, true), HU_OK);
    HU_ASSERT_EQ(hu_humor_audience_record(db, "user_a", HU_HUMOR_WORDPLAY, true), HU_OK);
    HU_ASSERT_EQ(hu_humor_audience_record(db, "user_a", HU_HUMOR_WORDPLAY, false), HU_OK);

    hu_humor_audience_t aud = {0};
    HU_ASSERT_EQ(hu_humor_audience_load(db, "user_a", &aud), HU_OK);
    HU_ASSERT_EQ(aud.success_count[HU_HUMOR_WORDPLAY], 2);
    HU_ASSERT_EQ(aud.attempt_count[HU_HUMOR_WORDPLAY], 3);

    sqlite3_close(db);
}

static void audience_preferred_type_returns_most_successful(void) {
    sqlite3 *db = open_humor_test_db();
    HU_ASSERT_EQ(hu_humor_audience_init(db), HU_OK);

    /* 1 success for callback */
    HU_ASSERT_EQ(hu_humor_audience_record(db, "user_b", HU_HUMOR_CALLBACK, true), HU_OK);
    /* 3 successes for observational */
    HU_ASSERT_EQ(hu_humor_audience_record(db, "user_b", HU_HUMOR_OBSERVATIONAL, true), HU_OK);
    HU_ASSERT_EQ(hu_humor_audience_record(db, "user_b", HU_HUMOR_OBSERVATIONAL, true), HU_OK);
    HU_ASSERT_EQ(hu_humor_audience_record(db, "user_b", HU_HUMOR_OBSERVATIONAL, true), HU_OK);

    hu_humor_audience_t aud = {0};
    HU_ASSERT_EQ(hu_humor_audience_load(db, "user_b", &aud), HU_OK);
    HU_ASSERT_EQ((int)hu_humor_audience_preferred_type(&aud), (int)HU_HUMOR_OBSERVATIONAL);

    sqlite3_close(db);
}

static void audience_empty_history_returns_default(void) {
    hu_humor_audience_t aud = {0};
    HU_ASSERT_EQ((int)hu_humor_audience_preferred_type(&aud), (int)HU_HUMOR_OBSERVATIONAL);
}

static void audience_null_returns_default(void) {
    HU_ASSERT_EQ((int)hu_humor_audience_preferred_type(NULL), (int)HU_HUMOR_OBSERVATIONAL);
}

static void audience_load_empty_contact_returns_zeroes(void) {
    sqlite3 *db = open_humor_test_db();
    HU_ASSERT_EQ(hu_humor_audience_init(db), HU_OK);

    hu_humor_audience_t aud = {0};
    HU_ASSERT_EQ(hu_humor_audience_load(db, "nobody", &aud), HU_OK);
    for (int i = 0; i < HU_HUMOR_TYPE_COUNT; i++) {
        HU_ASSERT_EQ(aud.success_count[i], 0);
        HU_ASSERT_EQ(aud.attempt_count[i], 0);
    }

    sqlite3_close(db);
}

static void audience_init_null_db_returns_error(void) {
    HU_ASSERT_EQ(hu_humor_audience_init(NULL), HU_ERR_INVALID_ARGUMENT);
}

#endif /* HU_ENABLE_SQLITE */

/* ================================================================
 * Task 2: Timing + appropriateness
 * ================================================================ */

static void timing_crisis_active_returns_false(void) {
    hu_humor_timing_result_t r = hu_humor_check_timing(14, 0.5f, true, "deep");
    HU_ASSERT_TRUE(!r.allowed);
    HU_ASSERT_STR_CONTAINS(r.reason, "Crisis");
}

static void timing_low_valence_returns_false(void) {
    hu_humor_timing_result_t r = hu_humor_check_timing(14, -0.8f, false, "deep");
    HU_ASSERT_TRUE(!r.allowed);
    HU_ASSERT_STR_CONTAINS(r.reason, "distress");
}

static void timing_happy_afternoon_returns_true(void) {
    hu_humor_timing_result_t r = hu_humor_check_timing(14, 0.6f, false, "friend");
    HU_ASSERT_TRUE(r.allowed);
}

static void timing_late_night_not_deep_returns_false(void) {
    hu_humor_timing_result_t r = hu_humor_check_timing(2, 0.5f, false, "acquaintance");
    HU_ASSERT_TRUE(!r.allowed);
    HU_ASSERT_STR_CONTAINS(r.reason, "Late night");
}

static void timing_late_night_deep_returns_true(void) {
    hu_humor_timing_result_t r = hu_humor_check_timing(2, 0.5f, false, "deep");
    HU_ASSERT_TRUE(r.allowed);
}

static void timing_hour_23_not_deep_returns_false(void) {
    hu_humor_timing_result_t r = hu_humor_check_timing(23, 0.5f, false, "new");
    HU_ASSERT_TRUE(!r.allowed);
}

static void timing_hour_5_is_allowed(void) {
    hu_humor_timing_result_t r = hu_humor_check_timing(5, 0.5f, false, "friend");
    HU_ASSERT_TRUE(r.allowed);
}

static void appropriate_self_deprecation_acquaintance_false(void) {
    HU_ASSERT_TRUE(!hu_humor_check_appropriate(HU_HUMOR_SELF_DEPRECATION, "tech", "acquaintance"));
}

static void appropriate_misdirection_acquaintance_false(void) {
    HU_ASSERT_TRUE(!hu_humor_check_appropriate(HU_HUMOR_MISDIRECTION, "tech", "acquaintance"));
}

static void appropriate_wordplay_acquaintance_true(void) {
    HU_ASSERT_TRUE(hu_humor_check_appropriate(HU_HUMOR_WORDPLAY, "tech", "acquaintance"));
}

static void appropriate_self_deprecation_health_topic_false(void) {
    HU_ASSERT_TRUE(!hu_humor_check_appropriate(HU_HUMOR_SELF_DEPRECATION, "health issues", "deep"));
}

static void appropriate_grief_topic_always_false(void) {
    HU_ASSERT_TRUE(!hu_humor_check_appropriate(HU_HUMOR_OBSERVATIONAL, "grief counseling", "deep"));
    HU_ASSERT_TRUE(!hu_humor_check_appropriate(HU_HUMOR_WORDPLAY, "death of a pet", "deep"));
    HU_ASSERT_TRUE(!hu_humor_check_appropriate(HU_HUMOR_CALLBACK, "funeral planning", "deep"));
}

static void appropriate_observational_deep_normal_topic_true(void) {
    HU_ASSERT_TRUE(hu_humor_check_appropriate(HU_HUMOR_OBSERVATIONAL, "cooking", "deep"));
}

static void appropriate_null_topic_true(void) {
    HU_ASSERT_TRUE(hu_humor_check_appropriate(HU_HUMOR_WORDPLAY, NULL, "deep"));
}

/* ================================================================
 * Task 3: Generation strategy
 * ================================================================ */

static void strategy_null_audience_uses_observational(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 0;
    char *s = hu_humor_generate_strategy(&alloc, NULL, "cooking", 0.3f, "friend", NULL, &out_len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_CONTAINS(s, "observational");
    HU_ASSERT_STR_CONTAINS(s, "cooking");
    alloc.free(alloc.ctx, s, out_len + 1);
}

static void strategy_happy_mood_prefers_wordplay(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 0;
    char *s = hu_humor_generate_strategy(&alloc, NULL, "tech", 0.7f, "friend", NULL, &out_len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_CONTAINS(s, "wordplay");
    alloc.free(alloc.ctx, s, out_len + 1);
}

static void strategy_low_mood_prefers_understatement(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 0;
    char *s = hu_humor_generate_strategy(&alloc, NULL, "work", -0.4f, "friend", NULL, &out_len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_CONTAINS(s, "understatement");
    alloc.free(alloc.ctx, s, out_len + 1);
}

static void strategy_includes_persona_style(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_profile_t humor = {0};
    strncpy(humor.style[0], "deadpan", sizeof(humor.style[0]) - 1);
    humor.style_count = 1;

    size_t out_len = 0;
    char *s = hu_humor_generate_strategy(&alloc, NULL, "food", 0.3f, "friend", &humor, &out_len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_CONTAINS(s, "Style: deadpan");
    alloc.free(alloc.ctx, s, out_len + 1);
}

static void strategy_includes_audience_preference(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_humor_audience_t aud = {0};
    aud.success_count[HU_HUMOR_CALLBACK] = 5;
    aud.attempt_count[HU_HUMOR_CALLBACK] = 7;

    size_t out_len = 0;
    char *s = hu_humor_generate_strategy(&alloc, &aud, "tech", 0.3f, "friend", NULL, &out_len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_CONTAINS(s, "callback");
    HU_ASSERT_STR_CONTAINS(s, "Audience prefers callback");
    alloc.free(alloc.ctx, s, out_len + 1);
}

static void strategy_null_alloc_returns_null(void) {
    size_t out_len = 0;
    char *s = hu_humor_generate_strategy(NULL, NULL, "tech", 0.3f, "friend", NULL, &out_len);
    HU_ASSERT_NULL(s);
}

/* ================================================================
 * Task 4: Failed humor recovery
 * ================================================================ */

static void detect_failure_short_response_true(void) {
    HU_ASSERT_TRUE(hu_humor_detect_failure("ok", 2));
    HU_ASSERT_TRUE(hu_humor_detect_failure("k", 1));
}

static void detect_failure_what_question_true(void) {
    const char *r = "what? I don't understand";
    HU_ASSERT_TRUE(hu_humor_detect_failure(r, strlen(r)));
}

static void detect_failure_topic_change_true(void) {
    const char *r = "anyway, let's talk about something else";
    HU_ASSERT_TRUE(hu_humor_detect_failure(r, strlen(r)));
}

static void detect_failure_moving_on_true(void) {
    const char *r = "moving on from that";
    HU_ASSERT_TRUE(hu_humor_detect_failure(r, strlen(r)));
}

static void detect_failure_laugh_response_false(void) {
    const char *r = "haha that's hilarious, tell me more!";
    HU_ASSERT_TRUE(!hu_humor_detect_failure(r, strlen(r)));
}

static void detect_failure_normal_response_false(void) {
    const char *r = "That's a great point about the project timeline";
    HU_ASSERT_TRUE(!hu_humor_detect_failure(r, strlen(r)));
}

static void detect_failure_null_returns_false(void) {
    HU_ASSERT_TRUE(!hu_humor_detect_failure(NULL, 0));
    HU_ASSERT_TRUE(!hu_humor_detect_failure("hello", 0));
}

static void recover_returns_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 0;
    char *d = hu_humor_recover(&alloc, &out_len);
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_STR_CONTAINS(d, "didn't land");
    HU_ASSERT_STR_CONTAINS(d, "Do NOT explain");
    alloc.free(alloc.ctx, d, out_len + 1);
}

static void recover_null_alloc_returns_null(void) {
    size_t out_len = 0;
    char *d = hu_humor_recover(NULL, &out_len);
    HU_ASSERT_NULL(d);
}

/* ================================================================
 * Test suite runner
 * ================================================================ */

void run_humor_tests(void) {
    HU_TEST_SUITE("humor");

    /* Original directive tests */
    HU_RUN_TEST(playful_with_humor_config_returns_directive_with_style);
    HU_RUN_TEST(emotion_grief_in_never_during_returns_null);
    HU_RUN_TEST(no_humor_config_returns_null);
    HU_RUN_TEST(not_playful_low_frequency_returns_null);
    HU_RUN_TEST(directive_includes_signature_phrases_and_self_deprecation);

    /* Humor type names */
    HU_RUN_TEST(type_name_returns_known_names);
    HU_RUN_TEST(type_from_name_roundtrips);
    HU_RUN_TEST(type_from_name_unknown_returns_observational);

    /* Task 1: Audience model */
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(audience_init_creates_table);
    HU_RUN_TEST(audience_record_success_increments_counts);
    HU_RUN_TEST(audience_preferred_type_returns_most_successful);
    HU_RUN_TEST(audience_empty_history_returns_default);
    HU_RUN_TEST(audience_null_returns_default);
    HU_RUN_TEST(audience_load_empty_contact_returns_zeroes);
    HU_RUN_TEST(audience_init_null_db_returns_error);
#endif

    /* Task 2: Timing + appropriateness */
    HU_RUN_TEST(timing_crisis_active_returns_false);
    HU_RUN_TEST(timing_low_valence_returns_false);
    HU_RUN_TEST(timing_happy_afternoon_returns_true);
    HU_RUN_TEST(timing_late_night_not_deep_returns_false);
    HU_RUN_TEST(timing_late_night_deep_returns_true);
    HU_RUN_TEST(timing_hour_23_not_deep_returns_false);
    HU_RUN_TEST(timing_hour_5_is_allowed);
    HU_RUN_TEST(appropriate_self_deprecation_acquaintance_false);
    HU_RUN_TEST(appropriate_misdirection_acquaintance_false);
    HU_RUN_TEST(appropriate_wordplay_acquaintance_true);
    HU_RUN_TEST(appropriate_self_deprecation_health_topic_false);
    HU_RUN_TEST(appropriate_grief_topic_always_false);
    HU_RUN_TEST(appropriate_observational_deep_normal_topic_true);
    HU_RUN_TEST(appropriate_null_topic_true);

    /* Task 3: Generation strategy */
    HU_RUN_TEST(strategy_null_audience_uses_observational);
    HU_RUN_TEST(strategy_happy_mood_prefers_wordplay);
    HU_RUN_TEST(strategy_low_mood_prefers_understatement);
    HU_RUN_TEST(strategy_includes_persona_style);
    HU_RUN_TEST(strategy_includes_audience_preference);
    HU_RUN_TEST(strategy_null_alloc_returns_null);

    /* Task 4: Failed humor recovery */
    HU_RUN_TEST(detect_failure_short_response_true);
    HU_RUN_TEST(detect_failure_what_question_true);
    HU_RUN_TEST(detect_failure_topic_change_true);
    HU_RUN_TEST(detect_failure_moving_on_true);
    HU_RUN_TEST(detect_failure_laugh_response_false);
    HU_RUN_TEST(detect_failure_normal_response_false);
    HU_RUN_TEST(detect_failure_null_returns_false);
    HU_RUN_TEST(recover_returns_directive);
    HU_RUN_TEST(recover_null_alloc_returns_null);
}
