/* tests/test_score_fusion.c — hu_rerank_score_fusion, hu_rerank_bm25_to_relevance
 * and the HU_HYBRID_FUSION / HU_HYBRID_FUSION_ALPHA gate parsers
 * (src/memory/rerank.c), the score-level alternative to hu_rerank_rrf that
 * hu_hybrid_retrieve uses when HU_HYBRID_FUSION=score (arXiv 2606.04194).
 *
 * Wiring through hu_hybrid_retrieve (default order == RRF, alpha extremes on a
 * real SQLite + sqlite-vec index) is pinned in tests/test_hybrid_reconstructive.c.
 */
#include "human/core/allocator.h"
#include "human/memory/rerank.h"
#include "test_framework.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Input rows are read-only to the fusion, so literals are fine here; only the
 * merged output owns strings (freed with hu_rerank_free_results). */
static hu_search_result_t row(const char *content, float score) {
    hu_search_result_t r;
    memset(&r, 0, sizeof(r));
    r.content = (char *)content;
    r.score = score;
    return r;
}

static size_t fuse(hu_search_result_t *kw, size_t kw_n, hu_search_result_t *sem, size_t sem_n,
                   float alpha, hu_search_result_t *out, size_t cap) {
    memset(out, 0, cap * sizeof(*out));
    size_t n = 99;
    HU_ASSERT_EQ(hu_rerank_score_fusion(kw, kw_n, sem, sem_n, out, cap, &n, alpha), HU_OK);
    return n;
}

/* The fusion reads score MAGNITUDES, RRF reads ranks. A and B are the top two
 * of both legs in opposite orders; lexically they are nearly tied (10 vs 9.9)
 * while densely B is far ahead of A (0.5 vs 0.1). RRF cannot see either gap
 * and puts A first (1/61 + 1/63 > 2/62); alpha=0.5 score fusion gives
 * A = 0.5*1 + 0.5*0 = 0.5, B = 0.5*0.99 + 0.5*0.5 = 0.745 and puts B first. */
static void test_score_fusion_reads_magnitudes_where_rrf_reads_ranks(void) {
    hu_search_result_t kw[3] = {row("A", 10.0f), row("B", 9.9f), row("C", 0.0f)};
    hu_search_result_t sem[3] = {row("C", 0.9f), row("B", 0.5f), row("A", 0.1f)};
    hu_search_result_t out[8];
    size_t n = fuse(kw, 3, sem, 3, 0.5f, out, 8);
    HU_ASSERT_EQ(n, 3u);
    HU_ASSERT_STR_EQ(out[0].content, "B");
    HU_ASSERT_FLOAT_EQ(out[0].rerank_score, 0.745f, 1e-4);
    hu_rerank_free_results(out, n);

    hu_search_result_t rrf_out[8];
    memset(rrf_out, 0, sizeof(rrf_out));
    size_t rn = 0;
    HU_ASSERT_EQ(hu_rerank_rrf(kw, 3, sem, 3, rrf_out, 8, &rn, 60.0f), HU_OK);
    HU_ASSERT_STR_EQ(rrf_out[0].content, "A");
    hu_rerank_free_results(rrf_out, rn);
}

/* SQLite bm25() is negative and lower-is-better. After the leg's conversion
 * the BEST hit (most negative engine score) normalises to 1.0 and the worst
 * to 0.0; a sign slip would put "worst" first. */
static void test_bm25_best_hit_normalises_to_one(void) {
    const double engine[3] = {-3.2, -1.1, -0.4};
    double rel[3] = {0};
    hu_rerank_bm25_to_relevance(engine, 3, rel);
    HU_ASSERT_FLOAT_EQ(rel[0], 3.2, 1e-9);
    HU_ASSERT_FLOAT_EQ(rel[1], 1.1, 1e-9);
    HU_ASSERT_FLOAT_EQ(rel[2], 0.4, 1e-9);

    hu_search_result_t kw[3] = {row("best", (float)rel[0]), row("mid", (float)rel[1]),
                                row("worst", (float)rel[2])};
    hu_search_result_t out[4];
    size_t n = fuse(kw, 3, NULL, 0, 0.0f, out, 4);
    HU_ASSERT_EQ(n, 3u);
    HU_ASSERT_STR_EQ(out[0].content, "best");
    HU_ASSERT_FLOAT_EQ(out[0].rerank_score, 1.0f, 1e-6);
    HU_ASSERT_STR_EQ(out[2].content, "worst");
    HU_ASSERT_FLOAT_EQ(out[2].rerank_score, 0.0f, 1e-6);
    hu_rerank_free_results(out, n);
}

/* Rows the engine appended after its FTS hits carry POSITIVE scores
 * (spreading activation energy*0.5, hierarchy 0.35): negated they would sit
 * below every FTS hit anyway, but a later FTS-looking score must never climb
 * above a row the engine ranked higher. NaN (LIKE fallback) ties upward. */
static void test_bm25_relevance_never_rises_along_recall_order(void) {
    const double engine[4] = {-2.0, 0.3, -1.5, NAN};
    double rel[4] = {0};
    hu_rerank_bm25_to_relevance(engine, 4, rel);
    HU_ASSERT_FLOAT_EQ(rel[0], 2.0, 1e-9);
    HU_ASSERT_FLOAT_EQ(rel[1], -0.3, 1e-9);
    HU_ASSERT_FLOAT_EQ(rel[2], -0.3, 1e-9); /* 1.5 clamped to the row above */
    HU_ASSERT_FLOAT_EQ(rel[3], -0.3, 1e-9); /* NaN ties upward */

    const double like_only[2] = {NAN, NAN};
    double rel2[2] = {7.0, 7.0};
    hu_rerank_bm25_to_relevance(like_only, 2, rel2);
    HU_ASSERT_FLOAT_EQ(rel2[0], 0.0, 1e-12);
    HU_ASSERT_FLOAT_EQ(rel2[1], 0.0, 1e-12);
}

/* A row present in only one leg scores 0 for the other. A (kw best, dense
 * absent) and C (dense best, kw absent) both fuse to 0.5; B (kw worst, dense
 * absent) to 0. The A/C tie breaks on RRF: A is in both lists (1/61 + 1/62)
 * and C only in one (1/61). */
static void test_score_fusion_single_leg_rows_score_zero_for_missing_leg(void) {
    hu_search_result_t kw[2] = {row("A", 5.0f), row("B", 1.0f)};
    hu_search_result_t sem[2] = {row("C", 0.9f), row("A", 0.2f)};
    hu_search_result_t out[8];
    size_t n = fuse(kw, 2, sem, 2, 0.5f, out, 8);
    HU_ASSERT_EQ(n, 3u);
    HU_ASSERT_STR_EQ(out[0].content, "A");
    HU_ASSERT_FLOAT_EQ(out[0].rerank_score, 0.5f, 1e-6);
    HU_ASSERT_STR_EQ(out[1].content, "C");
    HU_ASSERT_FLOAT_EQ(out[1].rerank_score, 0.5f, 1e-6);
    HU_ASSERT_STR_EQ(out[2].content, "B");
    HU_ASSERT_FLOAT_EQ(out[2].rerank_score, 0.0f, 1e-6);
    hu_rerank_free_results(out, n);
}

/* Same content in both legs (and twice in one leg) yields ONE merged row that
 * keeps the key of its first appearance; the in-leg duplicate keeps the best
 * of its scores. */
static void test_score_fusion_dedupes_by_content_and_carries_key(void) {
    hu_search_result_t kw[3] = {row("same", 2.0f), row("other", 1.0f), row("same", 0.0f)};
    kw[0].key = (char *)"s1:t1";
    kw[0].key_len = 5;
    hu_search_result_t sem[1] = {row("same", 0.8f)};
    sem[0].key = (char *)"s1:t1";
    sem[0].key_len = 5;
    hu_search_result_t out[8];
    size_t n = fuse(kw, 3, sem, 1, 0.5f, out, 8);
    HU_ASSERT_EQ(n, 2u);
    HU_ASSERT_STR_EQ(out[0].content, "same");
    HU_ASSERT_NOT_NULL(out[0].key);
    HU_ASSERT_STR_EQ(out[0].key, "s1:t1");
    HU_ASSERT_EQ(out[0].key_len, 5u);
    HU_ASSERT_FLOAT_EQ(out[0].rerank_score, 1.0f, 1e-6); /* lex 1 (best of 2.0/0.0), dense 1 */
    HU_ASSERT_FLOAT_EQ(out[0].score, 2.0f, 1e-6);        /* first appearance's original score */
    HU_ASSERT_STR_EQ(out[1].content, "other");
    HU_ASSERT_NULL(out[1].key);
    hu_rerank_free_results(out, n);
}

/* alpha=0 is the pure lexical order and alpha=1 the pure dense order: every
 * row of the weighted leg keeps its leg's relative order. Min-max puts each
 * leg's WORST row at 0 -- the same value an absent row gets -- so that row ties
 * with the other leg's rows and the RRF tie-break places it: at alpha=0,
 * "both" (kw 0, but in both lists) leads the zero block; at alpha=1, "s2"
 * (dense 0, one list, rank 3) trails k1 and k2 (ranks 1 and 2). */
static void test_score_fusion_alpha_extremes_are_the_single_leg_orders(void) {
    hu_search_result_t kw[3] = {row("k1", 3.0f), row("k2", 2.0f), row("both", 1.0f)};
    hu_search_result_t sem[3] = {row("both", 0.9f), row("s1", 0.6f), row("s2", 0.3f)};
    hu_search_result_t out[8];

    size_t n = fuse(kw, 3, sem, 3, 0.0f, out, 8);
    HU_ASSERT_EQ(n, 5u);
    const char *lexical[5] = {"k1", "k2", "both", "s1", "s2"};
    for (size_t i = 0; i < n; i++)
        HU_ASSERT_STR_EQ(out[i].content, lexical[i]);
    hu_rerank_free_results(out, n);

    n = fuse(kw, 3, sem, 3, 1.0f, out, 8);
    HU_ASSERT_EQ(n, 5u);
    const char *dense[5] = {"both", "s1", "k1", "k2", "s2"};
    for (size_t i = 0; i < n; i++)
        HU_ASSERT_STR_EQ(out[i].content, dense[i]);
    hu_rerank_free_results(out, n);
}

/* A leg whose scores are all equal gives every row 1.0; an empty leg
 * contributes nothing (alpha=1 over an empty dense leg fuses everything to 0
 * and the order falls back to RRF, i.e. the keyword order). */
static void test_score_fusion_flat_leg_is_one_and_empty_leg_is_zero(void) {
    hu_search_result_t kw[2] = {row("x", 2.0f), row("y", 2.0f)};
    hu_search_result_t out[4];
    size_t n = fuse(kw, 2, NULL, 0, 0.0f, out, 4);
    HU_ASSERT_EQ(n, 2u);
    HU_ASSERT_FLOAT_EQ(out[0].rerank_score, 1.0f, 1e-6);
    HU_ASSERT_FLOAT_EQ(out[1].rerank_score, 1.0f, 1e-6);
    hu_rerank_free_results(out, n);

    hu_search_result_t kw2[2] = {row("first", 9.0f), row("second", 1.0f)};
    n = fuse(kw2, 2, NULL, 0, 1.0f, out, 4);
    HU_ASSERT_EQ(n, 2u);
    HU_ASSERT_STR_EQ(out[0].content, "first");
    HU_ASSERT_FLOAT_EQ(out[0].rerank_score, 0.0f, 1e-6);
    HU_ASSERT_STR_EQ(out[1].content, "second");
    hu_rerank_free_results(out, n);
}

/* The cap applies AFTER the sort: a dense-only row that is the best fused row
 * survives max_results=1 even though three keyword rows precede it in input
 * order (hu_rerank_rrf's insertion-order cap would have dropped it). */
static void test_score_fusion_caps_after_sorting(void) {
    hu_search_result_t kw[3] = {row("a", 3.0f), row("b", 2.0f), row("c", 1.0f)};
    hu_search_result_t sem[1] = {row("dense", 0.9f)};
    hu_search_result_t out[1];
    size_t n = fuse(kw, 3, sem, 1, 1.0f, out, 1);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_STR_EQ(out[0].content, "dense");
    hu_rerank_free_results(out, n);
}

/* Same input, same output order, every time -- ties are broken totally. */
static void test_score_fusion_is_deterministic_under_ties(void) {
    hu_search_result_t kw[3] = {row("p", 1.0f), row("q", 1.0f), row("r", 1.0f)};
    hu_search_result_t sem[3] = {row("r", 0.5f), row("s", 0.5f), row("p", 0.5f)};
    hu_search_result_t a[8], b[8];
    size_t na = fuse(kw, 3, sem, 3, 0.5f, a, 8);
    size_t nb = fuse(kw, 3, sem, 3, 0.5f, b, 8);
    HU_ASSERT_EQ(na, 4u);
    HU_ASSERT_EQ(na, nb);
    for (size_t i = 0; i < na; i++)
        HU_ASSERT_STR_EQ(a[i].content, b[i].content);
    /* p and r are in both legs; p wins the RRF tie-break (1/61 + 1/63 vs
     * 1/63 + 1/61 is an exact tie, so first appearance decides: p). */
    HU_ASSERT_STR_EQ(a[0].content, "p");
    HU_ASSERT_STR_EQ(a[1].content, "r");
    HU_ASSERT_STR_EQ(a[2].content, "q");
    HU_ASSERT_STR_EQ(a[3].content, "s");
    hu_rerank_free_results(a, na);
    hu_rerank_free_results(b, nb);
}

static void test_score_fusion_rejects_bad_arguments_and_skips_null_content(void) {
    hu_search_result_t kw[2] = {row(NULL, 5.0f), row("real", 1.0f)};
    hu_search_result_t out[4];
    size_t n = 0;
    HU_ASSERT_EQ(hu_rerank_score_fusion(kw, 2, NULL, 0, out, 4, NULL, 0.5f),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_rerank_score_fusion(kw, 2, NULL, 0, out, 4, &n, 1.5f), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_rerank_score_fusion(kw, 2, NULL, 0, out, 4, &n, -0.1f),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_rerank_score_fusion(kw, 2, NULL, 0, out, 4, &n, NAN), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_rerank_score_fusion(NULL, 0, NULL, 0, out, 4, &n, 0.5f), HU_OK);
    HU_ASSERT_EQ(n, 0u);

    n = fuse(kw, 2, NULL, 0, 0.0f, out, 4);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_STR_EQ(out[0].content, "real");
    /* The NULL-content row's 5.0 is not part of the leg's range: "real" is
     * the leg's only row, so it normalises to 1.0, not 0.0. */
    HU_ASSERT_FLOAT_EQ(out[0].rerank_score, 1.0f, 1e-6);
    hu_rerank_free_results(out, n);
}

static void test_hybrid_fusion_mode_defaults_to_rrf(void) {
    unsetenv("HU_HYBRID_FUSION");
    HU_ASSERT_EQ(hu_hybrid_fusion_mode(), HU_HYBRID_FUSION_RRF);
    setenv("HU_HYBRID_FUSION", "", 1);
    HU_ASSERT_EQ(hu_hybrid_fusion_mode(), HU_HYBRID_FUSION_RRF);
    setenv("HU_HYBRID_FUSION", "rrf", 1);
    HU_ASSERT_EQ(hu_hybrid_fusion_mode(), HU_HYBRID_FUSION_RRF);
    setenv("HU_HYBRID_FUSION", "score", 1);
    HU_ASSERT_EQ(hu_hybrid_fusion_mode(), HU_HYBRID_FUSION_SCORE);
    setenv("HU_HYBRID_FUSION", "scores", 1); /* unknown -> rrf, never score */
    HU_ASSERT_EQ(hu_hybrid_fusion_mode(), HU_HYBRID_FUSION_RRF);
    setenv("HU_HYBRID_FUSION", "live", 1);
    HU_ASSERT_EQ(hu_hybrid_fusion_mode(), HU_HYBRID_FUSION_RRF);
    unsetenv("HU_HYBRID_FUSION");
}

static void test_hybrid_fusion_alpha_parses_and_rejects_invalid(void) {
    unsetenv("HU_HYBRID_FUSION_ALPHA");
    HU_ASSERT_FLOAT_EQ(hu_hybrid_fusion_alpha(), HU_HYBRID_FUSION_ALPHA_DEFAULT, 1e-9);
    setenv("HU_HYBRID_FUSION_ALPHA", "0.3", 1);
    HU_ASSERT_FLOAT_EQ(hu_hybrid_fusion_alpha(), 0.3f, 1e-6);
    setenv("HU_HYBRID_FUSION_ALPHA", "0", 1);
    HU_ASSERT_FLOAT_EQ(hu_hybrid_fusion_alpha(), 0.0f, 1e-9);
    setenv("HU_HYBRID_FUSION_ALPHA", "1", 1);
    HU_ASSERT_FLOAT_EQ(hu_hybrid_fusion_alpha(), 1.0f, 1e-9);
    const char *bad[] = {"1.5", "-0.1", "abc", "0.3x", "nan", "inf", "", " "};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        setenv("HU_HYBRID_FUSION_ALPHA", bad[i], 1);
        HU_ASSERT_FLOAT_EQ(hu_hybrid_fusion_alpha(), HU_HYBRID_FUSION_ALPHA_DEFAULT, 1e-9);
    }
    unsetenv("HU_HYBRID_FUSION_ALPHA");
}

void run_score_fusion_tests(void) {
    HU_TEST_SUITE("score_fusion");
    HU_RUN_TEST(test_score_fusion_reads_magnitudes_where_rrf_reads_ranks);
    HU_RUN_TEST(test_bm25_best_hit_normalises_to_one);
    HU_RUN_TEST(test_bm25_relevance_never_rises_along_recall_order);
    HU_RUN_TEST(test_score_fusion_single_leg_rows_score_zero_for_missing_leg);
    HU_RUN_TEST(test_score_fusion_dedupes_by_content_and_carries_key);
    HU_RUN_TEST(test_score_fusion_alpha_extremes_are_the_single_leg_orders);
    HU_RUN_TEST(test_score_fusion_flat_leg_is_one_and_empty_leg_is_zero);
    HU_RUN_TEST(test_score_fusion_caps_after_sorting);
    HU_RUN_TEST(test_score_fusion_is_deterministic_under_ties);
    HU_RUN_TEST(test_score_fusion_rejects_bad_arguments_and_skips_null_content);
    HU_RUN_TEST(test_hybrid_fusion_mode_defaults_to_rrf);
    HU_RUN_TEST(test_hybrid_fusion_alpha_parses_and_rejects_invalid);
}
