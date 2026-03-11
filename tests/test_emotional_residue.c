#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/emotional_residue.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

static void emotional_residue_add_and_get_active_finds_with_decay(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t id = 0;
    hu_error_t err = hu_emotional_residue_add(db, 0, "contact_x", 9, 0.7, 0.8, 0.1, &id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(id > 0);

    hu_emotional_residue_t *out = NULL;
    size_t count = 0;
    int64_t now_ts = (int64_t)time(NULL) + 3600; /* 1 hour later */
    err = hu_emotional_residue_get_active(&alloc, db, "contact_x", 9, now_ts, &out, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(out[0].contact_id, "contact_x");
    HU_ASSERT_TRUE(out[0].valence > 0.0);
    /* intensity should be decayed: 0.8 * exp(-0.1 * (1/24)) ≈ 0.797 */
    HU_ASSERT_TRUE(out[0].intensity > 0.7 && out[0].intensity <= 0.81);

    alloc.free(alloc.ctx, out, count * sizeof(hu_emotional_residue_t));
    mem.vtable->deinit(mem.ctx);
}

static void emotional_residue_after_many_days_excluded(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t id = 0;
    HU_ASSERT_EQ(hu_emotional_residue_add(db, 0, "contact_y", 9, -0.5, 0.3, 0.2, &id), HU_OK);

    /* 30 days later: 0.3 * exp(-0.2 * 30) = 0.3 * exp(-6) ≈ 0.0074 < 0.05 */
    int64_t now_ts = (int64_t)time(NULL) + (int64_t)(30 * 86400);
    hu_emotional_residue_t *out = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_emotional_residue_get_active(&alloc, db, "contact_y", 9, now_ts, &out, &count),
                 HU_OK);
    HU_ASSERT_EQ(count, 0u);
    HU_ASSERT_NULL(out);

    mem.vtable->deinit(mem.ctx);
}

static void emotional_residue_build_directive_returns_non_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_residue_t r = {0};
    r.id = 1;
    r.episode_id = 1;
    memcpy(r.contact_id, "user_a", 7);
    r.valence = 0.6;
    r.intensity = 0.7;
    r.decay_rate = 0.1;
    r.created_at = (int64_t)time(NULL);

    size_t len = 0;
    char *dir = hu_emotional_residue_build_directive(&alloc, &r, 1, &len);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(dir, "EMOTIONAL RESIDUE") != NULL);
    HU_ASSERT_TRUE(strstr(dir, "positive") != NULL || strstr(dir, "negative") != NULL);
    alloc.free(alloc.ctx, dir, len + 1);
}

void run_emotional_residue_tests(void) {
    HU_TEST_SUITE("emotional_residue");
    HU_RUN_TEST(emotional_residue_add_and_get_active_finds_with_decay);
    HU_RUN_TEST(emotional_residue_after_many_days_excluded);
    HU_RUN_TEST(emotional_residue_build_directive_returns_non_null);
}

#else

void run_emotional_residue_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
