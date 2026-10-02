/* test_context_relevance.c — HU_CONTEXT_RELEVANCE: a relevance decision in
 * place of the word-count cliff that dropped semantic recall on short turns.
 * The graph-grounding half is exercised through the real loader in
 * test_graph_grounding.c (its fixture is file-local there). */
#include "human/memory/context_relevance.h"

#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/memory/semantic_recall.h"
#include "test_framework.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"
#include "human/memory/vector/store_sqlite_vec.h"
#endif

static void clear_env(void) {
    unsetenv("HU_CONTEXT_RELEVANCE");
    unsetenv("HU_CONTEXT_RELEVANCE_MIN_SCORE");
    unsetenv("HU_CONTEXT_RELEVANCE_CASUAL_BYTES");
    hu_context_relevance_null_reset();
}

/* Build a result of n entries with the given contents and scores. */
static void make_result(hu_allocator_t *a, hu_retrieval_result_t *r, const char **contents,
                        const double *scores, size_t n) {
    r->entries = (hu_memory_entry_t *)a->alloc(a->ctx, n * sizeof(hu_memory_entry_t));
    r->scores = (double *)a->alloc(a->ctx, n * sizeof(double));
    memset(r->entries, 0, n * sizeof(hu_memory_entry_t));
    for (size_t i = 0; i < n; i++) {
        char key[8] = {'k', (char)('0' + (int)i), 0};
        r->entries[i].key = hu_strndup(a, key, 2);
        r->entries[i].key_len = 2;
        r->entries[i].id = r->entries[i].key;
        r->entries[i].id_len = 2;
        r->entries[i].content = hu_strndup(a, contents[i], strlen(contents[i]));
        r->entries[i].content_len = strlen(contents[i]);
        r->entries[i].score = scores[i];
        r->scores[i] = scores[i];
    }
    r->count = n;
}

static void test_mode_defaults_off_and_parses(void) {
    clear_env();
    HU_ASSERT_EQ((int)hu_context_relevance_mode(), (int)HU_GATE_OFF);
    setenv("HU_CONTEXT_RELEVANCE", "shadow", 1);
    HU_ASSERT_EQ((int)hu_context_relevance_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_CONTEXT_RELEVANCE", "live", 1);
    HU_ASSERT_EQ((int)hu_context_relevance_mode(), (int)HU_GATE_LIVE);
    setenv("HU_CONTEXT_RELEVANCE", "garbage", 1);
    HU_ASSERT_EQ((int)hu_context_relevance_mode(), (int)HU_GATE_OFF);
    clear_env();
}

static void test_min_score_default_and_override(void) {
    clear_env();
    HU_ASSERT_TRUE(fabs(hu_context_relevance_min_score() - HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE) <
                   1e-9);
    setenv("HU_CONTEXT_RELEVANCE_MIN_SCORE", "0.62", 1);
    HU_ASSERT_TRUE(fabs(hu_context_relevance_min_score() - 0.62) < 1e-9);
    setenv("HU_CONTEXT_RELEVANCE_MIN_SCORE", "1.5", 1); /* out of cosine range */
    HU_ASSERT_TRUE(fabs(hu_context_relevance_min_score() - HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE) <
                   1e-9);
    setenv("HU_CONTEXT_RELEVANCE_MIN_SCORE", "abc", 1);
    HU_ASSERT_TRUE(fabs(hu_context_relevance_min_score() - HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE) <
                   1e-9);
    clear_env();
    HU_ASSERT_EQ(hu_context_relevance_casual_bytes(),
                 (size_t)HU_CONTEXT_RELEVANCE_DEFAULT_CASUAL_BYTES);
    setenv("HU_CONTEXT_RELEVANCE_CASUAL_BYTES", "100", 1);
    HU_ASSERT_EQ(hu_context_relevance_casual_bytes(), (size_t)100);
    setenv("HU_CONTEXT_RELEVANCE_CASUAL_BYTES", "0", 1);
    HU_ASSERT_EQ(hu_context_relevance_casual_bytes(),
                 (size_t)HU_CONTEXT_RELEVANCE_DEFAULT_CASUAL_BYTES);
    clear_env();
}

static void test_assess_counts_passing_items_within_budget(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_retrieval_result_t r = {0};
    const char *c[] = {"mel started the new job at the bakery", "her dog is called biscuit",
                       "unrelated note about taxes", "Task: x\nActions: y\nOutcome: z\nScore: 1"};
    const double s[] = {0.81, 0.55, 0.20, 0.95};
    make_result(&a, &r, c, s, 4);
    /* the 4th is the experience scaffold: excluded by the recall content policy */
    hu_context_relevance_stats_t st;
    hu_context_relevance_assess(&r, 0.46, 1000, 240, &st);
    HU_ASSERT_EQ(st.items_considered, (size_t)3);
    HU_ASSERT_EQ(st.items_passing, (size_t)2);
    HU_ASSERT_EQ(st.bytes_would_inject, strlen(c[0]) + strlen(c[1]));
    HU_ASSERT_TRUE(fabs(st.top_score - 0.81) < 1e-9);
    HU_ASSERT_EQ(st.hist[4], 1u);  /* 0.20 -> [0.20, 0.25) */
    HU_ASSERT_EQ(st.hist[11], 1u); /* 0.55 -> [0.55, 0.60) */
    HU_ASSERT_EQ(st.hist[16], 1u); /* 0.81 -> [0.80, 0.85) */
    /* A budget that fits only the first passing item: same rule as the clamp. */
    hu_context_relevance_assess(&r, 0.46, strlen(c[0]) + 3, 240, &st);
    HU_ASSERT_EQ(st.bytes_would_inject, strlen(c[0]));
    /* assess never mutates */
    HU_ASSERT_EQ(r.count, (size_t)4);
    hu_retrieval_result_free(&a, &r);
}

static void test_drop_below_keeps_rank_and_scores_aligned(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_retrieval_result_t r = {0};
    const char *c[] = {"first", "second", "third"};
    const double s[] = {0.9, 0.1, 0.5};
    make_result(&a, &r, c, s, 3);
    HU_ASSERT_EQ(hu_semantic_recall_drop_below(&a, &r, 0.46), (size_t)1);
    HU_ASSERT_EQ(r.count, (size_t)2);
    HU_ASSERT_STR_EQ(r.entries[0].content, "first");
    HU_ASSERT_STR_EQ(r.entries[1].content, "third");
    HU_ASSERT_TRUE(fabs(r.scores[1] - 0.5) < 1e-9);
    HU_ASSERT_EQ(hu_semantic_recall_drop_below(&a, &r, 0.95), (size_t)2);
    HU_ASSERT_EQ(r.count, (size_t)0);
    HU_ASSERT_TRUE(r.entries == NULL);
    hu_retrieval_result_free(&a, &r);
}

static void test_line_cut_keeps_whole_lines(void) {
    const char *s = "- sailboat (topic)\n  - docked at slip 14\n- guitar\n";
    HU_ASSERT_EQ(hu_context_relevance_line_cut(s, strlen(s), 1000), strlen(s));
    HU_ASSERT_EQ(hu_context_relevance_line_cut(s, strlen(s), 25), strlen("- sailboat (topic)\n"));
    HU_ASSERT_EQ(hu_context_relevance_line_cut(s, strlen(s), 5), (size_t)0);
    HU_ASSERT_EQ(hu_context_relevance_line_cut(NULL, 0, 5), (size_t)0);
}

/* LIVE on a casual turn: below-threshold hits leave, the budget shrinks to the
 * casual budget. OFF / SHADOW: nothing touched. */
static void test_semantic_decision_by_mode(void) {
    hu_allocator_t a = hu_system_allocator();
    const char *c[] = {"mel started the new job at the bakery", "unrelated note about taxes"};
    const double s[] = {0.81, 0.20};
    clear_env();
    for (int m = 0; m < 3; m++) {
        hu_retrieval_result_t r = {0};
        make_result(&a, &r, c, s, 2);
        size_t budget = 1200;
        setenv("HU_CONTEXT_RELEVANCE", m == 0 ? "off" : (m == 1 ? "shadow" : "live"), 1);
        bool decided = hu_context_relevance_semantic(&a, &r, true, &budget, NULL, NULL);
        if (m < 2) {
            HU_ASSERT_FALSE(decided);
            HU_ASSERT_EQ(r.count, (size_t)2);
            HU_ASSERT_EQ(budget, (size_t)1200);
        } else {
            HU_ASSERT_TRUE(decided);
            HU_ASSERT_EQ(r.count, (size_t)1);
            HU_ASSERT_EQ(budget, (size_t)HU_CONTEXT_RELEVANCE_DEFAULT_CASUAL_BYTES);
        }
        hu_retrieval_result_free(&a, &r);
    }
    /* A substantive turn under LIVE is untouched until the threshold is
     * calibrated: no hit dropped, full budget, no decision taken. */
    hu_retrieval_result_t r = {0};
    make_result(&a, &r, c, s, 2);
    size_t budget = 1200;
    HU_ASSERT_FALSE(hu_context_relevance_semantic(&a, &r, false, &budget, NULL, NULL));
    HU_ASSERT_EQ(r.count, (size_t)2);
    HU_ASSERT_EQ(budget, (size_t)1200);
    hu_retrieval_result_free(&a, &r);
    clear_env();
}

static void test_p95_is_nearest_rank(void) {
    float v[20];
    for (int i = 0; i < 20; i++)
        v[i] = (float)(19 - i) / 20.0f; /* 0.95 .. 0.00, unsorted order */
    HU_ASSERT_TRUE(fabs(hu_context_relevance_p95(v, 20) - 0.90) < 1e-6);
    HU_ASSERT_TRUE(fabs(v[0] - 0.95f) < 1e-6); /* input not reordered */
    HU_ASSERT_TRUE(hu_context_relevance_p95(v, 0) == 0.0);
}

/* The threshold is the null pool's p95 once the pool is full enough; the env
 * overrides it; the 0.46 doc-to-doc number is only the cold start. */
static void test_threshold_null_pool_then_env(void) {
    clear_env();
    hu_context_relevance_threshold_src_t src = HU_CR_THRESHOLD_ENV;
    HU_ASSERT_TRUE(
        fabs(hu_context_relevance_threshold(&src) - HU_CONTEXT_RELEVANCE_DEFAULT_MIN_SCORE) < 1e-9);
    HU_ASSERT_EQ((int)src, (int)HU_CR_THRESHOLD_COLD);
    float nulls[HU_CONTEXT_RELEVANCE_NULL_MIN];
    for (size_t i = 0; i < HU_CONTEXT_RELEVANCE_NULL_MIN; i++)
        nulls[i] = 0.10f + 0.20f * (float)i / (float)HU_CONTEXT_RELEVANCE_NULL_MIN; /* 0.10..0.30 */
    hu_context_relevance_null_add(nulls, HU_CONTEXT_RELEVANCE_NULL_MIN - 1);
    (void)hu_context_relevance_threshold(&src);
    HU_ASSERT_EQ((int)src, (int)HU_CR_THRESHOLD_COLD); /* one short of the minimum */
    hu_context_relevance_null_add(nulls + HU_CONTEXT_RELEVANCE_NULL_MIN - 1, 1);
    HU_ASSERT_EQ(hu_context_relevance_null_count(), (size_t)HU_CONTEXT_RELEVANCE_NULL_MIN);
    double t = hu_context_relevance_threshold(&src);
    HU_ASSERT_EQ((int)src, (int)HU_CR_THRESHOLD_NULL);
    HU_ASSERT_TRUE(t > 0.28 && t < 0.30); /* p95 of 0.10..0.30, not 0.46 */
    setenv("HU_CONTEXT_RELEVANCE_MIN_SCORE", "0.62", 1);
    HU_ASSERT_TRUE(fabs(hu_context_relevance_threshold(&src) - 0.62) < 1e-9);
    HU_ASSERT_EQ((int)src, (int)HU_CR_THRESHOLD_ENV);
    /* the pool is bounded: old scores roll out */
    for (int k = 0; k < 20; k++)
        hu_context_relevance_null_add(nulls, HU_CONTEXT_RELEVANCE_NULL_MIN);
    HU_ASSERT_EQ(hu_context_relevance_null_count(), (size_t)HU_CONTEXT_RELEVANCE_NULL_POOL);
    clear_env();
    HU_ASSERT_EQ(hu_context_relevance_null_count(), (size_t)0);
}

#ifdef HU_ENABLE_SQLITE

/* Deterministic 3-dim stub embedder (same as test_semantic_recall_register.c):
 * the class of the first byte decides the direction, so rows starting with
 * 'x' score 1.0 against the query "xq" and a row starting with 'b' ~0.21. */
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

/* Run the REAL hybrid retrieve (prod config: recall live + register gate live)
 * on a casual one-word message and serialize what comes back. */
static size_t recall_q(const char *relevance_mode, const char *q, char *dump, size_t dump_cap) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    hu_embedder_t emb = {.ctx = NULL, .vtable = &stub_vt};
    hu_vector_store_t vs =
        hu_vector_store_sqlite_vec_create(&alloc, hu_sqlite_memory_get_db(&mem), 3);
    hu_sqlite_memory_set_semantic_index(&mem, &emb, &vs);
    const char *rows[][2] = {{"m1", "xavier moved to denver for the new job in june"},
                             {"m2", "xmas plans are at her sister's place this year"},
                             {"m3", "bills are due on the first of every month"}};
    for (size_t i = 0; i < 3; i++)
        HU_ASSERT_EQ(mem.vtable->store(mem.ctx, rows[i][0], 2, rows[i][1], strlen(rows[i][1]), NULL,
                                       NULL, 0),
                     HU_OK);
    setenv("HU_SEMANTIC_RECALL", "live", 1);
    setenv("HU_SEMANTIC_RECALL_REGISTER_GATE", "live", 1);
    if (relevance_mode)
        setenv("HU_CONTEXT_RELEVANCE", relevance_mode, 1);
    else
        unsetenv("HU_CONTEXT_RELEVANCE");
    hu_retrieval_options_t opts = {0};
    opts.limit = 10;
    hu_retrieval_result_t res = {0};
    HU_ASSERT_EQ(hu_hybrid_retrieve(&alloc, &mem, &emb, &vs, NULL, q, strlen(q), &opts, &res),
                 HU_OK);
    size_t n = 0, bytes = 0;
    dump[0] = '\0';
    for (size_t i = 0; i < res.count; i++) {
        const hu_memory_entry_t *e = &res.entries[i];
        if (e->content)
            bytes += e->content_len;
        int w = snprintf(dump + n, dump_cap - n, "%.*s=%.*s|", (int)e->key_len, e->key,
                         (int)e->content_len, e->content ? e->content : "");
        if (w > 0 && (size_t)w < dump_cap - n)
            n += (size_t)w;
    }
    hu_retrieval_result_free(&alloc, &res);
    hu_sqlite_memory_set_semantic_index(&mem, NULL, NULL);
    vs.vtable->deinit(vs.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
    unsetenv("HU_SEMANTIC_RECALL");
    unsetenv("HU_SEMANTIC_RECALL_REGISTER_GATE");
    unsetenv("HU_CONTEXT_RELEVANCE");
    unsetenv("HU_CONTEXT_RELEVANCE_MIN_SCORE");
    return bytes;
}

/* One word, casual; matches no keyword. */
static size_t casual_recall(const char *relevance_mode, char *dump, size_t dump_cap) {
    return recall_q(relevance_mode, "xq", dump, dump_cap);
}

/* Headline: a short message used to get ZERO recall (the word-count cliff);
 * under LIVE it gets the relevant memories and not the irrelevant one. */
static void test_hybrid_casual_turn_recalls_relevant_memories_under_live(void) {
    clear_env();
    char before[1024], after[1024];
    HU_ASSERT_EQ(casual_recall(NULL, before, sizeof(before)), (size_t)0); /* the cliff */
    size_t live_bytes = casual_recall("live", after, sizeof(after));
    HU_ASSERT_TRUE(live_bytes > 0);
    HU_ASSERT_TRUE(live_bytes <= (size_t)HU_CONTEXT_RELEVANCE_DEFAULT_CASUAL_BYTES);
    HU_ASSERT_TRUE(strstr(after, "denver") != NULL);
    HU_ASSERT_TRUE(strstr(after, "bills") == NULL); /* below the relevance threshold */
}

/* OFF is byte-identical to the gate being unset; SHADOW changes nothing. */
static void test_hybrid_off_and_shadow_are_byte_identical(void) {
    clear_env();
    char unset[1024], off[1024], shadow[1024];
    size_t b0 = casual_recall(NULL, unset, sizeof(unset));
    size_t b1 = casual_recall("off", off, sizeof(off));
    size_t b2 = casual_recall("shadow", shadow, sizeof(shadow));
    HU_ASSERT_EQ(b0, b1);
    HU_ASSERT_EQ(b0, b2);
    HU_ASSERT_STR_EQ(unset, off);
    HU_ASSERT_STR_EQ(unset, shadow);
    /* OFF samples nothing; SHADOW scored the query against the 3 stored
     * vectors (the calibration sample) */
    HU_ASSERT_EQ(hu_context_relevance_null_count(), (size_t)3);
    clear_env();
    (void)casual_recall("off", off, sizeof(off));
    HU_ASSERT_EQ(hu_context_relevance_null_count(), (size_t)0);
}

/* Substantive turns are unchanged under LIVE until the threshold is
 * calibrated: the low-scoring 'bills' row stays, exactly as under OFF. */
static void test_hybrid_live_leaves_substantive_turns_unchanged(void) {
    clear_env();
    const char *q = "xq what was it she said about moving away for that new job again";
    char off[1024], live[1024];
    size_t b_off = recall_q(NULL, q, off, sizeof(off));
    size_t b_live = recall_q("live", q, live, sizeof(live));
    HU_ASSERT_TRUE(strstr(off, "bills") != NULL);
    HU_ASSERT_EQ(b_off, b_live);
    HU_ASSERT_STR_EQ(off, live);
    clear_env();
}

/* The threshold comes from the env when set: lowered to 0.1, the 'b' row
 * (cosine ~0.21) is relevant too. */
static void test_hybrid_live_threshold_override_is_honoured(void) {
    clear_env();
    char dump[1024];
    setenv("HU_CONTEXT_RELEVANCE_MIN_SCORE", "0.1", 1); /* casual_recall clears it */
    size_t b = casual_recall("live", dump, sizeof(dump));
    HU_ASSERT_TRUE(b > 0);
    HU_ASSERT_TRUE(strstr(dump, "bills") != NULL);
}

#else
static void test_hybrid_casual_turn_recalls_relevant_memories_under_live(void) {
    (void)0;
}
static void test_hybrid_off_and_shadow_are_byte_identical(void) {
    (void)0;
}
static void test_hybrid_live_threshold_override_is_honoured(void) {
    (void)0;
}
static void test_hybrid_live_leaves_substantive_turns_unchanged(void) {
    (void)0;
}
#endif

void run_context_relevance_tests(void) {
    HU_TEST_SUITE("context_relevance");
    HU_RUN_TEST(test_mode_defaults_off_and_parses);
    HU_RUN_TEST(test_min_score_default_and_override);
    HU_RUN_TEST(test_p95_is_nearest_rank);
    HU_RUN_TEST(test_threshold_null_pool_then_env);
    HU_RUN_TEST(test_assess_counts_passing_items_within_budget);
    HU_RUN_TEST(test_drop_below_keeps_rank_and_scores_aligned);
    HU_RUN_TEST(test_line_cut_keeps_whole_lines);
    HU_RUN_TEST(test_semantic_decision_by_mode);
    HU_RUN_TEST(test_hybrid_casual_turn_recalls_relevant_memories_under_live);
    HU_RUN_TEST(test_hybrid_off_and_shadow_are_byte_identical);
    HU_RUN_TEST(test_hybrid_live_threshold_override_is_honoured);
    HU_RUN_TEST(test_hybrid_live_leaves_substantive_turns_unchanged);
}
