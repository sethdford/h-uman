/*
 * Phase 1 (RL SOTA) — KV-cache index module unit tests.
 *
 * The cache tracks (system_prompt_hash, n_past_system) per provider
 * context. Tests don't touch llama.h at all — they verify the index
 * logic (record/lookup/reset) in isolation. Task 8 wires the chat
 * path to call these functions before/after each llama_decode of the
 * system prefix.
 */

#include "human/providers/llamacpp_kvcache.h"

#include "human/core/error.h"
#include "test_framework.h"

#include <stdint.h>
#include <string.h>

static void test_kvcache_init_starts_empty(void) {
    hu_llamacpp_kvcache_t cache = {0};
    HU_ASSERT_EQ(hu_llamacpp_kvcache_init(&cache), HU_OK);
    int32_t out = -1;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "anything", 8, &out),
                 HU_ERR_NOT_FOUND);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_record_then_lookup_hits(void) {
    hu_llamacpp_kvcache_t cache = {0};
    hu_llamacpp_kvcache_init(&cache);
    const char *sys = "You are a helpful assistant.";
    HU_ASSERT_EQ(
        hu_llamacpp_kvcache_record_system(&cache, sys, strlen(sys), 42), HU_OK);
    int32_t out = -1;
    HU_ASSERT_EQ(
        hu_llamacpp_kvcache_lookup_system(&cache, sys, strlen(sys), &out), HU_OK);
    HU_ASSERT_EQ(out, 42);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_lookup_misses_on_different_prompt(void) {
    hu_llamacpp_kvcache_t cache = {0};
    hu_llamacpp_kvcache_init(&cache);
    const char *sys_a = "You are persona A.";
    const char *sys_b = "You are persona B.";
    hu_llamacpp_kvcache_record_system(&cache, sys_a, strlen(sys_a), 50);
    int32_t out = 999; /* sentinel — must not be touched on miss */
    hu_error_t err =
        hu_llamacpp_kvcache_lookup_system(&cache, sys_b, strlen(sys_b), &out);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(out, 999);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_record_overwrites_prior_slot(void) {
    hu_llamacpp_kvcache_t cache = {0};
    hu_llamacpp_kvcache_init(&cache);
    hu_llamacpp_kvcache_record_system(&cache, "old", 3, 10);
    hu_llamacpp_kvcache_record_system(&cache, "new", 3, 20);
    int32_t out = -1;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "old", 3, &out),
                 HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "new", 3, &out),
                 HU_OK);
    HU_ASSERT_EQ(out, 20);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_reset_invalidates_slot(void) {
    hu_llamacpp_kvcache_t cache = {0};
    hu_llamacpp_kvcache_init(&cache);
    hu_llamacpp_kvcache_record_system(&cache, "x", 1, 7);
    hu_llamacpp_kvcache_reset(&cache);
    int32_t out = -1;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "x", 1, &out),
                 HU_ERR_NOT_FOUND);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_rejects_null_args(void) {
    hu_llamacpp_kvcache_t cache = {0};
    hu_llamacpp_kvcache_init(&cache);
    int32_t out;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_init(NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_record_system(NULL, "x", 1, 1),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_record_system(&cache, NULL, 1, 1),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_record_system(&cache, "x", 1, 0),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(NULL, "x", 1, &out),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "x", 1, NULL),
                 HU_ERR_INVALID_ARGUMENT);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_fnv1a_is_deterministic_and_distinguishes_inputs(void) {
    /* Determinism: same input -> same hash on repeat call.
     * Discrimination: different inputs -> different hashes.
     * Sentinel: the implementation maps 0 -> 1 so the empty-slot
     *           sentinel (system_prompt_hash == 0) stays unambiguous;
     *           verify hashing the empty string never returns 0. */
    uint64_t h1 = hu_llamacpp_kvcache_fnv1a("human", 5);
    uint64_t h2 = hu_llamacpp_kvcache_fnv1a("human", 5);
    HU_ASSERT_EQ(h1, h2);
    uint64_t h3 = hu_llamacpp_kvcache_fnv1a("Human", 5); /* case differs by 1 byte */
    HU_ASSERT_TRUE(h1 != h3);
    HU_ASSERT_TRUE(hu_llamacpp_kvcache_fnv1a("", 0) != 0);
}

void run_llamacpp_kvcache_tests(void) {
    HU_RUN_TEST(test_kvcache_init_starts_empty);
    HU_RUN_TEST(test_kvcache_record_then_lookup_hits);
    HU_RUN_TEST(test_kvcache_lookup_misses_on_different_prompt);
    HU_RUN_TEST(test_kvcache_record_overwrites_prior_slot);
    HU_RUN_TEST(test_kvcache_reset_invalidates_slot);
    HU_RUN_TEST(test_kvcache_rejects_null_args);
    HU_RUN_TEST(test_kvcache_fnv1a_is_deterministic_and_distinguishes_inputs);
}
