#ifdef HU_ENABLE_SQLITE

#include "human/context/style_tracker.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "test_framework.h"
#include <string.h>

static void style_update_haha_sets_laugh_style_and_lowercase(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err =
        hu_style_fingerprint_update(&mem, &alloc, "contact_x", 9, "haha that's great", 17);
    HU_ASSERT_EQ(err, HU_OK);

    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    err = hu_style_fingerprint_get(&mem, &alloc, "contact_x", 9, &fp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(fp.uses_lowercase);
    HU_ASSERT_STR_EQ(fp.laugh_style, "haha");

    mem.vtable->deinit(mem.ctx);
}

static void style_update_periods_sets_uses_periods(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err =
        hu_style_fingerprint_update(&mem, &alloc, "contact_y", 9, "OK. Sounds good.", 16);
    HU_ASSERT_EQ(err, HU_OK);

    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    err = hu_style_fingerprint_get(&mem, &alloc, "contact_y", 9, &fp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(fp.uses_periods);
    HU_ASSERT_FALSE(fp.uses_lowercase);

    mem.vtable->deinit(mem.ctx);
}

static void style_get_build_directive_contains_hints(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    (void)hu_style_fingerprint_update(&mem, &alloc, "contact_z", 9, "haha that's great", 17);

    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    hu_style_fingerprint_get(&mem, &alloc, "contact_z", 9, &fp);

    char buf[256];
    size_t len = hu_style_fingerprint_build_directive(&fp, buf, sizeof(buf));
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "STYLE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "haha") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Match it") != NULL);

    mem.vtable->deinit(mem.ctx);
}

static void style_no_fingerprint_build_directive_returns_zero(void) {
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));

    char buf[256];
    size_t len = hu_style_fingerprint_build_directive(&fp, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);
}

static void style_get_nonexistent_returns_zeroed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_style_fingerprint_t fp;
    memset(&fp, 0xff, sizeof(fp));
    hu_error_t err = hu_style_fingerprint_get(&mem, &alloc, "nonexistent", 11, &fp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(fp.uses_lowercase);
    HU_ASSERT_FALSE(fp.uses_periods);
    HU_ASSERT_EQ(fp.laugh_style[0], '\0');

    char buf[256];
    size_t len = hu_style_fingerprint_build_directive(&fp, buf, sizeof(buf));
    HU_ASSERT_EQ(len, 0u);

    mem.vtable->deinit(mem.ctx);
}

static void style_none_memory_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);

    hu_error_t err = hu_style_fingerprint_update(&mem, &alloc, "x", 1, "haha", 4);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);

    hu_style_fingerprint_t fp;
    err = hu_style_fingerprint_get(&mem, &alloc, "x", 1, &fp);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);

    mem.vtable->deinit(mem.ctx);
}

static void style_update_populates_common_phrases(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err = hu_style_fingerprint_update(&mem, &alloc, "contact_phrases", 15,
                                                 "that sounds great thanks so much", 33);
    HU_ASSERT_EQ(err, HU_OK);

    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    err = hu_style_fingerprint_get(&mem, &alloc, "contact_phrases", 15, &fp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(fp.common_phrases[0] != '\0');
    HU_ASSERT_EQ(fp.common_phrases[0], '[');
    HU_ASSERT_TRUE(strstr(fp.common_phrases, "sounds great") != NULL);

    mem.vtable->deinit(mem.ctx);
}

/* ── Inbound-path entry point (adaptive per-contact style loop) ────── */

static void style_update_inbound_populates_fingerprint(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* Pre: no row exists for this contact */
    hu_style_fingerprint_t fp;
    memset(&fp, 0xff, sizeof(fp));
    HU_ASSERT_EQ(hu_style_fingerprint_get(&mem, &alloc, "user_a", 6, &fp), HU_OK);
    HU_ASSERT_FALSE(fp.uses_lowercase);
    HU_ASSERT_EQ(fp.avg_message_length, 0);

    /* Invoke the production inbound entry point (the daemon calls this for
     * every received message on the common batch path) */
    hu_error_t err =
        hu_style_fingerprint_update_inbound(&mem, &alloc, "user_a", 6, "lol yeah that works", 19);
    HU_ASSERT_EQ(err, HU_OK);

    /* Post: fingerprint now reflects the contact's inbound message */
    memset(&fp, 0, sizeof(fp));
    HU_ASSERT_EQ(hu_style_fingerprint_get(&mem, &alloc, "user_a", 6, &fp), HU_OK);
    HU_ASSERT_TRUE(fp.uses_lowercase);
    HU_ASSERT_STR_EQ(fp.laugh_style, "lol");
    HU_ASSERT_EQ(fp.avg_message_length, 19);

    mem.vtable->deinit(mem.ctx);
}

static void style_update_inbound_refreshes_existing_row(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* Pre: a stale row from an earlier message (periods, no laugh style) */
    HU_ASSERT_EQ(
        hu_style_fingerprint_update_inbound(&mem, &alloc, "user_b", 6, "OK. Sounds good.", 16),
        HU_OK);
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    HU_ASSERT_EQ(hu_style_fingerprint_get(&mem, &alloc, "user_b", 6, &fp), HU_OK);
    HU_ASSERT_EQ(fp.laugh_style[0], '\0');
    HU_ASSERT_EQ(fp.avg_message_length, 16);

    /* A new inbound message merges into the existing row */
    HU_ASSERT_EQ(hu_style_fingerprint_update_inbound(&mem, &alloc, "user_b", 6,
                                                     "haha yeah that works great", 26),
                 HU_OK);
    memset(&fp, 0, sizeof(fp));
    HU_ASSERT_EQ(hu_style_fingerprint_get(&mem, &alloc, "user_b", 6, &fp), HU_OK);
    HU_ASSERT_STR_EQ(fp.laugh_style, "haha");
    HU_ASSERT_EQ(fp.avg_message_length, (16 + 26) / 2);

    mem.vtable->deinit(mem.ctx);
}

static void style_update_inbound_rejects_reserved_self_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* "__self__" is reserved for drift self-tracking; an inbound message must
     * never overwrite it */
    HU_ASSERT_EQ(hu_style_fingerprint_update_inbound(&mem, &alloc, "__self__", 8, "haha", 4),
                 HU_ERR_INVALID_ARGUMENT);

    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    HU_ASSERT_EQ(hu_style_fingerprint_get(&mem, &alloc, "__self__", 8, &fp), HU_OK);
    HU_ASSERT_EQ(fp.laugh_style[0], '\0');
    HU_ASSERT_EQ(fp.avg_message_length, 0);

    mem.vtable->deinit(mem.ctx);
}

static void style_update_inbound_empty_message_is_noop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_style_fingerprint_update_inbound(&mem, &alloc, "user_c", 6, "", 0), HU_OK);

    /* No row was created */
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    HU_ASSERT_EQ(hu_style_fingerprint_get(&mem, &alloc, "user_c", 6, &fp), HU_OK);
    HU_ASSERT_EQ(fp.avg_message_length, 0);
    HU_ASSERT_FALSE(fp.uses_lowercase);

    mem.vtable->deinit(mem.ctx);
}

static void style_update_inbound_null_memory_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_style_fingerprint_update_inbound(NULL, &alloc, "user_d", 6, "hi", 2),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ── Self-tracking / drift detection tests ────────────────────────── */

static void style_update_self_stores_fingerprint(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err = hu_style_fingerprint_update_self(&mem, &alloc, "haha that's great", 17);
    HU_ASSERT_EQ(err, HU_OK);

    /* Verify it's stored under __self__ */
    hu_style_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    err = hu_style_fingerprint_get(&mem, &alloc, "__self__", 8, &fp);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(fp.uses_lowercase);
    HU_ASSERT_STR_EQ(fp.laugh_style, "haha");

    mem.vtable->deinit(mem.ctx);
}

static void style_update_self_null_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_style_fingerprint_update_self(NULL, &alloc, "hi", 2), HU_ERR_INVALID_ARGUMENT);
}

static void style_drift_no_data_returns_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_style_fingerprint_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.uses_lowercase = true;

    hu_style_drift_result_t drift;
    hu_error_t err = hu_style_drift_check(&mem, &alloc, &baseline, &drift);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FLOAT_EQ((float)drift.score, 0.0f, 0.01f);
    HU_ASSERT_FALSE(drift.corrective);

    mem.vtable->deinit(mem.ctx);
}

static void style_drift_identical_returns_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* Feed self messages that match baseline */
    hu_style_fingerprint_update_self(&mem, &alloc, "haha that's great", 17);

    hu_style_fingerprint_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.uses_lowercase = true;
    baseline.uses_periods = false;
    strncpy(baseline.laugh_style, "haha", sizeof(baseline.laugh_style));

    hu_style_drift_result_t drift;
    hu_error_t err = hu_style_drift_check(&mem, &alloc, &baseline, &drift);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FLOAT_EQ((float)drift.score, 0.0f, 0.1f);
    HU_ASSERT_FALSE(drift.corrective);

    mem.vtable->deinit(mem.ctx);
}

static void style_drift_mismatch_increases_score(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* Self uses formal style: periods, uppercase, "lol" */
    hu_style_fingerprint_update_self(&mem, &alloc, "OK. Sounds good. LOL.", 21);

    /* Baseline expects: lowercase, no periods, "haha" */
    hu_style_fingerprint_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.uses_lowercase = true;
    baseline.uses_periods = false;
    strncpy(baseline.laugh_style, "haha", sizeof(baseline.laugh_style));

    hu_style_drift_result_t drift;
    hu_error_t err = hu_style_drift_check(&mem, &alloc, &baseline, &drift);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(drift.score > 0.3);

    mem.vtable->deinit(mem.ctx);
}

static void style_drift_above_threshold_triggers_correction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* Self uses completely opposite style */
    hu_style_fingerprint_update_self(&mem, &alloc, "OK. Sounds good. LOL.", 21);

    hu_style_fingerprint_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.uses_lowercase = true;
    baseline.uses_periods = false;
    strncpy(baseline.laugh_style, "haha", sizeof(baseline.laugh_style));
    baseline.avg_message_length = 200; /* very different from 21 */

    hu_style_drift_result_t drift;
    hu_error_t err = hu_style_drift_check(&mem, &alloc, &baseline, &drift);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(drift.score >= 0.5);
    HU_ASSERT_TRUE(drift.corrective);
    HU_ASSERT_NOT_NULL(strstr(drift.directive, "DRIFT"));

    mem.vtable->deinit(mem.ctx);
}

static void style_drift_check_no_self_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* No update_self calls — drift_check should return 0.0 */
    hu_style_fingerprint_t baseline;
    memset(&baseline, 0, sizeof(baseline));

    hu_style_drift_result_t drift;
    hu_error_t err = hu_style_drift_check(&mem, &alloc, &baseline, &drift);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FLOAT_EQ((float)drift.score, 0.0f, 0.01f);
    HU_ASSERT_FALSE(drift.corrective);

    mem.vtable->deinit(mem.ctx);
}

static void style_drift_null_result_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_style_fingerprint_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    HU_ASSERT_EQ(hu_style_drift_check(&mem, &alloc, &baseline, NULL), HU_ERR_INVALID_ARGUMENT);

    mem.vtable->deinit(mem.ctx);
}

void run_style_tracker_tests(void) {
    HU_TEST_SUITE("style_tracker");
    HU_RUN_TEST(style_update_haha_sets_laugh_style_and_lowercase);
    HU_RUN_TEST(style_update_periods_sets_uses_periods);
    HU_RUN_TEST(style_get_build_directive_contains_hints);
    HU_RUN_TEST(style_no_fingerprint_build_directive_returns_zero);
    HU_RUN_TEST(style_get_nonexistent_returns_zeroed);
    HU_RUN_TEST(style_none_memory_returns_not_supported);
    HU_RUN_TEST(style_update_populates_common_phrases);

    HU_TEST_SUITE("style_inbound");
    HU_RUN_TEST(style_update_inbound_populates_fingerprint);
    HU_RUN_TEST(style_update_inbound_refreshes_existing_row);
    HU_RUN_TEST(style_update_inbound_rejects_reserved_self_key);
    HU_RUN_TEST(style_update_inbound_empty_message_is_noop);
    HU_RUN_TEST(style_update_inbound_null_memory_returns_error);

    HU_TEST_SUITE("style_drift");
    HU_RUN_TEST(style_update_self_stores_fingerprint);
    HU_RUN_TEST(style_update_self_null_returns_error);
    HU_RUN_TEST(style_drift_no_data_returns_zero);
    HU_RUN_TEST(style_drift_identical_returns_zero);
    HU_RUN_TEST(style_drift_mismatch_increases_score);
    HU_RUN_TEST(style_drift_above_threshold_triggers_correction);
    HU_RUN_TEST(style_drift_check_no_self_data);
    HU_RUN_TEST(style_drift_null_result_returns_error);
}

#else

void run_style_tracker_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
