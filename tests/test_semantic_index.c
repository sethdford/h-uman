/* 5d — the sqlite engine embeds rows at write time and can reindex the
 * backlog. Uses a stub embedder (deterministic 3-dim) + the real sqlite-vec
 * store on the engine's own connection. */
#ifdef HU_ENABLE_SQLITE
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/vector.h"
#include "human/memory/vector/store_sqlite_vec.h"
#include "test_framework.h"
#include <math.h>
#include <string.h>

static hu_error_t stub_embed(void *ctx, hu_allocator_t *alloc, const char *text, size_t len,
                             hu_embedding_t *out) {
    (void)ctx;
    float *v = (float *)alloc->alloc(alloc->ctx, 3 * sizeof(float));
    if (!v)
        return HU_ERR_OUT_OF_MEMORY;
    /* first char picks an axis; deterministic and content-dependent */
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

static void test_store_indexes_row_when_index_attached(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 0L);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "k1", 2, "apple pie", 9, NULL, "", 0), HU_OK);
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "k2", 2, "banana split", 12, NULL, "", 0), HU_OK);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 2L); /* indexed at write time */
    size_t done = 99;
    HU_ASSERT_EQ(hu_sqlite_memory_reindex_semantic(&mem, 0, &done), HU_OK);
    HU_ASSERT_EQ((long)done, 0L); /* nothing left: 0 is honest here */
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

static void test_reindex_backfills_rows_stored_before_attach(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "a", 1, "alpha", 5, NULL, "", 0), HU_OK);
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "b", 1, "bravo", 5, NULL, "", 0), HU_OK);
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "c", 1, "charlie", 7, NULL, "", 0), HU_OK);
    size_t done = 0;
    /* No index attached: reindex must REFUSE, not report 0. */
    HU_ASSERT_EQ(hu_sqlite_memory_reindex_semantic(&mem, 0, &done), HU_ERR_NOT_SUPPORTED);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);
    HU_ASSERT_EQ(hu_sqlite_memory_reindex_semantic(&mem, 0, &done), HU_OK);
    HU_ASSERT_EQ((long)done, 3L);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 3L);
    /* Nearest neighbour of "avocado" (same axis as "alpha") is key a. */
    hu_embedding_t q = {0};
    HU_ASSERT_EQ(stub_embed(NULL, &alloc, "avocado", 7, &q), HU_OK);
    hu_vector_entry_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(vs.vtable->search(vs.ctx, &alloc, &q, 1, &out, &n), HU_OK);
    HU_ASSERT_EQ((long)n, 1L);
    HU_ASSERT_STR_EQ(out[0].id, "a");
    alloc.free(alloc.ctx, (void *)out[0].id, out[0].id_len + 1);
    alloc.free(alloc.ctx, (void *)out[0].content, out[0].content_len + 1);
    alloc.free(alloc.ctx, out, sizeof(*out));
    alloc.free(alloc.ctx, q.values, 3 * sizeof(float));
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

/* ── Index policy (2026-09-02 live-gate finding): 576 of the 1130 rows in
 * the production index were "experience:" episodic records — eval
 * scaffolding in Task:/Actions:/Outcome:/Score: format that surfaced as
 * top hits for almost every context. They never belong in the semantic
 * index: skipped at write time, skipped AND purged at reindex. ─────── */

static void test_store_skips_experience_rows_at_write_time(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);
    const char *scaffold = "Task: hi\nActions: agent_turn\nOutcome: ok\nScore: 1.0000";
    /* The row itself IS stored (experience_log / keyword recall still see it). */
    HU_ASSERT_EQ(
        mem.vtable->store(mem.ctx, "experience:hi", 13, scaffold, strlen(scaffold), NULL, "", 0),
        HU_OK);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 0L); /* but never indexed */
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "k1", 2, "apple pie", 9, NULL, "", 0), HU_OK);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 1L);
    hu_memory_entry_t row = {0};
    bool found = false;
    HU_ASSERT_EQ(mem.vtable->get(mem.ctx, &alloc, "experience:hi", 13, &row, &found), HU_OK);
    HU_ASSERT(found); /* the row is reachable by key; only the vector is skipped */
    hu_memory_entry_free_fields(&alloc, &row);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

static void test_reindex_skips_and_purges_experience_rows(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    const char *scaffold = "Task: a\nActions: b\nOutcome: c\nScore: 0.5000";
    HU_ASSERT_EQ(
        mem.vtable->store(mem.ctx, "experience:a", 12, scaffold, strlen(scaffold), NULL, "", 0),
        HU_OK);
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "b", 1, "bravo", 5, NULL, "", 0), HU_OK);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    /* A stale index row from before the write-time exclusion. */
    hu_embedding_t e = {0};
    HU_ASSERT_EQ(stub_embed(NULL, &alloc, scaffold, strlen(scaffold), &e), HU_OK);
    HU_ASSERT_EQ(
        vs.vtable->insert(vs.ctx, &alloc, "experience:old", 14, &e, scaffold, strlen(scaffold)),
        HU_OK);
    alloc.free(alloc.ctx, e.values, 3 * sizeof(float));
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 1L);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);
    size_t done = 0;
    HU_ASSERT_EQ(hu_sqlite_memory_reindex_semantic(&mem, 0, &done), HU_OK);
    HU_ASSERT_EQ((long)done, 1L);                     /* only "b" was embedded */
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 1L); /* experience:old purged, b added */
    hu_embedding_t q = {0};
    HU_ASSERT_EQ(stub_embed(NULL, &alloc, "bravo", 5, &q), HU_OK);
    hu_vector_entry_t *out = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(vs.vtable->search(vs.ctx, &alloc, &q, 5, &out, &n), HU_OK);
    HU_ASSERT_EQ((long)n, 1L);
    HU_ASSERT_STR_EQ(out[0].id, "b");
    hu_vector_entries_free(&alloc, out, n);
    alloc.free(alloc.ctx, q.values, 3 * sizeof(float));
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

void run_semantic_index_tests(void) {
    HU_TEST_SUITE("semantic_index");
    HU_RUN_TEST(test_store_indexes_row_when_index_attached);
    HU_RUN_TEST(test_reindex_backfills_rows_stored_before_attach);
    HU_RUN_TEST(test_store_skips_experience_rows_at_write_time);
    HU_RUN_TEST(test_reindex_skips_and_purges_experience_rows);
}
#else
void run_semantic_index_tests(void) {
    (void)0;
}
#endif
