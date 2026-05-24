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
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "anything", 8, &out), HU_ERR_NOT_FOUND);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_record_then_lookup_hits(void) {
    hu_llamacpp_kvcache_t cache = {0};
    hu_llamacpp_kvcache_init(&cache);
    const char *sys = "You are a helpful assistant.";
    HU_ASSERT_EQ(hu_llamacpp_kvcache_record_system(&cache, sys, strlen(sys), 42), HU_OK);
    int32_t out = -1;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, sys, strlen(sys), &out), HU_OK);
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
    hu_error_t err = hu_llamacpp_kvcache_lookup_system(&cache, sys_b, strlen(sys_b), &out);
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
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "old", 3, &out), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "new", 3, &out), HU_OK);
    HU_ASSERT_EQ(out, 20);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_reset_invalidates_slot(void) {
    hu_llamacpp_kvcache_t cache = {0};
    hu_llamacpp_kvcache_init(&cache);
    hu_llamacpp_kvcache_record_system(&cache, "x", 1, 7);
    hu_llamacpp_kvcache_reset(&cache);
    int32_t out = -1;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "x", 1, &out), HU_ERR_NOT_FOUND);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_rejects_null_args(void) {
    hu_llamacpp_kvcache_t cache = {0};
    hu_llamacpp_kvcache_init(&cache);
    int32_t out;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_init(NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_record_system(NULL, "x", 1, 1), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_record_system(&cache, NULL, 1, 1), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_record_system(&cache, "x", 1, 0), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(NULL, "x", 1, &out), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "x", 1, NULL), HU_ERR_INVALID_ARGUMENT);
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

/* Phase 0.3 — counter contracts. Hit/miss telemetry must (a) start at
 * zero, (b) only count honest lookups (NULL args don't count as
 * misses), (c) survive reset() (lifetime telemetry, not per-slot), and
 * (d) read 0 on NULL cache (so an aggregator can iterate without
 * null-checking). */

static void test_kvcache_counters_start_at_zero(void) {
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_hits(&cache), 0u);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_misses(&cache), 0u);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_hit_increments_hits_only(void) {
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    const char *sys = "You are helpful.";
    hu_llamacpp_kvcache_record_system(&cache, sys, strlen(sys), 42);
    int32_t out = -1;
    hu_llamacpp_kvcache_lookup_system(&cache, sys, strlen(sys), &out);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_hits(&cache), 1u);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_misses(&cache), 0u);
    /* Second hit on the same prompt — counter accumulates. */
    hu_llamacpp_kvcache_lookup_system(&cache, sys, strlen(sys), &out);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_hits(&cache), 2u);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_miss_against_empty_slot_increments_misses(void) {
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    int32_t out = -1;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "x", 1, &out), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_hits(&cache), 0u);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_misses(&cache), 1u);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_miss_against_different_prompt_increments_misses(void) {
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    hu_llamacpp_kvcache_record_system(&cache, "persona_a", 9, 50);
    int32_t out = -1;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "persona_b", 9, &out), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_hits(&cache), 0u);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_misses(&cache), 1u);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_null_args_do_not_count_as_misses(void) {
    /* Programmer error (NULL args) must NOT pollute hit-rate telemetry.
     * Pinned because a sloppy implementation would treat "anything that
     * returned non-OK" as a miss. */
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    int32_t out;
    hu_llamacpp_kvcache_lookup_system(NULL, "x", 1, &out);
    hu_llamacpp_kvcache_lookup_system(&cache, NULL, 1, &out);
    hu_llamacpp_kvcache_lookup_system(&cache, "x", 1, NULL);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_hits(&cache), 0u);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_misses(&cache), 0u);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_reset_preserves_counters(void) {
    /* reset() fires on LoRA hot-swap. Counters are lifetime telemetry;
     * operators want to see the cache had been useful before the swap. */
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    const char *sys = "persona";
    hu_llamacpp_kvcache_record_system(&cache, sys, strlen(sys), 10);
    int32_t out = -1;
    hu_llamacpp_kvcache_lookup_system(&cache, sys, strlen(sys), &out); /* hit */
    hu_llamacpp_kvcache_lookup_system(&cache, "miss-me", 7, &out);     /* miss */
    HU_ASSERT_EQ(hu_llamacpp_kvcache_hits(&cache), 1u);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_misses(&cache), 1u);
    hu_llamacpp_kvcache_reset(&cache);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_hits(&cache), 1u);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_misses(&cache), 1u);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_null_cache_getters_return_zero(void) {
    HU_ASSERT_EQ(hu_llamacpp_kvcache_hits(NULL), 0u);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_misses(NULL), 0u);
}

void run_llamacpp_kvcache_tests(void) {
    HU_RUN_TEST(test_kvcache_init_starts_empty);
    HU_RUN_TEST(test_kvcache_record_then_lookup_hits);
    HU_RUN_TEST(test_kvcache_lookup_misses_on_different_prompt);
    HU_RUN_TEST(test_kvcache_record_overwrites_prior_slot);
    HU_RUN_TEST(test_kvcache_reset_invalidates_slot);
    HU_RUN_TEST(test_kvcache_rejects_null_args);
    HU_RUN_TEST(test_kvcache_fnv1a_is_deterministic_and_distinguishes_inputs);
    HU_RUN_TEST(test_kvcache_counters_start_at_zero);
    HU_RUN_TEST(test_kvcache_hit_increments_hits_only);
    HU_RUN_TEST(test_kvcache_miss_against_empty_slot_increments_misses);
    HU_RUN_TEST(test_kvcache_miss_against_different_prompt_increments_misses);
    HU_RUN_TEST(test_kvcache_null_args_do_not_count_as_misses);
    HU_RUN_TEST(test_kvcache_reset_preserves_counters);
    HU_RUN_TEST(test_kvcache_null_cache_getters_return_zero);
}
