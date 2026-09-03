/* Contract C2 — reconstructive hybrid retrieval (EverMemOS shape):
 * scene-select -> neighbour expansion -> rerank -> time-bounded filter ->
 * sufficiency check, wired behind hu_retrieval_options_t.reconstructive and
 * exercised through hu_hybrid_retrieve (the production symbol under test).
 *
 * SQLite-gated; a stub keeps run_hybrid_reconstructive_tests() resolvable
 * when SQLite is off (mirrors tests/test_semantic_index.c's pattern). */
#ifdef HU_ENABLE_SQLITE
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include "human/memory/vector/store_sqlite_vec.h"
#include "test_framework.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- stub embedder (deterministic 3-dim), lifted from test_semantic_index.c ---- */
static hu_error_t stub_embed(void *ctx, hu_allocator_t *alloc, const char *text, size_t len,
                             hu_embedding_t *out) {
    (void)ctx;
    float *v = (float *)alloc->alloc(alloc->ctx, 3 * sizeof(float));
    if (!v)
        return HU_ERR_OUT_OF_MEMORY;
    unsigned c = len ? (unsigned char)text[0] : 0;
    v[0] = (c % 3 == 0) ? 1.0f : 0.1f;
    v[1] = (c % 3 == 1) ? 1.0f : 0.1f;
    v[2] = (c % 3 == 2) ? 1.0f : 0.1f;
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= n;
    v[1] /= n;
    v[2] /= n;
    out->values = v;
    out->dim = 3;
    return HU_OK;
}
static hu_error_t stub_embed_batch(void *ctx, hu_allocator_t *alloc, const char **texts,
                                   const size_t *lens, size_t count, hu_embedding_t *out) {
    for (size_t i = 0; i < count; i++) {
        hu_error_t e = stub_embed(ctx, alloc, texts[i], lens[i], &out[i]);
        if (e != HU_OK)
            return e;
    }
    return HU_OK;
}
static size_t stub_dims(void *ctx) {
    (void)ctx;
    return 3;
}
static void stub_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}
static const hu_embedder_vtable_t stub_vt = {.embed = stub_embed,
                                             .embed_batch = stub_embed_batch,
                                             .dimensions = stub_dims,
                                             .deinit = stub_deinit};

/* Backdate a stored row's created_at (impl_store always writes "now"; the
 * scene/temporal logic under test needs controlled timestamps). Test-only:
 * builds the SQL from trusted, test-authored literals. */
static void set_created_at(hu_memory_t *mem, const char *key, const char *iso_ts) {
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    char sql[512];
    snprintf(sql, sizeof(sql), "UPDATE memories SET created_at='%s' WHERE key='%s'", iso_ts, key);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void fmt_iso(time_t t, char *buf, size_t buf_cap) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, buf_cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static bool result_has_key(const hu_retrieval_result_t *r, const char *key) {
    for (size_t i = 0; i < r->count; i++)
        if (r->entries[i].key && strcmp(r->entries[i].key, key) == 0)
            return true;
    return false;
}

static bool result_has_content_word(const hu_retrieval_result_t *r, const char *word) {
    for (size_t i = 0; i < r->count; i++)
        if (r->entries[i].content && strstr(r->entries[i].content, word))
            return true;
    return false;
}

/* strlen()-derived lengths throughout -- a hand-counted literal length is a
 * one-off-away key/content truncation that silently corrupts the WHERE
 * clause in set_created_at() above (caught during authoring of this file). */
static hu_error_t store_row(hu_memory_t *mem, const char *key, const char *content,
                            const char *session_id) {
    return mem->vtable->store(mem->ctx, key, strlen(key), content, strlen(content), NULL,
                              session_id, session_id ? strlen(session_id) : 0);
}

/* AC-1: scene-select picks the two-hit session over the one-hit session.
 * Session A has two matching rows, session B has one; with limit=2 the
 * reconstructive path must fill the answer from A alone, never from B. */
static void test_scene_select_prefers_two_hit_session_over_one_hit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);

    HU_ASSERT_EQ(store_row(&mem, "a1", "regatta sailing schedule saturday morning race", "A"),
                 HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "a2", "regatta sailing schedule friday committee meeting", "A"),
                 HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "b1", "regatta sailing schedule postponed announcement", "B"),
                 HU_OK);

    hu_retrieval_options_t opts = {0};
    opts.limit = 2;
    opts.reconstructive = true;
    hu_retrieval_result_t res = {0};
    const char *q = "regatta sailing schedule";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res),
                 HU_OK);

    HU_ASSERT_TRUE(res.count > 0);
    HU_ASSERT_TRUE(result_has_key(&res, "a1") || result_has_key(&res, "a2"));
    HU_ASSERT_TRUE(!result_has_key(&res, "b1"));

    hu_retrieval_result_free(&alloc, &res);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* AC-2: a temporal cue ("still") in the query prefers the newer of two rows
 * that share a key prefix (a superseded fact family), dropping the older one
 * even though both are keyword hits. */
static void test_temporal_cue_prefers_newer_same_prefix_row(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    HU_ASSERT_EQ(store_row(&mem, "profile:city:2026-01-01", "I live in Springfield", NULL), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "profile:city:2026-06-01", "I live in Shelbyville now", NULL),
                 HU_OK);

    time_t now = time(NULL);
    char old_ts[64], new_ts[64];
    fmt_iso(now - (time_t)(180 * 86400), old_ts, sizeof(old_ts)); /* ~6 months ago */
    fmt_iso(now, new_ts, sizeof(new_ts));
    set_created_at(&mem, "profile:city:2026-01-01", old_ts);
    set_created_at(&mem, "profile:city:2026-06-01", new_ts);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    opts.reconstructive = true;
    hu_retrieval_result_t res = {0};
    const char *q = "do I still live in shelbyville";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, q, strlen(q), &opts, &res),
                 HU_OK);

    HU_ASSERT_EQ((long)res.count, 1L);
    HU_ASSERT_TRUE(result_has_content_word(&res, "Shelbyville"));
    HU_ASSERT_TRUE(!result_has_content_word(&res, "Springfield"));

    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
}

/* AC-3: when the candidate pool only spans one scene, the sufficiency check
 * must fall back to the plain hybrid result -- never an empty one, even
 * though reconstructive=true was requested. */
static void test_sufficiency_fallback_returns_plain_result_for_one_scene(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);

    HU_ASSERT_EQ(store_row(&mem, "s1", "budget meeting notes for the project", "S"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "s2", "budget meeting followup action items", "S"), HU_OK);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    opts.reconstructive = true;
    hu_retrieval_result_t res = {0};
    const char *q = "budget meeting";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res),
                 HU_OK);

    /* Only one scene (session "S") exists -- reconstruction must decline and
     * the plain hybrid merge must still answer. */
    HU_ASSERT_TRUE(res.count > 0);

    hu_retrieval_result_free(&alloc, &res);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

void run_hybrid_reconstructive_tests(void) {
    HU_TEST_SUITE("hybrid_reconstructive");
    HU_RUN_TEST(test_scene_select_prefers_two_hit_session_over_one_hit);
    HU_RUN_TEST(test_temporal_cue_prefers_newer_same_prefix_row);
    HU_RUN_TEST(test_sufficiency_fallback_returns_plain_result_for_one_scene);
}
#else
void run_hybrid_reconstructive_tests(void) {
    (void)0;
}
#endif
