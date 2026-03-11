typedef int hu_test_anticipatory_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/context/anticipatory.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/superhuman.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

static void store_micro_moment_kid_game_predicts_nervous(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err = hu_superhuman_micro_moment_store(
        &mem, &alloc, "contact_a", 9, "kid game tomorrow", 17, "sports event", 12);
    HU_ASSERT_EQ(err, HU_OK);

    hu_emotional_prediction_t *preds = NULL;
    size_t count = 0;
    int64_t now_ts = (int64_t)time(NULL);
    err = hu_anticipatory_predict(&alloc, &mem, "contact_a", 9, now_ts, &preds, &count);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(preds);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_TRUE(strstr(preds[0].predicted_emotion, "nervous") != NULL);
    HU_ASSERT_TRUE(preds[0].confidence > 0.5f);

    hu_anticipatory_predictions_free(&alloc, preds, count);
    mem.vtable->deinit(mem.ctx);
}

static void no_events_empty_predictions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_emotional_prediction_t *preds = NULL;
    size_t count = 0;
    int64_t now_ts = (int64_t)time(NULL);
    hu_error_t err = hu_anticipatory_predict(&alloc, &mem, "contact_none", 11, now_ts, &preds, &count);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(preds);
    HU_ASSERT_EQ(count, 0u);

    mem.vtable->deinit(mem.ctx);
}

static void build_directive_with_predictions_returns_non_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_prediction_t preds[1];
    memset(preds, 0, sizeof(preds));
    memcpy(preds[0].contact_id, "contact_a", 9);
    memcpy(preds[0].predicted_emotion, "nervous", 7);
    preds[0].confidence = 0.75f;
    memcpy(preds[0].basis, "kid has a game tomorrow", 23);

    size_t out_len = 0;
    char *s = hu_anticipatory_build_directive(&alloc, preds, 1, "Sarah", 5, &out_len);

    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(s, "ANTICIPATORY") != NULL);
    HU_ASSERT_TRUE(strstr(s, "nervous") != NULL);
    HU_ASSERT_TRUE(strstr(s, "checking in") != NULL);

    alloc.free(alloc.ctx, s, out_len + 1);
}

static void build_directive_low_confidence_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_prediction_t preds[1];
    memset(preds, 0, sizeof(preds));
    preds[0].confidence = 0.3f;
    memcpy(preds[0].predicted_emotion, "nervous", 7);
    memcpy(preds[0].basis, "game", 4);

    size_t out_len = 0;
    char *s = hu_anticipatory_build_directive(&alloc, preds, 1, "Test", 4, &out_len);

    HU_ASSERT_NULL(s);
    HU_ASSERT_EQ(out_len, 0u);
}

static void exam_keyword_predicts_stressed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err = hu_superhuman_micro_moment_store(
        &mem, &alloc, "contact_b", 9, "big exam next week", 18, "finals", 6);
    HU_ASSERT_EQ(err, HU_OK);

    hu_emotional_prediction_t *preds = NULL;
    size_t count = 0;
    int64_t now_ts = (int64_t)time(NULL);
    err = hu_anticipatory_predict(&alloc, &mem, "contact_b", 9, now_ts, &preds, &count);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(preds);
    HU_ASSERT_TRUE(count >= 1);
    HU_ASSERT_TRUE(strstr(preds[0].predicted_emotion, "stressed") != NULL);

    hu_anticipatory_predictions_free(&alloc, preds, count);
    mem.vtable->deinit(mem.ctx);
}

void run_anticipatory_tests(void) {
    HU_TEST_SUITE("anticipatory");
    HU_RUN_TEST(store_micro_moment_kid_game_predicts_nervous);
    HU_RUN_TEST(no_events_empty_predictions);
    HU_RUN_TEST(build_directive_with_predictions_returns_non_null);
    HU_RUN_TEST(build_directive_low_confidence_returns_null);
    HU_RUN_TEST(exam_keyword_predicts_stressed);
}

#else

void run_anticipatory_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
