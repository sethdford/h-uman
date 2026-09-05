/* test_semantic_recall_register.c — register-conditioned semantic recall gate (US-5).
 * Word-count proxy limitation (Finding 4): the 12-word boundary is a heuristic, not precise.
 * Contexts like "multi-word phrase" (1 word in split() but 2 in hyphenation) or contractions
 * (can't, won't) may fall slightly above or below the intended threshold. This proxy is used
 * in both C and Python for practical efficiency; a more precise register classifier (e.g.
 * token-based) would require additional infrastructure. Acceptable for gating a suppression
 * feature that can be refined later. */
#include "human/memory/semantic_recall.h"

#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include "human/memory/vector/store_sqlite_vec.h"
#include <math.h>
#include <stdio.h>
#endif

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Pure predicate tests (no #ifdef, always compiled). */

static void test_register_admits_boundary_12_words_is_casual(void) {
    /* Exactly 12 words -> casual -> false */
    const char *query = "one two three four five six seven eight nine ten eleven twelve";
    size_t query_len = strlen(query);
    HU_ASSERT_FALSE(hu_semantic_recall_register_admits(query, query_len));
}

static void test_register_admits_boundary_13_words_is_substantive(void) {
    /* Exactly 13 words -> substantive -> true */
    const char *query = "one two three four five six seven eight nine ten eleven twelve thirteen";
    size_t query_len = strlen(query);
    HU_ASSERT_TRUE(hu_semantic_recall_register_admits(query, query_len));
}

static void test_register_admits_short_casual_input_suppressed(void) {
    /* Short query (3 words) -> casual -> false */
    const char *query = "yo what's up";
    size_t query_len = strlen(query);
    HU_ASSERT_FALSE(hu_semantic_recall_register_admits(query, query_len));
}

static void test_register_admits_long_substantive_input_admitted(void) {
    /* Long realistic query (>12 words) -> substantive -> true */
    const char *query = "I've been thinking about what makes a conversation feel natural and "
                        "I'm curious if you have any thoughts on that";
    size_t query_len = strlen(query);
    HU_ASSERT_TRUE(hu_semantic_recall_register_admits(query, query_len));
}

static void test_register_admits_null_fails_closed(void) {
    /* NULL / empty -> fail closed to casual -> false */
    HU_ASSERT_FALSE(hu_semantic_recall_register_admits(NULL, 0));
}

static void test_register_admits_empty_string_fails_closed(void) {
    /* Empty string -> fail closed to casual -> false */
    const char *query = "";
    size_t query_len = 0;
    HU_ASSERT_FALSE(hu_semantic_recall_register_admits(query, query_len));
}

static void test_register_admits_whitespace_only_fails_closed(void) {
    /* Whitespace only (0 words) -> fail closed to casual -> false */
    const char *query = "   \n\t  ";
    size_t query_len = strlen(query);
    HU_ASSERT_FALSE(hu_semantic_recall_register_admits(query, query_len));
}

static void test_register_admits_extra_whitespace_does_not_inflate_count(void) {
    /* Irregular spacing should not inflate word count (Python str.split() semantics) */
    const char *query = "  hi   there  "; /* 2 words, irregular spacing */
    size_t query_len = strlen(query);
    HU_ASSERT_FALSE(hu_semantic_recall_register_admits(query, query_len));
}

static void test_register_gate_mode_default_off(void) {
    /* Unset env var -> default OFF */
    unsetenv("HU_SEMANTIC_RECALL_REGISTER_GATE");
    hu_gate_mode_t mode = hu_semantic_recall_register_gate_mode();
    HU_ASSERT_EQ(mode, HU_GATE_OFF);
}

static void test_register_gate_mode_parses_shadow(void) {
    /* Set to "shadow" -> HU_GATE_SHADOW */
    setenv("HU_SEMANTIC_RECALL_REGISTER_GATE", "shadow", 1);
    hu_gate_mode_t mode = hu_semantic_recall_register_gate_mode();
    HU_ASSERT_EQ(mode, HU_GATE_SHADOW);
    unsetenv("HU_SEMANTIC_RECALL_REGISTER_GATE");
}

static void test_register_gate_mode_parses_live(void) {
    /* Set to "live" -> HU_GATE_LIVE */
    setenv("HU_SEMANTIC_RECALL_REGISTER_GATE", "live", 1);
    hu_gate_mode_t mode = hu_semantic_recall_register_gate_mode();
    HU_ASSERT_EQ(mode, HU_GATE_LIVE);
    unsetenv("HU_SEMANTIC_RECALL_REGISTER_GATE");
}

#ifdef HU_ENABLE_SQLITE

/* ---- stub embedder (deterministic 3-dim), lifted from test_hybrid_reconstructive.c ---- */
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

/* Helper to store a memory row with known content. */
static hu_error_t store_row(hu_memory_t *mem, const char *key, const char *content) {
    return mem->vtable->store(mem->ctx, key, strlen(key), content, strlen(content), NULL, NULL, 0);
}

/* Helper to check total recall bytes in result. */
static size_t result_total_recall_bytes(const hu_retrieval_result_t *r) {
    size_t total = 0;
    for (size_t i = 0; i < r->count; i++)
        if (r->entries[i].content)
            total += strlen(r->entries[i].content);
    return total;
}

/* AC-5.1: register gate LIVE + casual query (≤12 words) -> suppress semantic recall. */
static void test_hybrid_retrieve_register_gate_live_suppresses_casual_turn(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);

    /* Store memories that will match on semantic embedding but not keyword search. */
    HU_ASSERT_EQ(store_row(&mem, "mem1", "xyzabc went to the coffee shop"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "mem2", "xyzbcd likes to cook pizza"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "mem3", "xyzdef enjoys reading books"), HU_OK);

    /* Query: "xyz" (1 word) - casual, should be suppressed in LIVE mode.
     * This query won't match via keyword search but will match via semantic embedding. */
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_SEMANTIC_RECALL_REGISTER_GATE");
    setenv("HU_SEMANTIC_RECALL", "live", 1);
    setenv("HU_SEMANTIC_RECALL_REGISTER_GATE", "live", 1);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    hu_retrieval_result_t res_casual = {0};
    const char *q_casual = "xyz";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q_casual, strlen(q_casual),
                                    &opts, &res_casual),
                 HU_OK);

    /* In LIVE mode with register gate enabled, casual query should suppress semantic hits.
     * Since the query won't match keywords either, the result should be empty (0 bytes). */
    HU_ASSERT_EQ(result_total_recall_bytes(&res_casual), 0UL);

    hu_retrieval_result_free(&alloc, &res_casual);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_SEMANTIC_RECALL_REGISTER_GATE");
}

/* AC-5.2: register gate LIVE + substantive query (>12 words) -> admit semantic recall. */
static void test_hybrid_retrieve_register_gate_live_admits_substantive_turn(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);

    /* Store a memory with keyword. */
    HU_ASSERT_EQ(store_row(&mem, "mem1", "store alice went shopping at the store yesterday"),
                 HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "mem2", "unrelated content here"), HU_OK);

    /* Query: >12 words - substantive, should NOT be suppressed in LIVE mode. */
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_SEMANTIC_RECALL_REGISTER_GATE");
    setenv("HU_SEMANTIC_RECALL", "live", 1);
    setenv("HU_SEMANTIC_RECALL_REGISTER_GATE", "live", 1);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    hu_retrieval_result_t res_subst = {0};
    const char *q_subst = "I went to the store yesterday and I bought some groceries there "
                          "what did I get";
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q_subst, strlen(q_subst), &opts,
                                    &res_subst),
                 HU_OK);

    /* In LIVE mode with register gate enabled, substantive query should include semantic hits. */
    HU_ASSERT_TRUE(result_total_recall_bytes(&res_subst) > 0UL);

    hu_retrieval_result_free(&alloc, &res_subst);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_SEMANTIC_RECALL_REGISTER_GATE");
}

/* AC-5.3: register gate SHADOW -> never changes output, just logs. */
static void test_hybrid_retrieve_register_gate_shadow_never_changes_output(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);

    HU_ASSERT_EQ(store_row(&mem, "mem1", "store alice shopping"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "mem2", "unrelated"), HU_OK);

    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_SEMANTIC_RECALL_REGISTER_GATE");
    setenv("HU_SEMANTIC_RECALL", "live", 1);

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    hu_retrieval_result_t res_off = {0};
    hu_retrieval_result_t res_shadow = {0};

    const char *q = "store"; /* 1 word, casual */

    /* First: OFF mode (default) */
    setenv("HU_SEMANTIC_RECALL_REGISTER_GATE", "off", 1);
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res_off),
                 HU_OK);
    size_t bytes_off = result_total_recall_bytes(&res_off);

    /* Second: SHADOW mode */
    setenv("HU_SEMANTIC_RECALL_REGISTER_GATE", "shadow", 1);
    HU_ASSERT_EQ(
        hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res_shadow), HU_OK);
    size_t bytes_shadow = result_total_recall_bytes(&res_shadow);

    /* SHADOW should NOT change the output compared to OFF. */
    HU_ASSERT_EQ(bytes_off, bytes_shadow);

    hu_retrieval_result_free(&alloc, &res_off);
    hu_retrieval_result_free(&alloc, &res_shadow);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_SEMANTIC_RECALL_REGISTER_GATE");
}

/* AC-5.5: register gate OFF (default) -> behavior unchanged from baseline. */
static void test_hybrid_retrieve_register_gate_off_default_unchanged(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    HU_ASSERT_NOT_NULL(vs.ctx);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);

    HU_ASSERT_EQ(store_row(&mem, "mem1", "store alice shopping yesterday"), HU_OK);
    HU_ASSERT_EQ(store_row(&mem, "mem2", "unrelated"), HU_OK);

    /* Unset register gate env var (defaults to OFF). */
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_SEMANTIC_RECALL_REGISTER_GATE");
    setenv("HU_SEMANTIC_RECALL", "live", 1);
    /* Leave HU_SEMANTIC_RECALL_REGISTER_GATE unset - defaults to OFF */

    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    hu_retrieval_result_t res = {0};

    const char *q = "store"; /* 1 word, casual */
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res),
                 HU_OK);

    /* With register gate OFF (default), casual query should still return semantic results. */
    HU_ASSERT_TRUE(result_total_recall_bytes(&res) > 0UL);

    hu_retrieval_result_free(&alloc, &res);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
    unsetenv("HU_SEMANTIC_RECALL");
}

#else

/* Stub runners for non-SQLite builds, per test-source-gate-symmetry.md. */
static void test_hybrid_retrieve_register_gate_live_suppresses_casual_turn(void) {
    (void)0;
}
static void test_hybrid_retrieve_register_gate_live_admits_substantive_turn(void) {
    (void)0;
}
static void test_hybrid_retrieve_register_gate_shadow_never_changes_output(void) {
    (void)0;
}
static void test_hybrid_retrieve_register_gate_off_default_unchanged(void) {
    (void)0;
}

#endif

void run_semantic_recall_register_tests(void) {
    HU_TEST_SUITE("semantic_recall_register");
    HU_RUN_TEST(test_register_admits_boundary_12_words_is_casual);
    HU_RUN_TEST(test_register_admits_boundary_13_words_is_substantive);
    HU_RUN_TEST(test_register_admits_short_casual_input_suppressed);
    HU_RUN_TEST(test_register_admits_long_substantive_input_admitted);
    HU_RUN_TEST(test_register_admits_null_fails_closed);
    HU_RUN_TEST(test_register_admits_empty_string_fails_closed);
    HU_RUN_TEST(test_register_admits_whitespace_only_fails_closed);
    HU_RUN_TEST(test_register_admits_extra_whitespace_does_not_inflate_count);
    HU_RUN_TEST(test_register_gate_mode_default_off);
    HU_RUN_TEST(test_register_gate_mode_parses_shadow);
    HU_RUN_TEST(test_register_gate_mode_parses_live);
    HU_RUN_TEST(test_hybrid_retrieve_register_gate_live_suppresses_casual_turn);
    HU_RUN_TEST(test_hybrid_retrieve_register_gate_live_admits_substantive_turn);
    HU_RUN_TEST(test_hybrid_retrieve_register_gate_shadow_never_changes_output);
    HU_RUN_TEST(test_hybrid_retrieve_register_gate_off_default_unchanged);
}
