/* Task 5c — the sqlite-vec store: exact KNN over the memory DB. */
#ifdef HU_ENABLE_SQLITE
#include "human/core/allocator.h"
#include "human/memory/vector/store_sqlite_vec.h"
#include "test_framework.h"
#include <math.h>
#include <sqlite3.h>
#include <string.h>

static hu_embedding_t vec3(float *buf, float a, float b, float c) {
    float n = sqrtf(a * a + b * b + c * c);
    buf[0] = a / n;
    buf[1] = b / n;
    buf[2] = c / n;
    hu_embedding_t e = {.values = buf, .dim = 3};
    return e;
}

static void test_insert_search_remove_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_vector_store_t vs = hu_vector_store_sqlite_vec_create(&alloc, db, 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    float b1[3], b2[3], b3[3], q[3];
    hu_embedding_t x = vec3(b1, 1, 0, 0), y = vec3(b2, 0, 1, 0), xy = vec3(b3, 1, 1, 0);
    HU_ASSERT_EQ(vs.vtable->insert(vs.ctx, &alloc, "x", 1, &x, "about x", 7), HU_OK);
    HU_ASSERT_EQ(vs.vtable->insert(vs.ctx, &alloc, "y", 1, &y, "about y", 7), HU_OK);
    HU_ASSERT_EQ(vs.vtable->insert(vs.ctx, &alloc, "xy", 2, &xy, "about both", 10), HU_OK);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 3L);
    hu_embedding_t qv = vec3(q, 0.9f, 0.1f, 0);
    hu_vector_entry_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(vs.vtable->search(vs.ctx, &alloc, &qv, 2, &out, &n), HU_OK);
    HU_ASSERT_EQ((long)n, 2L);
    HU_ASSERT_STR_EQ(out[0].id, "x");            /* nearest */
    HU_ASSERT_STR_EQ(out[1].id, "xy");           /* then the diagonal */
    HU_ASSERT_TRUE(out[0].score > out[1].score); /* cosine, descending */
    HU_ASSERT_STR_EQ(out[0].content, "about x");
    for (size_t i = 0; i < n; i++) {
        alloc.free(alloc.ctx, (void *)out[i].id, out[i].id_len + 1);
        alloc.free(alloc.ctx, (void *)out[i].content, out[i].content_len + 1);
    }
    alloc.free(alloc.ctx, out, 2 * sizeof(*out));
    /* upsert: same id, new vector, count unchanged */
    HU_ASSERT_EQ(vs.vtable->insert(vs.ctx, &alloc, "x", 1, &y, "moved", 5), HU_OK);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 3L);
    HU_ASSERT_EQ(vs.vtable->remove(vs.ctx, "xy", 2), HU_OK);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 2L);
    vs.vtable->deinit(vs.ctx, &alloc);
    sqlite3_close(db);
}

static void test_wrong_dimension_is_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    hu_vector_store_t vs = hu_vector_store_sqlite_vec_create(&alloc, db, 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    float four[4] = {1, 0, 0, 0};
    hu_embedding_t e = {.values = four, .dim = 4};
    HU_ASSERT_EQ(vs.vtable->insert(vs.ctx, &alloc, "bad", 3, &e, "", 0), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 0L);
    vs.vtable->deinit(vs.ctx, &alloc);
    sqlite3_close(db);
    hu_vector_store_t none = hu_vector_store_sqlite_vec_create(&alloc, NULL, 3);
    HU_ASSERT_NULL(none.ctx);
}

void run_store_sqlite_vec_tests(void) {
    HU_TEST_SUITE("store_sqlite_vec");
    HU_RUN_TEST(test_insert_search_remove_count);
    HU_RUN_TEST(test_wrong_dimension_is_rejected);
}
#else
void run_store_sqlite_vec_tests(void) {
    (void)0;
}
#endif
