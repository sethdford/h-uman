typedef int hu_test_persona_mood_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include <sqlite3.h>
#include "human/persona/mood.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>
#include <time.h>

static void mood_set_stressed_08_get_returns_08(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err = hu_mood_set(&alloc, &mem, HU_MOOD_STRESSED, 0.8f, "back-to-back meetings", 21);
    HU_ASSERT_EQ(err, HU_OK);

    hu_mood_state_t state = {0};
    err = hu_mood_get_current(&alloc, &mem, &state);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(state.mood, HU_MOOD_STRESSED);
    HU_ASSERT_FLOAT_EQ(state.intensity, 0.8f, 0.01f);
    HU_ASSERT_STR_EQ(state.cause, "back-to-back meetings");

    mem.vtable->deinit(mem.ctx);
}

static void mood_simulate_4hr_decay_stressed_approx_045(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* Insert with set_at = now - 4 hours */
    int64_t now_ts = (int64_t)time(NULL);
    int64_t past_ts = now_ts - 4 * 3600;

    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO mood_log(mood, intensity, cause, set_at) "
                                "VALUES('stressed', 0.8, 'meetings', ?)",
                                -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, past_ts);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    HU_ASSERT_EQ(rc, SQLITE_DONE);

    /* Invalidate cache so we fetch from DB */
    hu_mood_state_t state = {0};
    hu_error_t err = hu_mood_get_current(&alloc, &mem, &state);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(state.mood, HU_MOOD_STRESSED);

    /* Decay: 0.8 * exp(-0.15 * 4) ≈ 0.8 * 0.549 ≈ 0.44 */
    float expected = 0.8f * (float)exp(-0.15 * 4.0);
    HU_ASSERT_FLOAT_EQ(state.intensity, expected, 0.05f);
    HU_ASSERT_TRUE(state.intensity >= 0.4f && state.intensity <= 0.5f);

    mem.vtable->deinit(mem.ctx);
}

static void mood_low_intensity_returns_null_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mood_state_t state = {
        .mood = HU_MOOD_STRESSED,
        .intensity = 0.1f,
        .cause = "",
        .decay_rate = 0.15f,
        .set_at = 0,
    };
    size_t out_len = 0;
    char *dir = hu_mood_build_directive(&alloc, &state, &out_len);
    HU_ASSERT_NULL(dir);
}

static void mood_high_intensity_returns_non_null_directive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mood_state_t state = {
        .mood = HU_MOOD_STRESSED,
        .intensity = 0.8f,
        .decay_rate = 0.15f,
        .set_at = 0,
    };
    memcpy(state.cause, "back-to-back meetings", 21);
    state.cause[21] = '\0';

    size_t out_len = 0;
    char *dir = hu_mood_build_directive(&alloc, &state, &out_len);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_TRUE(strstr(dir, "CURRENT MOOD") != NULL);
    HU_ASSERT_TRUE(strstr(dir, "stressed") != NULL);
    HU_ASSERT_TRUE(strstr(dir, "back-to-back meetings") != NULL);
    alloc.free(alloc.ctx, dir, out_len + 1);
}

static void mood_decayed_below_01_returns_neutral(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* Insert stressed 0.5 at 12 hours ago — decay: 0.5 * exp(-0.15*12) ≈ 0.08 */
    int64_t now_ts = (int64_t)time(NULL);
    int64_t past_ts = now_ts - 12 * 3600;

    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO mood_log(mood, intensity, cause, set_at) "
                                "VALUES('stressed', 0.5, 'old', ?)",
                                -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, past_ts);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    hu_mood_state_t state = {0};
    hu_error_t err = hu_mood_get_current(&alloc, &mem, &state);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(state.mood, HU_MOOD_NEUTRAL);
    HU_ASSERT_FLOAT_EQ(state.intensity, 0.f, 0.01f);

    mem.vtable->deinit(mem.ctx);
}

static void mood_none_memory_returns_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);

    hu_mood_state_t state = {0};
    hu_error_t err = hu_mood_get_current(&alloc, &mem, &state);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);

    err = hu_mood_set(&alloc, &mem, HU_MOOD_HAPPY, 0.5f, "test", 4);
    HU_ASSERT_EQ(err, HU_ERR_NOT_SUPPORTED);

    mem.vtable->deinit(mem.ctx);
}

void run_persona_mood_tests(void) {
    HU_TEST_SUITE("persona_mood");
    HU_RUN_TEST(mood_set_stressed_08_get_returns_08);
    HU_RUN_TEST(mood_simulate_4hr_decay_stressed_approx_045);
    HU_RUN_TEST(mood_low_intensity_returns_null_directive);
    HU_RUN_TEST(mood_high_intensity_returns_non_null_directive);
    HU_RUN_TEST(mood_decayed_below_01_returns_neutral);
    HU_RUN_TEST(mood_none_memory_returns_not_supported);
}

#else

void run_persona_mood_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
