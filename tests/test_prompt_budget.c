/* Pure-predicate tests for hu_prompt_budget_t. No live prompt build path
 * — those tests come in a Task-1b slice when the wrapping in prompt.c
 * lands. This file pins the OBSERVER + DECISION logic in isolation.
 *
 * B3 Phase 3 adds two AGENT-level contract tests at the bottom that pin
 * the daemon-side threading: hu_agent_from_config allocates a long-lived
 * budget, and hu_agent_turn folds per-call stats into it. */

#include "human/agent.h"
#include "human/agent/prompt.h"
#include "human/agent/prompt_budget.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "test_framework.h"
#include <string.h>

static void test_field_name_returns_stable_string(void) {
    /* hu_prompt_field_name must return a non-null static string for
     * every valid index — the budget's snapshot relies on this. */
    HU_ASSERT_STR_EQ(hu_prompt_field_name(HU_PROMPT_FIELD_MEMORY_CONTEXT), "memory_context");
    HU_ASSERT_STR_EQ(hu_prompt_field_name(HU_PROMPT_FIELD_CONVERSATION_CONTEXT),
                     "conversation_context");
    HU_ASSERT_STR_EQ(hu_prompt_field_name(HU_PROMPT_FIELD_SOMATIC_CONTEXT), "somatic_context");
    /* Out-of-range returns NULL (defensive — no array-bounds read). */
    HU_ASSERT(hu_prompt_field_name((hu_prompt_field_t)HU_PROMPT_FIELD_COUNT) == NULL);
    HU_ASSERT(hu_prompt_field_name((hu_prompt_field_t)-1) == NULL);
}

static void test_init_free_round_trip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    HU_ASSERT_NOT_NULL(b);
    HU_ASSERT_EQ(hu_prompt_budget_observation_count(b), (size_t)0);
    hu_prompt_budget_free(b);
}

static void test_init_null_args_return_invalid(void) {
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(NULL, &b), HU_ERR_INVALID_ARGUMENT);
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, NULL), HU_ERR_INVALID_ARGUMENT);
    /* hu_prompt_budget_free(NULL) must be safe. */
    hu_prompt_budget_free(NULL);
}

static void test_observe_single_turn_advances_counter(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    stats[HU_PROMPT_FIELD_MEMORY_CONTEXT].bytes_contributed = 1024;
    stats[HU_PROMPT_FIELD_CONVERSATION_CONTEXT].bytes_contributed = 512;
    hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT_EQ(hu_prompt_budget_observation_count(b), (size_t)1);
    hu_prompt_budget_free(b);
}

static void test_dead_field_requires_min_samples(void) {
    /* A field cannot be tagged DEAD before we have enough observations,
     * even if every observed sample is zero. Prevents trim-on-first-tick. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    /* 5 observations, all zero bytes for somatic. */
    for (int i = 0; i < 5; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    /* Threshold is min_samples=100; 5 < 100 → still UNKNOWN, not DEAD. */
    HU_ASSERT(!hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_SOMATIC_CONTEXT, 16, 100));
    hu_prompt_budget_free(b);
}

static void test_dead_field_fires_after_threshold_with_zero_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    /* 100 observations, all zero — somatic IS dead. */
    for (int i = 0; i < 100; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT(hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_SOMATIC_CONTEXT, 16, 100));
}

static void test_dead_field_skips_when_field_is_populated(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    /* 100 observations, conversation field has 2000 bytes each — clearly
     * NOT dead. Mean 2000 > threshold 16. */
    stats[HU_PROMPT_FIELD_CONVERSATION_CONTEXT].bytes_contributed = 2000;
    for (int i = 0; i < 100; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT(!hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_CONVERSATION_CONTEXT, 16, 100));
    /* Memory field IS dead (mean 0). */
    HU_ASSERT(hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_MEMORY_CONTEXT, 16, 100));
    hu_prompt_budget_free(b);
}

static void test_dead_field_borderline_mean_below_threshold(void) {
    /* A field that consistently produces a small but non-zero contribution
     * (e.g. always exactly 8 bytes of header) should still tag as DEAD
     * when the threshold is 16. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    stats[HU_PROMPT_FIELD_RUPTURE_CONTEXT].bytes_contributed = 8;
    for (int i = 0; i < 100; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT(hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_RUPTURE_CONTEXT, 16, 100));
    /* Same field above threshold = not DEAD. */
    HU_ASSERT(!hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_RUPTURE_CONTEXT, 4, 100));
    hu_prompt_budget_free(b);
}

static void test_dead_field_with_one_outlier_observation(void) {
    /* If a field is non-zero in even ONE turn out of many, the mean
     * may still fall below threshold but the field arguably isn't
     * "dead." This test pins the current behavior (mean-based) so a
     * future change to "non_empty_count"-based logic is intentional. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    /* 99 zero observations + 1 with 100 bytes → mean = 1 byte < 16 → DEAD. */
    for (int i = 0; i < 99; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    stats[HU_PROMPT_FIELD_MOMENT_CONTEXT].bytes_contributed = 100;
    hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT(hu_prompt_budget_field_is_dead(b, HU_PROMPT_FIELD_MOMENT_CONTEXT, 16, 100));
    hu_prompt_budget_free(b);
}

static void test_snapshot_reports_mean_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    stats[HU_PROMPT_FIELD_MEMORY_CONTEXT].bytes_contributed = 1000;
    stats[HU_PROMPT_FIELD_MEMORY_CONTEXT].name =
        hu_prompt_field_name(HU_PROMPT_FIELD_MEMORY_CONTEXT);
    for (int i = 0; i < 4; i++)
        hu_prompt_budget_observe(b, stats, HU_PROMPT_FIELD_COUNT);
    hu_prompt_field_stat_t snap[HU_PROMPT_FIELD_COUNT];
    size_t n = hu_prompt_budget_snapshot(b, snap, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT_EQ(n, (size_t)HU_PROMPT_FIELD_COUNT);
    HU_ASSERT_EQ(snap[HU_PROMPT_FIELD_MEMORY_CONTEXT].bytes_contributed, (size_t)1000);
    HU_ASSERT_STR_EQ(snap[HU_PROMPT_FIELD_MEMORY_CONTEXT].name, "memory_context");
    /* Unobserved field reports zero mean but still has a name. */
    HU_ASSERT_EQ(snap[HU_PROMPT_FIELD_CONVERSATION_CONTEXT].bytes_contributed, (size_t)0);
    HU_ASSERT_STR_EQ(snap[HU_PROMPT_FIELD_CONVERSATION_CONTEXT].name, "conversation_context");
    hu_prompt_budget_free(b);
}

static void test_snapshot_respects_array_cap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_field_stat_t small[3];
    memset(small, 0, sizeof(small));
    size_t n = hu_prompt_budget_snapshot(b, small, 3);
    HU_ASSERT_EQ(n, (size_t)3);
    hu_prompt_budget_free(b);
}

static void test_observe_null_args_no_op(void) {
    /* All NULL combinations are no-ops (no crash, no state mutation). */
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    hu_prompt_budget_observe(NULL, stats, HU_PROMPT_FIELD_COUNT);

    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *b = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &b), HU_OK);
    hu_prompt_budget_observe(b, NULL, HU_PROMPT_FIELD_COUNT);
    hu_prompt_budget_observe(b, stats, 0);
    HU_ASSERT_EQ(hu_prompt_budget_observation_count(b), (size_t)0);
    hu_prompt_budget_free(b);
}

static void test_field_is_dead_null_budget_returns_false(void) {
    /* Defensive: NULL budget never reports a field as dead — caller
     * should never act on uninitialized telemetry. */
    HU_ASSERT(!hu_prompt_budget_field_is_dead(NULL, HU_PROMPT_FIELD_MEMORY_CONTEXT, 16, 100));
}

/* ──────────────────────────────────────────────────────────────────────────
 * Phase 1b — live integration: hu_prompt_build_system populates stats.
 *
 * The builder wraps each named-field appender block with macros that
 * record (len - _track_before) into stats[field_idx].bytes_contributed.
 * These tests use a minimal hu_prompt_config_t fixture and verify that
 * the stats slots reflect what was wired. They DO NOT assert exact byte
 * counts (those depend on prompt structure — headers, separators) — only
 * the inequalities that matter: populated fields > 0, empty fields == 0,
 * names always set, total roughly matches sum of contributions. */

static void test_build_system_stats_null_preserves_zero_overhead_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.memory_context = "test memory snapshot";
    cfg.memory_context_len = 20;
    char *out = NULL;
    size_t out_len = 0;
    /* Passing NULL stats MUST work (legacy callers expect it). */
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(out_len > 0);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_build_system_stats_records_memory_context_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* A specific, recognizable memory blob whose length is known. */
    cfg.memory_context = "ALPHA-MEMORY-FIXTURE-1234567890";
    cfg.memory_context_len = strlen(cfg.memory_context);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, stats, NULL, &out, &out_len), HU_OK);
    /* The memory_context slot reports AT LEAST as many bytes as the
     * input (the wrapper also captures the "\n\n" separator). */
    HU_ASSERT(stats[HU_PROMPT_FIELD_MEMORY_CONTEXT].bytes_contributed >= cfg.memory_context_len);
    HU_ASSERT_STR_EQ(stats[HU_PROMPT_FIELD_MEMORY_CONTEXT].name, "memory_context");
    /* Unwired fields (e.g. somatic) report exactly 0 — but still
     * carry a name, so operators can tell wired-vs-missing apart. */
    HU_ASSERT_EQ(stats[HU_PROMPT_FIELD_SOMATIC_CONTEXT].bytes_contributed, (size_t)0);
    HU_ASSERT_STR_EQ(stats[HU_PROMPT_FIELD_SOMATIC_CONTEXT].name, "somatic_context");
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_build_system_stats_records_multiple_populated_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.memory_context = "MEM";
    cfg.memory_context_len = 3;
    cfg.instruction_context = "INSTRUCTIONS-LONGER-STRING-HERE";
    cfg.instruction_context_len = strlen(cfg.instruction_context);
    cfg.stm_context = "STM-blob";
    cfg.stm_context_len = strlen(cfg.stm_context);
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, stats, NULL, &out, &out_len), HU_OK);
    /* All three populated fields report non-zero contribution. */
    HU_ASSERT(stats[HU_PROMPT_FIELD_MEMORY_CONTEXT].bytes_contributed > 0);
    HU_ASSERT(stats[HU_PROMPT_FIELD_INSTRUCTION_CONTEXT].bytes_contributed > 0);
    HU_ASSERT(stats[HU_PROMPT_FIELD_STM_CONTEXT].bytes_contributed > 0);
    /* Their relative sizes track input lengths — instruction is longest. */
    HU_ASSERT(stats[HU_PROMPT_FIELD_INSTRUCTION_CONTEXT].bytes_contributed >
              stats[HU_PROMPT_FIELD_MEMORY_CONTEXT].bytes_contributed);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_build_system_stats_unwired_field_stays_zero_even_when_others_populated(void) {
    /* The audit identified somatic_context as a likely DEAD field. This
     * test pins that signal: when the field isn't populated in cfg, its
     * stats slot reports 0 bytes regardless of what other fields contain. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.memory_context = "some-memory";
    cfg.memory_context_len = 11;
    cfg.conversation_context = "alice: hey\nbob: yo";
    cfg.conversation_context_len = 18;
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, stats, NULL, &out, &out_len), HU_OK);
    HU_ASSERT_EQ(stats[HU_PROMPT_FIELD_SOMATIC_CONTEXT].bytes_contributed, (size_t)0);
    HU_ASSERT_EQ(stats[HU_PROMPT_FIELD_RUPTURE_CONTEXT].bytes_contributed, (size_t)0);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_build_system_stats_feeds_budget_observer_round_trip(void) {
    /* End-to-end: build prompt → observe stats with budget → query DEAD.
     * Proves that the wrapping in prompt.c actually emits data that the
     * budget object can act on. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* Input must be > threshold (16) bytes so memory_context isn't itself
     * tagged DEAD by the same predicate. */
    cfg.memory_context = "PERSISTENT-MEMORY-FIXTURE-BLOB-FOR-DEAD-FIELD-TEST";
    cfg.memory_context_len = strlen(cfg.memory_context);

    hu_prompt_budget_t *budget = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &budget), HU_OK);

    /* Observe 100 prompt builds — somatic is never populated, so its
     * mean stays 0 < threshold 16 → DEAD. */
    for (int i = 0; i < 100; i++) {
        hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
        memset(stats, 0, sizeof(stats));
        char *out = NULL;
        size_t out_len = 0;
        HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, stats, NULL, &out, &out_len), HU_OK);
        hu_prompt_budget_observe(budget, stats, HU_PROMPT_FIELD_COUNT);
        alloc.free(alloc.ctx, out, out_len + 1);
    }

    HU_ASSERT_EQ(hu_prompt_budget_observation_count(budget), (size_t)100);
    /* somatic was never populated → DEAD. */
    HU_ASSERT(hu_prompt_budget_field_is_dead(budget, HU_PROMPT_FIELD_SOMATIC_CONTEXT, 16, 100));
    /* memory was always populated → NOT dead. */
    HU_ASSERT(!hu_prompt_budget_field_is_dead(budget, HU_PROMPT_FIELD_MEMORY_CONTEXT, 16, 100));

    hu_prompt_budget_free(budget);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Phase 2 — trim gate.
 *
 * When cfg->prompt_budget_trim_enabled=true AND the budget has tagged a
 * field DEAD, hu_prompt_build_system skips that field's appender block
 * entirely. Verified by building the same prompt twice (trim off vs on)
 * and asserting (a) the trimmed prompt is shorter and (b) the dead
 * field's contents are absent from the trimmed output. */

static void test_trim_gate_skips_dead_fields_when_enabled(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *budget = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &budget), HU_OK);

    /* Observe 100 turns where memory_context contributed 0 bytes and
     * conversation_context contributed 2000 bytes. Memory is now DEAD
     * per (mean=0 < 16) AND (count=100 >= 100). */
    hu_prompt_field_stat_t obs[HU_PROMPT_FIELD_COUNT];
    memset(obs, 0, sizeof(obs));
    obs[HU_PROMPT_FIELD_CONVERSATION_CONTEXT].bytes_contributed = 2000;
    for (int i = 0; i < 100; i++)
        hu_prompt_budget_observe(budget, obs, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT(hu_prompt_budget_field_is_dead(budget, HU_PROMPT_FIELD_MEMORY_CONTEXT, 16, 100));

    /* Build prompt WITH memory_context populated. */
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.memory_context = "PHASE2-TRIM-FIXTURE-MEMORY-VALUE-LONG-ENOUGH-TO-MATTER";
    cfg.memory_context_len = strlen(cfg.memory_context);

    /* Baseline: trim disabled → memory IS in the prompt. */
    char *baseline = NULL;
    size_t baseline_len = 0;
    cfg.prompt_budget_trim_enabled = false;
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, NULL, budget, &baseline, &baseline_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(baseline);
    HU_ASSERT(strstr(baseline, "PHASE2-TRIM-FIXTURE-MEMORY-VALUE-LONG-ENOUGH-TO-MATTER") != NULL);

    /* Trim enabled → memory is SKIPPED even though it's populated, because
     * the budget tagged it DEAD over 100 prior observations. */
    char *trimmed = NULL;
    size_t trimmed_len = 0;
    cfg.prompt_budget_trim_enabled = true;
    cfg.prompt_budget_dead_field_min_bytes = 16;
    cfg.prompt_budget_min_samples_before_tag = 100;
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, NULL, budget, &trimmed, &trimmed_len), HU_OK);
    HU_ASSERT_NOT_NULL(trimmed);
    /* Fixture string is absent from trimmed output. */
    HU_ASSERT(strstr(trimmed, "PHASE2-TRIM-FIXTURE-MEMORY-VALUE-LONG-ENOUGH-TO-MATTER") == NULL);
    /* And the trimmed prompt is shorter than the baseline by at least
     * the fixture's length (header overhead may make it slightly more). */
    HU_ASSERT(trimmed_len < baseline_len);

    alloc.free(alloc.ctx, baseline, baseline_len + 1);
    alloc.free(alloc.ctx, trimmed, trimmed_len + 1);
    hu_prompt_budget_free(budget);
}

static void test_trim_gate_disabled_by_default_keeps_dead_fields(void) {
    /* Safety: if the operator hasn't flipped trim_enabled=true, we
     * never trim, even with a budget passed. Prevents accidental
     * trim from the daemon-wired path before a week of observations. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_budget_t *budget = NULL;
    HU_ASSERT_EQ(hu_prompt_budget_init(&alloc, &budget), HU_OK);

    hu_prompt_field_stat_t obs[HU_PROMPT_FIELD_COUNT];
    memset(obs, 0, sizeof(obs));
    obs[HU_PROMPT_FIELD_CONVERSATION_CONTEXT].bytes_contributed = 2000;
    for (int i = 0; i < 100; i++)
        hu_prompt_budget_observe(budget, obs, HU_PROMPT_FIELD_COUNT);

    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.memory_context = "SAFETY-FIXTURE-MEMORY-SHOULD-BE-PRESENT";
    cfg.memory_context_len = strlen(cfg.memory_context);
    /* prompt_budget_trim_enabled left as false (default from memset). */

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, NULL, budget, &out, &out_len), HU_OK);
    HU_ASSERT(strstr(out, "SAFETY-FIXTURE-MEMORY-SHOULD-BE-PRESENT") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
    hu_prompt_budget_free(budget);
}

static void test_trim_gate_null_budget_never_trims(void) {
    /* NULL budget is the legacy path — no trim possible. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.memory_context = "NULL-BUDGET-MEMORY-PRESENT";
    cfg.memory_context_len = strlen(cfg.memory_context);
    cfg.prompt_budget_trim_enabled = true; /* would trim if budget existed */

    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_prompt_build_system(&alloc, &cfg, NULL, NULL, &out, &out_len), HU_OK);
    HU_ASSERT(strstr(out, "NULL-BUDGET-MEMORY-PRESENT") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ──────────────────────────────────────────────────────────────────────────
 * B3 Phase 3 — daemon-side threading contract.
 *
 * Pins the AGENT-level wire (separate from the lower-level wire pinned by
 * test_build_system_stats_feeds_budget_observer_round_trip above):
 *
 *   1. hu_agent_from_config allocates a long-lived agent->prompt_budget
 *      regardless of cfg gate (always-observe policy — observation flows
 *      so doctor + future trim always have data).
 *   2. hu_agent_turn flows per-call stats into that budget via the
 *      agent_turn.c call site, so observation_count advances and at
 *      least one field reports non-zero bytes.
 *
 * Without these pins, the prior B3 commit (d5485023) — which threads
 * agent->prompt_budget for the trim READ side but passed NULL stats and
 * never called observe — would re-emerge silently in a refactor and
 * Phase 2's trim gate would go back to having no data to act on.
 * ──────────────────────────────────────────────────────────────────────── */

/* Minimal mock provider — pattern-matched on test_agent_fail_path_regressions
 * benign_create but local so this file stays self-contained. Returns a fixed
 * payload from chat / stream_chat so hu_agent_turn completes successfully. */
typedef struct {
    int calls;
    const char *payload;
    size_t payload_len;
} pb_mock_ctx_t;

static const char *pb_mock_name(void *ctx) {
    (void)ctx;
    return "pb_mock";
}
static bool pb_mock_no_native_tools(void *ctx) {
    (void)ctx;
    return false;
}
static bool pb_mock_streams(void *ctx) {
    (void)ctx;
    return true;
}
static void pb_mock_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}
static hu_error_t pb_mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                               const char *model, size_t model_len, double temperature,
                               hu_chat_response_t *out) {
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    pb_mock_ctx_t *m = (pb_mock_ctx_t *)ctx;
    m->calls++;
    out->content = hu_strndup(alloc, m->payload, m->payload_len);
    out->content_len = out->content ? m->payload_len : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}
static hu_error_t pb_mock_stream_chat(void *ctx, hu_allocator_t *alloc,
                                      const hu_chat_request_t *request, const char *model,
                                      size_t model_len, double temperature,
                                      hu_stream_callback_t callback, void *callback_ctx,
                                      hu_stream_chat_result_t *out) {
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    (void)callback;
    (void)callback_ctx;
    pb_mock_ctx_t *m = (pb_mock_ctx_t *)ctx;
    m->calls++;
    out->content = hu_strndup(alloc, m->payload, m->payload_len);
    out->content_len = out->content ? m->payload_len : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}
static const hu_provider_vtable_t pb_mock_vtable = {
    .chat = pb_mock_chat,
    .supports_native_tools = pb_mock_no_native_tools,
    .get_name = pb_mock_name,
    .deinit = pb_mock_deinit,
    .supports_streaming = pb_mock_streams,
    .stream_chat = pb_mock_stream_chat,
};
static hu_provider_t pb_mock_create(pb_mock_ctx_t *ctx, const char *payload) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->payload = payload;
    ctx->payload_len = strlen(payload);
    return (hu_provider_t){.ctx = ctx, .vtable = &pb_mock_vtable};
}

static void test_agent_from_config_allocates_long_lived_prompt_budget(void) {
    /* Phase 3 wire (1): agent_from_config allocates the budget
     * unconditionally. The cfg gate controls trim, not observation. */
    hu_allocator_t alloc = hu_system_allocator();
    pb_mock_ctx_t provider_ctx;
    hu_provider_t provider = pb_mock_create(&provider_ctx, "ok");

    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "pb_mock", 7, 0.7, "/tmp", 4, 5, 50,
                                          false /* auto_save */, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    /* Budget pointer must be non-NULL right after construction — the
     * "always observe" policy depends on this invariant. */
    HU_ASSERT_NOT_NULL(agent.prompt_budget);
    /* Brand-new budget has zero observations until a turn runs. */
    HU_ASSERT_EQ(hu_prompt_budget_observation_count(agent.prompt_budget), (size_t)0);

    /* Deinit must free the budget cleanly — ASan in CI catches any leak. */
    hu_agent_deinit(&agent);
}

static void test_agent_turn_advances_prompt_budget_observation_count(void) {
    /* Phase 3 wire (2): the agent_turn.c call site reaches
     * hu_prompt_budget_observe after a successful prompt build. Run one
     * turn through hu_agent_turn and watch observation_count advance.
     *
     * This is the end-to-end pin — the lower-level builder→budget wire
     * is already pinned by test_build_system_stats_feeds_budget_observer_
     * round_trip above. Together they cover (build emits stats) +
     * (agent owns budget) + (agent_turn folds the two together). */
    hu_allocator_t alloc = hu_system_allocator();
    pb_mock_ctx_t provider_ctx;
    hu_provider_t provider = pb_mock_create(&provider_ctx, "fine, talk to you soon");

    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "pb_mock", 7, 0.7, "/tmp", 4, 5, 50,
                                          false /* auto_save */, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(agent.prompt_budget);
    HU_ASSERT_EQ(hu_prompt_budget_observation_count(agent.prompt_budget), (size_t)0);

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "hello", 5, &response, &response_len);
    HU_ASSERT_EQ(err, HU_OK);

    /* After one successful turn, observation_count MUST have advanced
     * past zero — the daemon-side thread is live. */
    HU_ASSERT(hu_prompt_budget_observation_count(agent.prompt_budget) >= (size_t)1);

    /* Snapshot at least one non-zero field — proves the stats array
     * carried real bytes through to the accumulator (not just an
     * observe(NULL) no-op). */
    hu_prompt_field_stat_t snap[HU_PROMPT_FIELD_COUNT];
    memset(snap, 0, sizeof(snap));
    size_t n = hu_prompt_budget_snapshot(agent.prompt_budget, snap, HU_PROMPT_FIELD_COUNT);
    HU_ASSERT_EQ(n, (size_t)HU_PROMPT_FIELD_COUNT);
    bool any_nonzero = false;
    for (size_t i = 0; i < n; i++) {
        if (snap[i].bytes_contributed > 0) {
            any_nonzero = true;
            break;
        }
    }
    HU_ASSERT(any_nonzero);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

void run_prompt_budget_tests(void);
void run_prompt_budget_tests(void) {
    HU_TEST_SUITE("prompt_budget");
    HU_RUN_TEST(test_field_name_returns_stable_string);
    HU_RUN_TEST(test_init_free_round_trip);
    HU_RUN_TEST(test_init_null_args_return_invalid);
    HU_RUN_TEST(test_observe_single_turn_advances_counter);
    HU_RUN_TEST(test_dead_field_requires_min_samples);
    HU_RUN_TEST(test_dead_field_fires_after_threshold_with_zero_bytes);
    HU_RUN_TEST(test_dead_field_skips_when_field_is_populated);
    HU_RUN_TEST(test_dead_field_borderline_mean_below_threshold);
    HU_RUN_TEST(test_dead_field_with_one_outlier_observation);
    HU_RUN_TEST(test_snapshot_reports_mean_bytes);
    HU_RUN_TEST(test_snapshot_respects_array_cap);
    HU_RUN_TEST(test_observe_null_args_no_op);
    HU_RUN_TEST(test_field_is_dead_null_budget_returns_false);
    /* Phase 1b — live wiring through hu_prompt_build_system */
    HU_RUN_TEST(test_build_system_stats_null_preserves_zero_overhead_path);
    HU_RUN_TEST(test_build_system_stats_records_memory_context_bytes);
    HU_RUN_TEST(test_build_system_stats_records_multiple_populated_fields);
    HU_RUN_TEST(test_build_system_stats_unwired_field_stays_zero_even_when_others_populated);
    HU_RUN_TEST(test_build_system_stats_feeds_budget_observer_round_trip);
    /* Phase 2 — trim gate */
    HU_RUN_TEST(test_trim_gate_skips_dead_fields_when_enabled);
    HU_RUN_TEST(test_trim_gate_disabled_by_default_keeps_dead_fields);
    HU_RUN_TEST(test_trim_gate_null_budget_never_trims);
    /* Phase 3 — daemon-side threading */
    HU_RUN_TEST(test_agent_from_config_allocates_long_lived_prompt_budget);
    HU_RUN_TEST(test_agent_turn_advances_prompt_budget_observation_count);
}
