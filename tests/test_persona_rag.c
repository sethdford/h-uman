/* Tests for src/persona/rag.c — RAG-over-own-messages voice grounding. */

#include "human/persona/rag.h"
#include "test_framework.h"

#include <string.h>

static void rag_relevance_identical_is_high(void) {
    double s =
        hu_persona_rag_relevance("lunch plans tomorrow downtown", "lunch plans tomorrow downtown");
    HU_ASSERT(s > 0.9);
}

static void rag_relevance_disjoint_is_zero(void) {
    HU_ASSERT_EQ(
        (int)(hu_persona_rag_relevance("lunch plans downtown", "quantum entanglement locality") *
              1000),
        0);
}

static void rag_relevance_partial_between(void) {
    double s =
        hu_persona_rag_relevance("are we getting lunch tomorrow", "lunch sounds great tomorrow");
    HU_ASSERT(s > 0.0 && s < 1.0);
}

static void rag_relevance_null_safe(void) {
    HU_ASSERT_EQ((int)hu_persona_rag_relevance(NULL, "x"), 0);
    HU_ASSERT_EQ((int)hu_persona_rag_relevance("x", NULL), 0);
}

static void rag_retrieve_ranks_by_overlap(void) {
    const char *corpus[] = {
        "cant make the meeting got a conflict",
        "yeah lunch sounds good the usual spot",
        "gonna head out soon catch you later",
    };
    size_t idx[3];
    size_t n = hu_persona_rag_retrieve("are we still getting lunch", corpus, 3, 2, idx, 3);
    HU_ASSERT(n >= 1);
    HU_ASSERT_EQ(idx[0], (size_t)1); /* the lunch message is most relevant */
}

static void rag_retrieve_offtopic_returns_zero(void) {
    const char *corpus[] = {"totally unrelated chatter", "more unrelated stuff"};
    size_t idx[2];
    size_t n = hu_persona_rag_retrieve("quantum chromodynamics lagrangian", corpus, 2, 2, idx, 2);
    HU_ASSERT_EQ(n, (size_t)0);
}

static void rag_retrieve_respects_k_and_cap(void) {
    const char *corpus[] = {
        "lunch today sounds great",
        "lunch tomorrow works too",
        "lunch later this week maybe",
        "lunch is always good honestly",
    };
    size_t idx[2];
    size_t n = hu_persona_rag_retrieve("lunch plans", corpus, 4, 10, idx, 2);
    HU_ASSERT_EQ(n, (size_t)2); /* capped at out_cap=2 even though k=10 */
}

static void rag_build_block_contains_examples(void) {
    const char *ex[] = {"yeah for a bit", "nah not yet"};
    char buf[256];
    size_t n = hu_persona_rag_build_block(ex, 2, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "yeah for a bit") != NULL);
    HU_ASSERT(strstr(buf, "nah not yet") != NULL);
    HU_ASSERT(strstr(buf, "match this voice") != NULL);
}

static void rag_build_block_empty_is_zero(void) {
    char buf[64];
    HU_ASSERT_EQ(hu_persona_rag_build_block(NULL, 0, buf, sizeof(buf)), (size_t)0);
    const char *ex[] = {"x"};
    HU_ASSERT_EQ(hu_persona_rag_build_block(ex, 0, buf, sizeof(buf)), (size_t)0);
}

static void rag_build_block_tiny_buffer_no_half_write(void) {
    /* Buffer too small for even the header → emit nothing, NUL-terminated. */
    const char *ex[] = {"some example message here"};
    char buf[8];
    size_t n = hu_persona_rag_build_block(ex, 1, buf, sizeof(buf));
    HU_ASSERT_EQ(n, (size_t)0);
    HU_ASSERT_EQ(buf[0], '\0');
}

void run_persona_rag_tests(void) {
    HU_TEST_SUITE("persona RAG grounding");
    HU_RUN_TEST(rag_relevance_identical_is_high);
    HU_RUN_TEST(rag_relevance_disjoint_is_zero);
    HU_RUN_TEST(rag_relevance_partial_between);
    HU_RUN_TEST(rag_relevance_null_safe);
    HU_RUN_TEST(rag_retrieve_ranks_by_overlap);
    HU_RUN_TEST(rag_retrieve_offtopic_returns_zero);
    HU_RUN_TEST(rag_retrieve_respects_k_and_cap);
    HU_RUN_TEST(rag_build_block_contains_examples);
    HU_RUN_TEST(rag_build_block_empty_is_zero);
    HU_RUN_TEST(rag_build_block_tiny_buffer_no_half_write);
}
