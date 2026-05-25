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

static void test_kvcache_record_same_prompt_updates_in_place(void) {
    /* Phase 2 contract: re-recording the SAME system_prompt updates
     * the existing slot's n_past, never evicts another slot. The
     * predecessor test (test_kvcache_record_overwrites_prior_slot,
     * pre-Phase 2) asserted that a second record() *with a different
     * prompt* evicted the first — that pinned the Phase 1 single-slot
     * design. With Phase 2's LRU, both prompts coexist; see
     * test_kvcache_holds_distinct_prompts_concurrently below. */
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    hu_llamacpp_kvcache_record_system(&cache, "persona", 7, 10);
    hu_llamacpp_kvcache_record_system(&cache, "persona", 7, 25); /* same hash */
    int32_t out = -1;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "persona", 7, &out), HU_OK);
    HU_ASSERT_EQ(out, 25); /* latest n_past wins */
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

/* Phase 2 — multi-slot LRU contracts. The cache holds up to
 * HU_LLAMACPP_KVCACHE_SLOTS distinct prompts simultaneously; only when
 * a NEW prompt arrives at capacity does eviction fire, and the LRU is
 * the victim (so frequently-used personas survive churn). */

static void test_kvcache_holds_distinct_prompts_concurrently(void) {
    /* Up to N=HU_LLAMACPP_KVCACHE_SLOTS distinct prompts coexist. This
     * is the core Phase 2 win: a conversation that switches between
     * personas (telegram, discord, imessage, slack) no longer thrashes
     * the single Phase 1 slot. */
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    const char *prompts[HU_LLAMACPP_KVCACHE_SLOTS] = {"a", "b", "c", "d"};
    for (size_t i = 0; i < HU_LLAMACPP_KVCACHE_SLOTS; i++)
        hu_llamacpp_kvcache_record_system(&cache, prompts[i], 1, (int32_t)(10 + i));
    int32_t out = -1;
    for (size_t i = 0; i < HU_LLAMACPP_KVCACHE_SLOTS; i++) {
        out = -1;
        HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, prompts[i], 1, &out), HU_OK);
        HU_ASSERT_EQ(out, (int32_t)(10 + i));
    }
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_at_capacity_new_record_evicts_lru(void) {
    /* Fill all slots, then record one more — the FIRST one inserted
     * (oldest tick, never re-touched) must be the victim, not any of
     * the later three. */
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    const char *prompts[HU_LLAMACPP_KVCACHE_SLOTS] = {"oldest", "b", "c", "d"};
    for (size_t i = 0; i < HU_LLAMACPP_KVCACHE_SLOTS; i++)
        hu_llamacpp_kvcache_record_system(&cache, prompts[i], (size_t)strlen(prompts[i]),
                                          (int32_t)(10 + i));
    /* New entry forces eviction of "oldest" (LRU). */
    hu_llamacpp_kvcache_record_system(&cache, "fresh", 5, 99);

    int32_t out = -1;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "oldest", 6, &out), HU_ERR_NOT_FOUND);
    /* The other three from the original fill must still be present. */
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "b", 1, &out), HU_OK);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "c", 1, &out), HU_OK);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "d", 1, &out), HU_OK);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "fresh", 5, &out), HU_OK);
    HU_ASSERT_EQ(out, 99);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_lookup_hit_refreshes_lru(void) {
    /* A lookup-hit must bump the slot's tick so it's NOT the LRU on
     * the next eviction. Pinned because a sloppy implementation that
     * only updates the tick in record_system would let frequently-
     * READ entries get evicted in favor of less-touched ones. */
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    /* All single-char so record-length matches lookup-length. The
     * hash is over (data, len) — a length mismatch silently produces
     * different hashes (caught the hard way in this test's first draft). */
    const char *prompts[HU_LLAMACPP_KVCACHE_SLOTS] = {"a", "b", "c", "d"};
    for (size_t i = 0; i < HU_LLAMACPP_KVCACHE_SLOTS; i++)
        hu_llamacpp_kvcache_record_system(&cache, prompts[i], 1, (int32_t)(10 + i));

    /* Touch "a" via lookup — its tick should now be highest, so any
     * eviction picks "b" (the next-oldest) instead. */
    int32_t out = -1;
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "a", 1, &out), HU_OK);

    /* Force eviction. */
    hu_llamacpp_kvcache_record_system(&cache, "fresh", 5, 99);

    /* "a" survived (recently looked up); "b" got evicted (LRU). */
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "a", 1, &out), HU_OK);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "b", 1, &out), HU_ERR_NOT_FOUND);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_reset_clears_all_slots_but_keeps_counters(void) {
    /* reset() must clear every slot (LoRA hot-swap invalidates all
     * KV equally), but preserve the lifetime hit/miss counters per the
     * Phase 0.3 contract. */
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    hu_llamacpp_kvcache_record_system(&cache, "a", 1, 1);
    hu_llamacpp_kvcache_record_system(&cache, "b", 1, 2);
    int32_t out = -1;
    hu_llamacpp_kvcache_lookup_system(&cache, "a", 1, &out); /* hit */
    hu_llamacpp_kvcache_lookup_system(&cache, "z", 1, &out); /* miss */
    HU_ASSERT_EQ(hu_llamacpp_kvcache_hits(&cache), 1u);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_misses(&cache), 1u);

    hu_llamacpp_kvcache_reset(&cache);

    /* All slots gone — both lookups miss. */
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "a", 1, &out), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_lookup_system(&cache, "b", 1, &out), HU_ERR_NOT_FOUND);
    /* Counters survive (Phase 0.3 contract) — and the two new misses
     * we just measured got added on top. */
    HU_ASSERT_EQ(hu_llamacpp_kvcache_hits(&cache), 1u);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_misses(&cache), 3u);
    hu_llamacpp_kvcache_free(&cache);
}

/* Phase 2b — would-skip counter contracts. Operators read the
 * accumulated count via the getter to size the deferred TTFT
 * opportunity Phase 2b.2 will unlock. The chat-path call site is at
 * src/providers/llamacpp.c — it calls record_hit_savings on every hit
 * before the (currently safe but wasteful) re-decode. */

static void test_kvcache_tokens_would_skip_starts_at_zero(void) {
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_tokens_would_skip(&cache), 0u);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_record_hit_savings_accumulates(void) {
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    hu_llamacpp_kvcache_record_hit_savings(&cache, 100);
    hu_llamacpp_kvcache_record_hit_savings(&cache, 50);
    hu_llamacpp_kvcache_record_hit_savings(&cache, 25);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_tokens_would_skip(&cache), 175u);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_record_hit_savings_ignores_non_positive(void) {
    /* The chat path may pass cached_n_past directly without filtering.
     * Negative / zero must be a silent no-op, not a counter corruption
     * (signed -> unsigned promotion of -1 would explode the counter). */
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    hu_llamacpp_kvcache_record_hit_savings(&cache, 0);
    hu_llamacpp_kvcache_record_hit_savings(&cache, -5);
    hu_llamacpp_kvcache_record_hit_savings(&cache, -1);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_tokens_would_skip(&cache), 0u);
    hu_llamacpp_kvcache_free(&cache);
}

static void test_kvcache_record_hit_savings_null_cache_safe(void) {
    /* NULL must not crash — call site simplification depends on it. */
    hu_llamacpp_kvcache_record_hit_savings(NULL, 100);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_tokens_would_skip(NULL), 0u);
}

static void test_kvcache_tokens_would_skip_survives_reset(void) {
    /* Lifetime telemetry — reset (LoRA hot-swap) must NOT zero the
     * would-skip count. Operators want the lifetime opportunity
     * total, not a per-adapter window. Same contract as hits/misses. */
    hu_llamacpp_kvcache_t cache;
    hu_llamacpp_kvcache_init(&cache);
    hu_llamacpp_kvcache_record_hit_savings(&cache, 200);
    hu_llamacpp_kvcache_reset(&cache);
    HU_ASSERT_EQ(hu_llamacpp_kvcache_tokens_would_skip(&cache), 200u);
    hu_llamacpp_kvcache_free(&cache);
}

void run_llamacpp_kvcache_tests(void) {
    HU_RUN_TEST(test_kvcache_init_starts_empty);
    HU_RUN_TEST(test_kvcache_record_then_lookup_hits);
    HU_RUN_TEST(test_kvcache_lookup_misses_on_different_prompt);
    HU_RUN_TEST(test_kvcache_record_same_prompt_updates_in_place);
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
    HU_RUN_TEST(test_kvcache_holds_distinct_prompts_concurrently);
    HU_RUN_TEST(test_kvcache_at_capacity_new_record_evicts_lru);
    HU_RUN_TEST(test_kvcache_lookup_hit_refreshes_lru);
    HU_RUN_TEST(test_kvcache_reset_clears_all_slots_but_keeps_counters);
    HU_RUN_TEST(test_kvcache_tokens_would_skip_starts_at_zero);
    HU_RUN_TEST(test_kvcache_record_hit_savings_accumulates);
    HU_RUN_TEST(test_kvcache_record_hit_savings_ignores_non_positive);
    HU_RUN_TEST(test_kvcache_record_hit_savings_null_cache_safe);
    HU_RUN_TEST(test_kvcache_tokens_would_skip_survives_reset);
}
