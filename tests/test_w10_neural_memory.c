/* W10 — Neural memory tier: adversarial tests for KV-cache, reasoning traces,
 * and multimodal blobs.
 *
 * All tests run against an in-memory SQLite DB via hu_graph_open(NULL, 0),
 * consistent with the W7 test pattern.  Every test that allocates frees before
 * returning — ASan is the final arbiter. */

#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/memory/neural_memory.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) { g_alloc = hu_system_allocator(); return &g_alloc; }

static void open_nn(hu_graph_t **g, hu_memory_facade_t **m) {
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
    HU_ASSERT_NOT_NULL(*g);
    HU_ASSERT_EQ(hu_memory_facade_open(A(), *g, m), HU_OK);
    HU_ASSERT_NOT_NULL(*m);
}

static void close_nn(hu_graph_t *g, hu_memory_facade_t *m) {
    hu_memory_facade_close(m, A());
    hu_graph_close(g, A());
}

/* ── KV-cache ─────────────────────────────────────────────────────────────── */

static void test_w10_kv_cache_round_trip(void) {
    hu_graph_t  *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_nn(&g, &m);

    static const uint8_t kv_data[] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                                       0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10};

    hu_kv_cache_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.prompt_hash,   "deadbeef01234567", sizeof(entry.prompt_hash)   - 1);
    strncpy(entry.model_version, "gemini-3.1-pro",   sizeof(entry.model_version) - 1);
    entry.prompt_token_count = 512;
    entry.blob               = (void *)kv_data;
    entry.blob_len           = sizeof(kv_data);
    entry.created_at         = 1000000;

    HU_ASSERT_EQ(hu_kv_cache_put(m, &entry), HU_OK);

    hu_kv_cache_entry_t *got = NULL;
    HU_ASSERT_EQ(hu_kv_cache_get(m, "deadbeef01234567", "gemini-3.1-pro", A(), &got), HU_OK);
    HU_ASSERT_NOT_NULL(got);
    HU_ASSERT_EQ(got->prompt_token_count, 512);
    HU_ASSERT_EQ((int)got->blob_len, (int)sizeof(kv_data));
    HU_ASSERT_EQ(memcmp(got->blob, kv_data, sizeof(kv_data)), 0);
    HU_ASSERT_EQ(strcmp(got->prompt_hash,   "deadbeef01234567"), 0);
    HU_ASSERT_EQ(strcmp(got->model_version, "gemini-3.1-pro"),   0);

    hu_kv_cache_entry_free(A(), got);
    close_nn(g, m);
}

static void test_w10_kv_cache_invalidates_on_model_version_change(void) {
    hu_graph_t  *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_nn(&g, &m);

    static const uint8_t kv[] = {0xAA, 0xBB};
    hu_kv_cache_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.prompt_hash,   "prompt_abc", sizeof(entry.prompt_hash)   - 1);
    strncpy(entry.model_version, "v1",          sizeof(entry.model_version) - 1);
    entry.blob     = (void *)kv;
    entry.blob_len = sizeof(kv);

    HU_ASSERT_EQ(hu_kv_cache_put(m, &entry), HU_OK);

    /* Same hash, different model version → miss. */
    hu_kv_cache_entry_t *got = NULL;
    HU_ASSERT_EQ(hu_kv_cache_get(m, "prompt_abc", "v2", A(), &got), HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(got);

    /* Original version still present. */
    HU_ASSERT_EQ(hu_kv_cache_get(m, "prompt_abc", "v1", A(), &got), HU_OK);
    HU_ASSERT_NOT_NULL(got);
    hu_kv_cache_entry_free(A(), got);

    close_nn(g, m);
}

static void test_w10_kv_cache_overwrites_on_duplicate_put(void) {
    hu_graph_t  *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_nn(&g, &m);

    static const uint8_t first[]  = {0x01, 0x02, 0x03};
    static const uint8_t second[] = {0xAA, 0xBB, 0xCC};

    hu_kv_cache_entry_t e;
    memset(&e, 0, sizeof(e));
    strncpy(e.prompt_hash,   "collision_hash", sizeof(e.prompt_hash)   - 1);
    strncpy(e.model_version, "v1",              sizeof(e.model_version) - 1);
    e.blob     = (void *)first;
    e.blob_len = sizeof(first);
    HU_ASSERT_EQ(hu_kv_cache_put(m, &e), HU_OK);

    /* Overwrite with different content. */
    e.blob     = (void *)second;
    e.blob_len = sizeof(second);
    HU_ASSERT_EQ(hu_kv_cache_put(m, &e), HU_OK);

    hu_kv_cache_entry_t *got = NULL;
    HU_ASSERT_EQ(hu_kv_cache_get(m, "collision_hash", "v1", A(), &got), HU_OK);
    HU_ASSERT_NOT_NULL(got);
    HU_ASSERT_EQ((int)got->blob_len, (int)sizeof(second));
    HU_ASSERT_EQ(memcmp(got->blob, second, sizeof(second)), 0);

    hu_kv_cache_entry_free(A(), got);
    close_nn(g, m);
}

static void test_w10_kv_cache_invalidate_for_model_clears_correct_rows(void) {
    hu_graph_t  *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_nn(&g, &m);

    static const uint8_t kv[] = {0xDE, 0xAD};

    hu_kv_cache_entry_t e;
    memset(&e, 0, sizeof(e));
    e.blob     = (void *)kv;
    e.blob_len = sizeof(kv);

    strncpy(e.prompt_hash,   "hash_a", sizeof(e.prompt_hash)   - 1);
    strncpy(e.model_version, "old_v",  sizeof(e.model_version) - 1);
    HU_ASSERT_EQ(hu_kv_cache_put(m, &e), HU_OK);

    memset(e.prompt_hash, 0, sizeof(e.prompt_hash));
    strncpy(e.prompt_hash, "hash_b", sizeof(e.prompt_hash) - 1);
    HU_ASSERT_EQ(hu_kv_cache_put(m, &e), HU_OK);

    memset(e.prompt_hash,   0, sizeof(e.prompt_hash));
    memset(e.model_version, 0, sizeof(e.model_version));
    strncpy(e.prompt_hash,   "hash_c",  sizeof(e.prompt_hash)   - 1);
    strncpy(e.model_version, "new_v",   sizeof(e.model_version) - 1);
    HU_ASSERT_EQ(hu_kv_cache_put(m, &e), HU_OK);

    HU_ASSERT_EQ(hu_kv_cache_invalidate_for_model(m, "old_v"), HU_OK);

    hu_kv_cache_entry_t *got = NULL;
    HU_ASSERT_EQ(hu_kv_cache_get(m, "hash_a", "old_v", A(), &got), HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(got);

    HU_ASSERT_EQ(hu_kv_cache_get(m, "hash_b", "old_v", A(), &got), HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(got);

    HU_ASSERT_EQ(hu_kv_cache_get(m, "hash_c", "new_v", A(), &got), HU_OK);
    HU_ASSERT_NOT_NULL(got);
    hu_kv_cache_entry_free(A(), got);

    close_nn(g, m);
}

/* ── Reasoning traces ─────────────────────────────────────────────────────── */

/* Insert a trace with the given anchors and cot_text; return the assigned id. */
static int64_t insert_trace(hu_memory_facade_t *m, const char *goal,
                             const int64_t *anchors, size_t n_anchors,
                             const char *cot) {
    hu_reasoning_trace_t t;
    memset(&t, 0, sizeof(t));
    strncpy(t.goal_verb, goal, sizeof(t.goal_verb) - 1);
    t.anchor_entity_ids = (int64_t *)anchors;
    t.anchors_count     = n_anchors;
    t.cot_text          = (char *)cot;
    t.cot_len           = strlen(cot);
    t.belief.mean       = 1.0f;
    t.belief.variance   = 0.0f;
    int64_t id          = 0;
    HU_ASSERT_EQ(hu_reasoning_trace_record(m, "u1", 2, &t, &id), HU_OK);
    HU_ASSERT_GT(id, 0);
    return id;
}

static void test_w10_reasoning_trace_recall_by_goal_and_anchors(void) {
    hu_graph_t  *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_nn(&g, &m);

    /* Three traces with distinct anchors; two share goal "solve". */
    int64_t anc_A[] = {10, 20};  /* trace A: anchors 10,20 */
    int64_t anc_B[] = {20, 30};  /* trace B: anchors 20,30 */
    int64_t anc_C[] = {40};      /* trace C: different goal */

    insert_trace(m, "solve", anc_A, 2, "COT_A");
    insert_trace(m, "solve", anc_B, 2, "COT_B");
    insert_trace(m, "plan",  anc_C, 1, "COT_C");

    /* Query goal="solve", anchor=[30] → only trace B matches. */
    int64_t q_anc[] = {30};
    hu_reasoning_trace_t *out   = NULL;
    size_t                count = 0;
    HU_ASSERT_EQ(
        hu_reasoning_trace_recall(m, A(), "u1", 2, "solve", 5,
                                  q_anc, 1, 10, &out, &count),
        HU_OK);
    HU_ASSERT_EQ((int)count, 1);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(strcmp(out[0].cot_text, "COT_B"), 0);
    hu_reasoning_traces_free(A(), out, count);

    /* Query goal="solve", anchor=[10] → only trace A matches. */
    int64_t q_anc2[] = {10};
    out   = NULL;
    count = 0;
    HU_ASSERT_EQ(
        hu_reasoning_trace_recall(m, A(), "u1", 2, "solve", 5,
                                  q_anc2, 1, 10, &out, &count),
        HU_OK);
    HU_ASSERT_EQ((int)count, 1);
    HU_ASSERT_EQ(strcmp(out[0].cot_text, "COT_A"), 0);
    hu_reasoning_traces_free(A(), out, count);

    /* Query goal="plan", anchor=[40] → only trace C matches. */
    int64_t q_anc3[] = {40};
    out   = NULL;
    count = 0;
    HU_ASSERT_EQ(
        hu_reasoning_trace_recall(m, A(), "u1", 2, "plan", 4,
                                  q_anc3, 1, 10, &out, &count),
        HU_OK);
    HU_ASSERT_EQ((int)count, 1);
    HU_ASSERT_EQ(strcmp(out[0].cot_text, "COT_C"), 0);
    hu_reasoning_traces_free(A(), out, count);

    close_nn(g, m);
}

static void test_w10_reasoning_trace_recall_respects_limit(void) {
    hu_graph_t  *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_nn(&g, &m);

    /* Insert 5 traces with the same goal and no anchors. */
    for (int i = 0; i < 5; i++) {
        char cot[32];
        snprintf(cot, sizeof(cot), "COT_%d", i);
        insert_trace(m, "think", NULL, 0, cot);
    }

    /* Recall with limit=2 → at most 2 results. */
    hu_reasoning_trace_t *out   = NULL;
    size_t                count = 0;
    HU_ASSERT_EQ(
        hu_reasoning_trace_recall(m, A(), "u1", 2, "think", 5,
                                  NULL, 0, 2, &out, &count),
        HU_OK);
    HU_ASSERT_LT((int)count, 3);  /* count <= 2 */
    HU_ASSERT_GT((int)count, 0);  /* at least one result */
    hu_reasoning_traces_free(A(), out, count);

    /* Recall with limit=0 (no limit) → all 5 results. */
    out   = NULL;
    count = 0;
    HU_ASSERT_EQ(
        hu_reasoning_trace_recall(m, A(), "u1", 2, "think", 5,
                                  NULL, 0, 0, &out, &count),
        HU_OK);
    HU_ASSERT_EQ((int)count, 5);
    hu_reasoning_traces_free(A(), out, count);

    close_nn(g, m);
}

/* ── Multimodal blobs ─────────────────────────────────────────────────────── */

static void test_w10_blob_round_trip_image_bytes(void) {
    hu_graph_t  *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_nn(&g, &m);

    /* 4 KB synthetic image-like payload with deterministic byte pattern. */
    static uint8_t image[4096];
    for (int i = 0; i < 4096; i++)
        image[i] = (uint8_t)(i & 0xFF);

    hu_memory_blob_t b;
    memset(&b, 0, sizeof(b));
    strncpy(b.mime_type, "image/png", sizeof(b.mime_type) - 1);
    b.bytes     = image;
    b.bytes_len = sizeof(image);

    int64_t id = 0;
    HU_ASSERT_EQ(hu_memory_blob_put(m, "u1", 2, &b, &id), HU_OK);
    HU_ASSERT_GT(id, 0);

    hu_memory_blob_t *got = NULL;
    HU_ASSERT_EQ(hu_memory_blob_get(m, A(), id, &got), HU_OK);
    HU_ASSERT_NOT_NULL(got);
    HU_ASSERT_EQ(got->id, id);
    HU_ASSERT_EQ((int)got->bytes_len, (int)sizeof(image));
    HU_ASSERT_NOT_NULL(got->bytes);
    HU_ASSERT_EQ(memcmp(got->bytes, image, sizeof(image)), 0);
    HU_ASSERT_EQ(strcmp(got->mime_type, "image/png"), 0);
    /* No caption was stored. */
    HU_ASSERT_NULL(got->caption);

    hu_memory_blob_free(A(), got);
    close_nn(g, m);
}

static void test_w10_adversarial_oversized_blob_rejected(void) {
    hu_graph_t  *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_nn(&g, &m);

    /* Construct a blob struct with bytes_len > 100 MB without allocating the
     * actual memory — the implementation must check the size before any I/O. */
    hu_memory_blob_t big;
    memset(&big, 0, sizeof(big));
    strncpy(big.mime_type, "image/jpeg", sizeof(big.mime_type) - 1);
    big.bytes_len = 101ULL * 1024ULL * 1024ULL; /* 101 MB */
    big.bytes     = (void *)big.mime_type;       /* dummy non-NULL; never dereferenced */

    int64_t id = 0;
    HU_ASSERT_EQ(hu_memory_blob_put(m, "u1", 2, &big, &id), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(id, 0);

    close_nn(g, m);
}

static void test_w10_blob_unknown_id_returns_not_found(void) {
    hu_graph_t  *g = NULL;
    hu_memory_facade_t *m = NULL;
    open_nn(&g, &m);

    hu_memory_blob_t *got = NULL;
    HU_ASSERT_EQ(hu_memory_blob_get(m, A(), 99999, &got), HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(got);

    close_nn(g, m);
}

#endif /* HU_ENABLE_SQLITE */

/* ── Test runner ──────────────────────────────────────────────────────────── */

void run_w10_neural_memory_tests(void) {
    HU_TEST_SUITE("W10 neural memory - KV cache + reasoning traces + blobs");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w10_kv_cache_round_trip);
    HU_RUN_TEST(test_w10_kv_cache_invalidates_on_model_version_change);
    HU_RUN_TEST(test_w10_kv_cache_overwrites_on_duplicate_put);
    HU_RUN_TEST(test_w10_kv_cache_invalidate_for_model_clears_correct_rows);
    HU_RUN_TEST(test_w10_reasoning_trace_recall_by_goal_and_anchors);
    HU_RUN_TEST(test_w10_reasoning_trace_recall_respects_limit);
    HU_RUN_TEST(test_w10_blob_round_trip_image_bytes);
    HU_RUN_TEST(test_w10_adversarial_oversized_blob_rejected);
    HU_RUN_TEST(test_w10_blob_unknown_id_returns_not_found);
#endif
}
