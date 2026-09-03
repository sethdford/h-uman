/* 5e — gate default and attach contract. No network: attach never embeds. */
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/memory/retrieval.h"
#include "human/memory/semantic_recall.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

static void test_gate_defaults_off_and_parses(void) {
    unsetenv("HU_SEMANTIC_RECALL");
    HU_ASSERT_EQ((int)hu_semantic_recall_mode(), (int)HU_GATE_OFF);
    setenv("HU_SEMANTIC_RECALL", "shadow", 1);
    HU_ASSERT_EQ((int)hu_semantic_recall_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_SEMANTIC_RECALL", "live", 1);
    HU_ASSERT_EQ((int)hu_semantic_recall_mode(), (int)HU_GATE_LIVE);
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_SEMANTIC_EMBED_URL");
    HU_ASSERT_STR_EQ(hu_semantic_recall_embed_url(), "http://127.0.0.1:8741");
    setenv("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8749", 1);
    HU_ASSERT_STR_EQ(hu_semantic_recall_embed_url(), "http://127.0.0.1:8749");
    unsetenv("HU_SEMANTIC_EMBED_URL");
}

#ifdef HU_ENABLE_SQLITE
static void test_attach_to_sqlite_engine_creates_index_tables(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {0};
    hu_vector_store_t vs = {0};
    HU_ASSERT_EQ(hu_semantic_recall_attach(&alloc, &mem, &emb, &vs), HU_OK);
    HU_ASSERT_NOT_NULL(emb.ctx);
    HU_ASSERT_NOT_NULL(vs.ctx);
    HU_ASSERT_EQ((long)vs.vtable->count(vs.ctx), 0L);
    /* A store() now tries to embed over HTTP; the test transport is a mock, so
     * the index insert fails and is LOGGED, but the row itself must be stored. */
    HU_ASSERT_EQ(mem.vtable->store(mem.ctx, "k", 1, "hello", 5, NULL, "", 0), HU_OK);
    vs.vtable->deinit(vs.ctx, &alloc);
    emb.vtable->deinit(emb.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}
#endif

/* ── Recall byte budget (2026-09-02 live-gate finding: 9/40 LIVE contexts
 * returned an EMPTY completion; the LIVE arm differs from SHADOW only by
 * the appended recall block, each hit up to 2000 chars). The block is
 * bounded at the source: per-hit content is cut at a word boundary and
 * the whole semantic leg is capped at a byte budget. ───────────────── */

static void test_max_bytes_default_and_env_override(void) {
    unsetenv("HU_SEMANTIC_RECALL_MAX_BYTES");
    HU_ASSERT_EQ((long)hu_semantic_recall_max_bytes(), 1200L);
    setenv("HU_SEMANTIC_RECALL_MAX_BYTES", "600", 1);
    HU_ASSERT_EQ((long)hu_semantic_recall_max_bytes(), 600L);
    /* Unparsable / non-positive values fail closed to the default. */
    setenv("HU_SEMANTIC_RECALL_MAX_BYTES", "banana", 1);
    HU_ASSERT_EQ((long)hu_semantic_recall_max_bytes(), 1200L);
    setenv("HU_SEMANTIC_RECALL_MAX_BYTES", "0", 1);
    HU_ASSERT_EQ((long)hu_semantic_recall_max_bytes(), 1200L);
    unsetenv("HU_SEMANTIC_RECALL_MAX_BYTES");
}

static void test_truncate_hit_cuts_at_word_boundary(void) {
    const char *s = "the quick brown fox jumps over";
    size_t n = strlen(s);
    /* Under the limit: untouched. */
    HU_ASSERT_EQ((long)hu_semantic_recall_truncate_len(s, n, 64), (long)n);
    /* "the quick brown fox jumps over" cut at 12 -> last space in the
     * upper half of [0,12] is after "quick" (index 9). */
    HU_ASSERT_EQ((long)hu_semantic_recall_truncate_len(s, n, 12), 9L);
    /* No space in the upper half -> hard cut at max (never over). */
    HU_ASSERT_EQ((long)hu_semantic_recall_truncate_len("abcdefghijklmnop", 16, 8), 8L);
    /* Degenerate inputs. */
    HU_ASSERT_EQ((long)hu_semantic_recall_truncate_len(NULL, 5, 8), 0L);
    HU_ASSERT_EQ((long)hu_semantic_recall_truncate_len(s, n, 0), 0L);
}

static void fill_result(hu_allocator_t *alloc, hu_retrieval_result_t *r, size_t count,
                        size_t content_len) {
    r->entries = (hu_memory_entry_t *)alloc->alloc(alloc->ctx, count * sizeof(hu_memory_entry_t));
    r->scores = (double *)alloc->alloc(alloc->ctx, count * sizeof(double));
    memset(r->entries, 0, count * sizeof(hu_memory_entry_t));
    for (size_t i = 0; i < count; i++) {
        char *c = (char *)alloc->alloc(alloc->ctx, content_len + 1);
        /* Words of 7 letters + space so every hit has word boundaries. */
        for (size_t k = 0; k < content_len; k++)
            c[k] = (k % 8 == 7) ? ' ' : (char)('a' + (int)(i % 26));
        c[content_len] = '\0';
        r->entries[i].content = c;
        r->entries[i].content_len = content_len;
        r->entries[i].key = hu_strndup(alloc, "k", 1);
        r->entries[i].key_len = 1;
        r->scores[i] = 1.0 - (double)i * 0.1;
    }
    r->count = count;
}

static void test_clamp_result_over_budget_is_deterministic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_retrieval_result_t r = {0};
    fill_result(&alloc, &r, 5, 500);
    size_t kept = hu_semantic_recall_clamp_result(&alloc, &r, 700, 240);
    /* Two hits of <=240 fit in 700; the third would push past it -> dropped. */
    HU_ASSERT_EQ((long)r.count, 2L);
    HU_ASSERT_LE((long)kept, 700L);
    for (size_t i = 0; i < r.count; i++) {
        HU_ASSERT_LE((long)r.entries[i].content_len, 240L);
        HU_ASSERT_EQ((long)strlen(r.entries[i].content), (long)r.entries[i].content_len);
        /* Word boundary: the cut never ends on a mid-word byte before a letter. */
        HU_ASSERT(r.entries[i].content[r.entries[i].content_len - 1] != ' ');
    }
    /* Scores stay aligned with the surviving entries. */
    HU_ASSERT_EQ((long)(r.scores[1] * 10.0 + 0.5), 9L);

    /* Determinism: an identical input yields byte-identical output. */
    hu_retrieval_result_t r2 = {0};
    fill_result(&alloc, &r2, 5, 500);
    size_t kept2 = hu_semantic_recall_clamp_result(&alloc, &r2, 700, 240);
    HU_ASSERT_EQ((long)kept2, (long)kept);
    HU_ASSERT_EQ((long)r2.count, (long)r.count);
    for (size_t i = 0; i < r.count; i++) {
        HU_ASSERT_EQ((long)r2.entries[i].content_len, (long)r.entries[i].content_len);
        HU_ASSERT(memcmp(r2.entries[i].content, r.entries[i].content, r.entries[i].content_len) ==
                  0);
    }
    hu_retrieval_result_free(&alloc, &r);
    hu_retrieval_result_free(&alloc, &r2);
}

static void test_clamp_result_under_budget_is_untouched(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_retrieval_result_t r = {0};
    fill_result(&alloc, &r, 3, 100);
    size_t kept = hu_semantic_recall_clamp_result(&alloc, &r, 1200, 240);
    HU_ASSERT_EQ((long)r.count, 3L);
    HU_ASSERT_EQ((long)kept, 300L);
    HU_ASSERT_EQ((long)r.entries[2].content_len, 100L);
    hu_retrieval_result_free(&alloc, &r);
}

static void test_clamp_result_preserves_embedded_nul_binary_safe(void) {
    /* Recalled content is binary-safe by contract (content_len, never strlen).
     * A NUL before the cut must not shrink the copy while content_len still
     * claims the full cut — that is the 2026-07-13 memory-loader overflow
     * shape. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_retrieval_result_t r = {0};
    fill_result(&alloc, &r, 1, 500);
    char *c = (char *)r.entries[0].content;
    c[5] = '\0'; /* embedded NUL well before any 240-byte cut */
    char expect[240];
    memcpy(expect, c, sizeof(expect));
    size_t kept = hu_semantic_recall_clamp_result(&alloc, &r, 1200, 240);
    HU_ASSERT_EQ((long)r.count, 1L);
    HU_ASSERT_LE((long)r.entries[0].content_len, 240L);
    HU_ASSERT_EQ((long)kept, (long)r.entries[0].content_len);
    /* Every byte up to content_len is readable and identical to the source
     * (ASan flags a short allocation here). */
    HU_ASSERT(memcmp(r.entries[0].content, expect, r.entries[0].content_len) == 0);
    HU_ASSERT_EQ((int)r.entries[0].content[r.entries[0].content_len], 0);
    hu_retrieval_result_free(&alloc, &r);
}

void run_semantic_recall_tests(void) {
    HU_TEST_SUITE("semantic_recall");
    HU_RUN_TEST(test_gate_defaults_off_and_parses);
    HU_RUN_TEST(test_max_bytes_default_and_env_override);
    HU_RUN_TEST(test_truncate_hit_cuts_at_word_boundary);
    HU_RUN_TEST(test_clamp_result_over_budget_is_deterministic);
    HU_RUN_TEST(test_clamp_result_under_budget_is_untouched);
    HU_RUN_TEST(test_clamp_result_preserves_embedded_nul_binary_safe);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_attach_to_sqlite_engine_creates_index_tables);
#endif
}
