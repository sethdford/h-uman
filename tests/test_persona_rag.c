/* Tests for src/persona/rag.c — RAG-over-own-messages voice grounding. */

#include "human/core/allocator.h"
#include "human/persona/rag.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* --- hu_persona_rag_ground_from_file: the hot-path I/O wrapper --- */

static void rag_ground_from_file_injects_relevant(void) {
    hu_allocator_t a = hu_system_allocator();
    char path[128];
    snprintf(path, sizeof(path), "/tmp/hu_rag_corpus_%d.jsonl", (int)getpid());
    FILE *f = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"text\": \"lunch tomorrow sounds great, downtown works\"}\n", f);
    fputs("{\"text\": \"quantum entanglement violates local realism\"}\n", f);
    fputs("{\"text\": \"yeah grabbing lunch downtown is good for me\"}\n", f);
    fclose(f);

    char buf[1024];
    size_t n = hu_persona_rag_ground_from_file("are we getting lunch downtown tomorrow", path, 2,
                                               buf, sizeof(buf), &a);
    remove(path);
    HU_ASSERT(n > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "lunch"));  /* relevant real message retrieved */
    HU_ASSERT(strstr(buf, "quantum") == NULL); /* off-topic message excluded */
}

static void rag_ground_from_file_missing_corpus_is_zero(void) {
    hu_allocator_t a = hu_system_allocator();
    char buf[256] = {'x', 0};
    size_t n = hu_persona_rag_ground_from_file("anything", "/tmp/hu_rag_absent_zzz.jsonl", 3, buf,
                                               sizeof(buf), &a);
    HU_ASSERT_EQ((int)n, 0);
    HU_ASSERT_EQ((int)buf[0], 0); /* buffer cleared even on miss */
}

static void rag_ground_from_file_null_safe(void) {
    hu_allocator_t a = hu_system_allocator();
    char buf[64];
    HU_ASSERT_EQ((int)hu_persona_rag_ground_from_file(NULL, "/x", 3, buf, sizeof(buf), &a), 0);
    HU_ASSERT_EQ((int)hu_persona_rag_ground_from_file("q", NULL, 3, buf, sizeof(buf), &a), 0);
    HU_ASSERT_EQ((int)hu_persona_rag_ground_from_file("q", "/x", 3, NULL, 10, &a), 0);
    HU_ASSERT_EQ((int)hu_persona_rag_ground_from_file("q", "/x", 0, buf, sizeof(buf), &a), 0);
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
    HU_RUN_TEST(rag_ground_from_file_injects_relevant);
    HU_RUN_TEST(rag_ground_from_file_missing_corpus_is_zero);
    HU_RUN_TEST(rag_ground_from_file_null_safe);
}
