/* tests/test_reranker.c — direct coverage for hu_mmr_rerank
 * (src/memory/retrieval/reranker.c), exported from
 * include/human/memory/retrieval.h and called from
 * src/memory/retrieval/engine.c when hu_retrieval_options_t.use_reranking
 * is set.
 *
 * Prior to this file, hu_mmr_rerank had only vacuous coverage in
 * tests/test_retrieval.c: test_mmr_rerank_empty passes count=0, which hits
 * the early `if (!entries || !scores || count == 0) return HU_OK;` guard
 * and never runs the MMR loop at all, and test_mmr_diversifies_results
 * asserts only `res.count >= 1` with a comment ("MMR should produce a
 * different ordering than pure score") but no assertion on the actual
 * order. Both pass whether or not hu_mmr_rerank does anything. These two
 * tests assert the real MMR contract: the output ORDER, computed from the
 * Jaccard word-overlap the implementation actually uses.
 */
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include "test_framework.h"
#include <string.h>

static hu_memory_entry_t make_entry(const char *key, const char *content) {
    hu_memory_entry_t e = {0};
    e.key = key;
    e.key_len = strlen(key);
    e.content = content;
    e.content_len = strlen(content);
    e.category.tag = HU_MEMORY_CATEGORY_CORE;
    e.score = 0.0;
    return e;
}

/* (a) lambda = 1.0 collapses MMR to pure query relevance: mmr = 1*sim_qd -
 * 0*max_sim_dj. Feed candidates in deliberately WRONG (ascending-relevance)
 * order and assert hu_mmr_rerank sorts them into descending Jaccard
 * similarity to the query. A no-op reranker leaves the input order
 * unchanged, which does not match this assertion. */
static void test_reranker_lambda_one_orders_by_relevance(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *query = "apple banana cherry";

    /* Jaccard(query, .) : "none"=0.0, "partial"=2/3, "full"=1.0 */
    hu_memory_entry_t entries[3] = {
        make_entry("none", "grape mango"),
        make_entry("partial", "apple banana"),
        make_entry("full", "apple banana cherry"),
    };
    double scores[3] = {0.1, 0.2, 0.3};

    hu_error_t err = hu_mmr_rerank(&alloc, query, strlen(query), entries, scores, 3, 1.0);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_STR_EQ(entries[0].key, "full");
    HU_ASSERT_STR_EQ(entries[1].key, "partial");
    HU_ASSERT_STR_EQ(entries[2].key, "none");
}

/* (b) low lambda favors diversity after the first pick: mmr = lam*sim_qd -
 * (1-lam)*max_sim_dj. "top" is the closest match to the query and wins the
 * first round regardless (nothing selected yet, so the penalty term is 0).
 * On the second round "near_dup" (near-identical to "top") pays a heavy
 * similarity penalty against the already-selected "top", while "dissimilar"
 * (weaker query match but NOT similar to "top") does not — so a low lambda
 * must push the near-duplicate below the dissimilar candidate. A no-op
 * reranker (input order top, near_dup, dissimilar unchanged) fails this. */
static void test_reranker_low_lambda_demotes_near_duplicate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *query = "apple banana cherry";

    hu_memory_entry_t entries[3] = {
        make_entry("top", "apple banana cherry"),
        make_entry("near_dup", "apple banana cherry date"),
        make_entry("dissimilar", "banana"),
    };
    double scores[3] = {1.0, 1.0, 1.0};

    hu_error_t err = hu_mmr_rerank(&alloc, query, strlen(query), entries, scores, 3, 0.1);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_STR_EQ(entries[0].key, "top");
    HU_ASSERT_STR_EQ(entries[1].key, "dissimilar");
    HU_ASSERT_STR_EQ(entries[2].key, "near_dup");
}

void run_reranker_tests(void) {
    HU_TEST_SUITE("reranker");
    HU_RUN_TEST(test_reranker_lambda_one_orders_by_relevance);
    HU_RUN_TEST(test_reranker_low_lambda_demotes_near_duplicate);
}
