/* Feedback signal detection (F78) tests */
#include "human/core/allocator.h"
#include "human/intelligence/feedback.h"
#include "human/memory.h"
#include "test_framework.h"
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

static void test_feedback_quick_response_long_emoji_positive(void) {
    /* response_time < 120000 (+2), length > 50 (+1), has_emoji (+1) = 4 >= 2 → POSITIVE */
    hu_feedback_class_t s = hu_feedback_classify(
        60000,  /* 1 min response */
        100,    /* long message */
        true,   /* has_emoji */
        false,  /* no tapback */
        false,  /* topic not continued */
        false,  /* no laughter */
        false,  /* topic not changed */
        false   /* not ignored */
    );
    HU_ASSERT_EQ(s, HU_FEEDBACK_CLASS_POSITIVE);
}

static void test_feedback_leave_on_read_short_negative(void) {
    /* response_time > 7200000 (-2), length < 10 (-1), ignored_question (-2) = -5 <= -2 → NEGATIVE */
    hu_feedback_class_t s = hu_feedback_classify(
        8000000, /* > 2 hours (leave on read) */
        5,       /* short */
        false,
        false,
        false,
        false,
        false,
        true /* ignored_question */
    );
    HU_ASSERT_EQ(s, HU_FEEDBACK_CLASS_NEGATIVE);
}

static void test_feedback_medium_response_no_extras_neutral(void) {
    /* response_time in middle (0), length 30 (0), no extras → 0, NEUTRAL */
    hu_feedback_class_t s = hu_feedback_classify(
        300000, /* 5 min */
        30,
        false,
        false,
        false,
        false,
        false,
        false
    );
    HU_ASSERT_EQ(s, HU_FEEDBACK_CLASS_NEUTRAL);
}

static void test_feedback_signal_str(void) {
    HU_ASSERT_STR_EQ(hu_feedback_class_str(HU_FEEDBACK_CLASS_POSITIVE), "positive");
    HU_ASSERT_STR_EQ(hu_feedback_class_str(HU_FEEDBACK_CLASS_NEGATIVE), "negative");
    HU_ASSERT_STR_EQ(hu_feedback_class_str(HU_FEEDBACK_CLASS_NEUTRAL), "neutral");
}

#ifdef HU_ENABLE_SQLITE
static void test_feedback_record_inserts_row(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    const char *bt = "response_style";
    const char *cid = "contact_a";
    const char *sig = "positive";
    const char *ctx = "quick reply with emoji";
    int64_t ts = 1700000000;

    hu_error_t err = hu_feedback_record(db, bt, strlen(bt), cid, strlen(cid), sig, strlen(sig),
                                        ctx, strlen(ctx), ts);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT id, behavior_type, contact_id, signal, context, "
                                    "timestamp FROM behavioral_feedback",
                                -1, &sel, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(sel);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(sel, 0), 1);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(sel, 1), "response_style");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(sel, 2), "contact_a");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(sel, 3), "positive");
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(sel, 4), "quick reply with emoji");
    HU_ASSERT_EQ(sqlite3_column_int64(sel, 5), ts);
    sqlite3_finalize(sel);

    mem.vtable->deinit(mem.ctx);
}
#endif

void run_feedback_tests(void) {
    HU_TEST_SUITE("feedback");
    HU_RUN_TEST(test_feedback_quick_response_long_emoji_positive);
    HU_RUN_TEST(test_feedback_leave_on_read_short_negative);
    HU_RUN_TEST(test_feedback_medium_response_no_extras_neutral);
    HU_RUN_TEST(test_feedback_signal_str);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_feedback_record_inserts_row);
#endif
}
