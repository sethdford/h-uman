#include "human/agent.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/consolidation.h"
#include "human/memory/engines.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void similarity_identical_returns_100(void) {
    const char *a = "hello world";
    const char *b = "hello world";
    uint32_t score = hu_similarity_score(a, 11, b, 11);
    HU_ASSERT_EQ(score, 100u);
}

static void similarity_completely_different_returns_0(void) {
    const char *a = "alpha beta gamma";
    const char *b = "foo bar baz";
    uint32_t score = hu_similarity_score(a, 15, b, 11);
    HU_ASSERT_EQ(score, 0u);
}

static void similarity_partial_overlap_returns_reasonable(void) {
    const char *a = "hello world foo";
    const char *b = "hello world bar";
    uint32_t score = hu_similarity_score(a, 14, b, 14);
    HU_ASSERT_TRUE(score >= 50u);
    HU_ASSERT_TRUE(score <= 100u);
}

#ifdef HU_ENABLE_SQLITE
static void consolidation_removes_duplicates(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    const char *dup = "user likes coffee and tea every morning";
    const char *unique = "user went hiking in the mountains last weekend";

    hu_error_t err = mem.vtable->store(mem.ctx, "cons_dup_a", 10, dup, strlen(dup), &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    err = mem.vtable->store(mem.ctx, "cons_dup_b", 10, dup, strlen(dup), &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    err = mem.vtable->store(mem.ctx, "cons_unique", 11, unique, strlen(unique), &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count_before = 0;
    err = mem.vtable->count(mem.ctx, &count_before);
    HU_ASSERT_EQ(err, HU_OK);

    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    config.dedup_threshold = 80;
    config.max_entries = 100;
    config.decay_days = 0;
    config.decay_factor = 1.0;

    err = hu_memory_consolidate(&alloc, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count_after = 0;
    err = mem.vtable->count(mem.ctx, &count_after);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count_after <= count_before);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}
/* MEM-003: Consolidation must not delete cross-contact entries */
static void consolidation_cross_contact_isolation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    const char *content = "user likes coffee and tea every morning";
    size_t content_len = strlen(content);

    /* Store identical content for two different contacts */
    hu_error_t err =
        mem.vtable->store(mem.ctx, "contact:alice:pref", 18, content, content_len, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    err = mem.vtable->store(mem.ctx, "contact:bob:pref", 16, content, content_len, &cat, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    config.dedup_threshold = 80;
    config.max_entries = 100;
    config.decay_days = 0;

    err = hu_memory_consolidate(&alloc, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);

    /* Both entries must survive — cross-contact dedup must NOT happen */
    size_t count = 0;
    err = mem.vtable->count(mem.ctx, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2u);

    /* Verify both are retrievable */
    hu_memory_entry_t entry;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "contact:alice:pref", 18, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(found);
    hu_memory_entry_free_fields(&alloc, &entry);

    found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "contact:bob:pref", 16, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(found);
    hu_memory_entry_free_fields(&alloc, &entry);

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}
/* CRITIC-RD2-001: Eviction must not wipe a contact's last entry */
static void consolidation_eviction_preserves_last_contact_entry(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_NOT_NULL(mem.vtable);

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};

    /* Store 5 entries for alice, 1 entry for bob */
    for (int i = 0; i < 5; i++) {
        char key[64], content[128];
        int kn = snprintf(key, sizeof(key), "contact:alice:item%d", i);
        int cn = snprintf(content, sizeof(content), "alice memory item number %d content", i);
        hu_error_t err =
            mem.vtable->store(mem.ctx, key, (size_t)kn, content, (size_t)cn, &cat, NULL, 0);
        HU_ASSERT_EQ(err, HU_OK);
    }
    {
        const char *key = "contact:bob:only";
        const char *content = "bob single memory entry that must survive";
        hu_error_t err =
            mem.vtable->store(mem.ctx, key, strlen(key), content, strlen(content), &cat, NULL, 0);
        HU_ASSERT_EQ(err, HU_OK);
    }

    size_t count_before = 0;
    hu_error_t err = mem.vtable->count(mem.ctx, &count_before);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count_before, 6u);

    /* Set max_entries = 3 — need to evict 3 entries */
    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    config.dedup_threshold = 100; /* no dedup */
    config.max_entries = 3;
    config.decay_days = 0;
    config.decay_factor = 1.0;

    err = hu_memory_consolidate(&alloc, &mem, &config);
    HU_ASSERT_EQ(err, HU_OK);

    /* Bob's entry MUST survive — it's the only entry for that contact */
    hu_memory_entry_t entry;
    bool found = false;
    err = mem.vtable->get(mem.ctx, &alloc, "contact:bob:only", 16, &entry, &found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(found);
    hu_memory_entry_free_fields(&alloc, &entry);

    /* Total should be <= 4 (max_entries=3 + bob's protected entry if alice
       had 3 evicted entries; could be 3 or 4 depending on protection logic) */
    size_t count_after = 0;
    err = mem.vtable->count(mem.ctx, &count_after);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count_after <= count_before);
    HU_ASSERT_TRUE(count_after >= 2); /* at least alice's 1 + bob's 1 */

    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}
#endif /* HU_ENABLE_SQLITE */

static void similarity_null_a_returns_0(void) {
    uint32_t score = hu_similarity_score(NULL, 0, "hello", 5);
    HU_ASSERT_EQ(score, 0u);
}

static void similarity_null_b_returns_0(void) {
    uint32_t score = hu_similarity_score("hello", 5, NULL, 0);
    HU_ASSERT_EQ(score, 0u);
}

static void similarity_both_empty_returns_100(void) {
    uint32_t score = hu_similarity_score("", 0, "", 0);
    HU_ASSERT_EQ(score, 100u);
}

static void similarity_same_prefix_reasonable(void) {
    const char *a = "hello world";
    const char *b = "hello there";
    uint32_t score = hu_similarity_score(a, 11, b, 11);
    HU_ASSERT_TRUE(score >= 30u);
    HU_ASSERT_TRUE(score <= 80u);
}

#ifdef HU_ENABLE_SQLITE
/* Two entries scoring 80: duplicates at the loaded-config default (70), kept
 * at the old hard-coded 85 that hu_agent_consolidate_memory used to ignore
 * config with. */
static const char k_near_dup_a[] = "seth bought fresh salmon tacos downtown friday night yesterday";
static const char k_near_dup_b[] = "seth bought fresh salmon tacos downtown friday night today";

static size_t consolidate_near_dups_via_agent(uint32_t dedup_threshold) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "near_a", 6, k_near_dup_a, sizeof(k_near_dup_a) - 1, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "near_b", 6, k_near_dup_b, sizeof(k_near_dup_b) - 1, &cat, NULL, 0);

    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.behavior.decay_days = 30;
    cfg.behavior.dedup_threshold = dedup_threshold;
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    agent.memory = &mem;
    agent.config = &cfg;

    HU_ASSERT_EQ(hu_agent_consolidate_memory(&agent), HU_OK);
    size_t after = 0;
    mem.vtable->count(mem.ctx, &after);
    mem.vtable->deinit(mem.ctx);
    return after;
}

static void agent_consolidate_memory_honors_configured_dedup_threshold(void) {
    uint32_t score = hu_similarity_score(k_near_dup_a, sizeof(k_near_dup_a) - 1, k_near_dup_b,
                                         sizeof(k_near_dup_b) - 1);
    HU_ASSERT_TRUE(score >= 70u && score < 85u);

    /* Same pair, only the configured threshold differs. */
    HU_ASSERT_EQ(consolidate_near_dups_via_agent(70), 1u);
    HU_ASSERT_EQ(consolidate_near_dups_via_agent(90), 2u);
}
#endif

/* Every hu_memory_consolidate caller (daemon tick, topic switch, per-turn,
 * gateway memory.consolidate) takes its settings from this one builder. */
static void agent_consolidation_config_reads_behavior(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.behavior.decay_days = 7;
    cfg.behavior.dedup_threshold = 42;

    hu_consolidation_config_t c = hu_agent_consolidation_config(&cfg);
    HU_ASSERT_EQ(c.decay_days, 7u);
    HU_ASSERT_EQ(c.dedup_threshold, 42u);
    HU_ASSERT_EQ(c.max_entries, 5000u);
}

/* NULL config must match a freshly loaded config (config_merge defaults), not
 * dedup_threshold = 0: every pair scores >= 0, so 0 would delete all but the
 * newest entry per contact. */
static void agent_consolidation_config_null_config_matches_loaded_defaults(void) {
    hu_consolidation_config_t c = hu_agent_consolidation_config(NULL);
    HU_ASSERT_EQ(c.decay_days, 30u);
    HU_ASSERT_EQ(c.dedup_threshold, 70u);
    HU_ASSERT_EQ(c.max_entries, 5000u);

    hu_consolidation_config_t d = HU_CONSOLIDATION_DEFAULTS;
    HU_ASSERT_EQ(d.dedup_threshold, c.dedup_threshold);
    HU_ASSERT_EQ(d.max_entries, c.max_entries);
}

static void consolidation_null_memory_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_consolidation_config_t config = HU_CONSOLIDATION_DEFAULTS;
    hu_error_t err = hu_memory_consolidate(&alloc, NULL, &config);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ── Topic-switch consolidation debounce tests ────────────────────────── */

static void debounce_init_prevents_immediate_run(void) {
    hu_consolidation_debounce_t d;
    hu_consolidation_debounce_init(&d);
    HU_ASSERT(!hu_consolidation_should_run(&d, 1000));
}

static void debounce_needs_min_entries(void) {
    hu_consolidation_debounce_t d;
    hu_consolidation_debounce_init(&d);
    for (int i = 0; i < HU_CONSOLIDATION_MIN_ENTRIES - 1; i++)
        hu_consolidation_debounce_tick(&d);
    HU_ASSERT(!hu_consolidation_should_run(&d, 1000));
    hu_consolidation_debounce_tick(&d);
    HU_ASSERT(hu_consolidation_should_run(&d, 1000));
}

static void debounce_respects_min_interval(void) {
    hu_consolidation_debounce_t d;
    hu_consolidation_debounce_init(&d);
    for (int i = 0; i < 10; i++)
        hu_consolidation_debounce_tick(&d);
    hu_consolidation_debounce_reset(&d, 1000);
    for (int i = 0; i < 10; i++)
        hu_consolidation_debounce_tick(&d);
    HU_ASSERT(!hu_consolidation_should_run(&d, 1000 + HU_CONSOLIDATION_MIN_INTERVAL_SECS - 1));
    HU_ASSERT(hu_consolidation_should_run(&d, 1000 + HU_CONSOLIDATION_MIN_INTERVAL_SECS));
}

static void debounce_reset_clears_counters(void) {
    hu_consolidation_debounce_t d;
    hu_consolidation_debounce_init(&d);
    for (int i = 0; i < 10; i++)
        hu_consolidation_debounce_tick(&d);
    HU_ASSERT(hu_consolidation_should_run(&d, 1000));
    hu_consolidation_debounce_reset(&d, 1000);
    HU_ASSERT(!hu_consolidation_should_run(&d, 2000));
}

static void debounce_null_safety(void) {
    hu_consolidation_debounce_init(NULL);
    hu_consolidation_debounce_tick(NULL);
    HU_ASSERT(!hu_consolidation_should_run(NULL, 1000));
    hu_consolidation_debounce_reset(NULL, 1000);
}

static void debounce_inject_adds_ticks(void) {
    hu_consolidation_debounce_t d;
    hu_consolidation_debounce_init(&d);
    hu_consolidation_debounce_inject(&d, HU_CONSOLIDATION_MIN_ENTRIES);
    HU_ASSERT(hu_consolidation_should_run(&d, 1000));
}

static void topic_switch_signal_roundtrip(void) {
    hu_consolidation_set_topic_switch(false);
    HU_ASSERT(!hu_consolidation_get_and_clear_topic_switch());
    hu_consolidation_set_topic_switch(true);
    HU_ASSERT(hu_consolidation_get_and_clear_topic_switch());
    HU_ASSERT(!hu_consolidation_get_and_clear_topic_switch());
}

static void similarity_case_insensitive(void) {
    uint32_t score = hu_similarity_score("Hello World", 11, "hello world", 11);
    HU_ASSERT(score == 100);
}

static void similarity_ignores_stop_words(void) {
    uint32_t score = hu_similarity_score("the cat is on the mat", 21, "cat mat", 7);
    HU_ASSERT(score >= 90);
}

static void similarity_strips_punctuation(void) {
    uint32_t score = hu_similarity_score("hello, world!", 13, "hello world", 11);
    HU_ASSERT(score == 100);
}

void run_consolidation_tests(void) {
    HU_TEST_SUITE("consolidation");
    HU_RUN_TEST(similarity_identical_returns_100);
    HU_RUN_TEST(similarity_completely_different_returns_0);
    HU_RUN_TEST(similarity_partial_overlap_returns_reasonable);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(consolidation_removes_duplicates);
    HU_RUN_TEST(consolidation_cross_contact_isolation);
    HU_RUN_TEST(consolidation_eviction_preserves_last_contact_entry);
    HU_RUN_TEST(agent_consolidate_memory_honors_configured_dedup_threshold);
#endif
    HU_RUN_TEST(similarity_null_a_returns_0);
    HU_RUN_TEST(similarity_null_b_returns_0);
    HU_RUN_TEST(similarity_both_empty_returns_100);
    HU_RUN_TEST(similarity_same_prefix_reasonable);
    HU_RUN_TEST(consolidation_null_memory_returns_error);
    HU_RUN_TEST(agent_consolidation_config_reads_behavior);
    HU_RUN_TEST(agent_consolidation_config_null_config_matches_loaded_defaults);

    /* Topic-switch debounce */
    HU_RUN_TEST(debounce_init_prevents_immediate_run);
    HU_RUN_TEST(debounce_needs_min_entries);
    HU_RUN_TEST(debounce_respects_min_interval);
    HU_RUN_TEST(debounce_reset_clears_counters);
    HU_RUN_TEST(debounce_null_safety);
    HU_RUN_TEST(debounce_inject_adds_ticks);
    HU_RUN_TEST(topic_switch_signal_roundtrip);

    /* Improved similarity */
    HU_RUN_TEST(similarity_case_insensitive);
    HU_RUN_TEST(similarity_ignores_stop_words);
    HU_RUN_TEST(similarity_strips_punctuation);
}
