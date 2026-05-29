/* ─────────────────────────────────────────────────────────────────────────
 * test_intrinsic_drive.c
 *
 * Pins intrinsic motivation (A3): drive dynamics, the safety-bearing start
 * predicate (preemption / budget / rate / drive threshold), self-originated
 * goal distinctness from user-reactive autonomy goals, and the share gate.
 *
 * Spec: docs/plans/2026-05-29-intrinsic-motivation/ (ACs 1, 2, 4, 5, 6)
 * ───────────────────────────────────────────────────────────────────────── */

#include "human/cognition/intrinsic_drive.h"
#include "test_framework.h"

#include <string.h>

/* ── Drive dynamics (AC-1) ──────────────────────────────────────────────── */

static void drive_rises_without_user_activity(void) {
    hu_intrinsic_drive_t d = {0};
    for (int i = 0; i < 5; i++)
        hu_intrinsic_drive_tick(&d, false, (int64_t)(i * 60));
    HU_ASSERT_TRUE(hu_intrinsic_drive_level(&d) > 0.0);
    HU_ASSERT_TRUE(d.boredom > 0.0);
}

static void drive_decays_on_user_activity(void) {
    hu_intrinsic_drive_t d = {0};
    for (int i = 0; i < 8; i++)
        hu_intrinsic_drive_tick(&d, false, (int64_t)(i * 60)); /* build up */
    double before = hu_intrinsic_drive_level(&d);
    hu_intrinsic_drive_tick(&d, true, 1000); /* user shows up */
    HU_ASSERT_TRUE(hu_intrinsic_drive_level(&d) < before);
    HU_ASSERT_EQ(d.last_user_ts, 1000);
}

static void drive_clamps_to_unit_interval(void) {
    hu_intrinsic_drive_t d = {0};
    for (int i = 0; i < 100; i++)
        hu_intrinsic_drive_tick(&d, false, (int64_t)i);
    HU_ASSERT_TRUE(d.boredom <= 1.0 + 1e-9);
    HU_ASSERT_TRUE(d.curiosity <= 1.0 + 1e-9);
}

/* ── Start predicate (AC-6, AC-4, AC-3) ─────────────────────────────────── */

static hu_intrinsic_start_facts_t ripe_facts(void) {
    hu_intrinsic_start_facts_t f;
    f.drive_level = 0.8;
    f.secs_since_user = HU_INTRINSIC_MIN_QUIET_SECS + 1;
    f.secs_since_intrinsic = HU_INTRINSIC_MIN_INTERVAL_SECS + 1;
    f.budget_tokens_remaining = HU_INTRINSIC_MIN_BUDGET_TOKENS + 1;
    f.user_active = false;
    return f;
}

static void start_when_ripe(void) {
    hu_intrinsic_start_facts_t f = ripe_facts();
    HU_ASSERT_TRUE(hu_intrinsic_should_start(&f));
}

/* THE load-bearing guarantee: a user turn in flight always preempts. */
static void start_vetoed_by_user_active(void) {
    hu_intrinsic_start_facts_t f = ripe_facts();
    f.user_active = true;
    HU_ASSERT_FALSE(hu_intrinsic_should_start(&f));
}

static void start_vetoed_by_low_budget(void) {
    hu_intrinsic_start_facts_t f = ripe_facts();
    f.budget_tokens_remaining = HU_INTRINSIC_MIN_BUDGET_TOKENS - 1;
    HU_ASSERT_FALSE(hu_intrinsic_should_start(&f));
}

static void start_vetoed_by_recent_user(void) {
    hu_intrinsic_start_facts_t f = ripe_facts();
    f.secs_since_user = HU_INTRINSIC_MIN_QUIET_SECS - 1;
    HU_ASSERT_FALSE(hu_intrinsic_should_start(&f));
}

static void start_vetoed_by_rate_limit(void) {
    hu_intrinsic_start_facts_t f = ripe_facts();
    f.secs_since_intrinsic = HU_INTRINSIC_MIN_INTERVAL_SECS - 1;
    HU_ASSERT_FALSE(hu_intrinsic_should_start(&f));
}

static void start_vetoed_by_low_drive(void) {
    hu_intrinsic_start_facts_t f = ripe_facts();
    f.drive_level = HU_INTRINSIC_DRIVE_THRESHOLD - 0.01;
    HU_ASSERT_FALSE(hu_intrinsic_should_start(&f));
}

static void start_null_is_false(void) {
    HU_ASSERT_FALSE(hu_intrinsic_should_start(NULL));
}

/* ── Self-originated goal distinct from autonomy (AC-2) ─────────────────── */

static void goal_origin_is_intrinsic_and_distinct(void) {
    hu_intrinsic_drive_t d = {0};
    d.curiosity = 0.9;
    hu_intrinsic_goal_t g;
    hu_intrinsic_make_goal(&d, &g);
    HU_ASSERT_STR_EQ(g.origin, "intrinsic_curiosity");
    /* MUST NOT be one of autonomy.c's user-reactive descriptions. */
    HU_ASSERT_STR_NOT_CONTAINS(g.description, "failures");
    HU_ASSERT_STR_NOT_CONTAINS(g.description, "completed tasks");
    HU_ASSERT_STR_NOT_CONTAINS(g.description, "pending schedules");
    HU_ASSERT_TRUE(strlen(g.description) > 0);
}

/* ── Share gate (AC-5): only via the proposer's confidence bar ──────────── */

static void share_requires_proposer_confidence(void) {
    HU_ASSERT_TRUE(hu_intrinsic_may_share(0.9));
    HU_ASSERT_TRUE(hu_intrinsic_may_share(HU_INTRINSIC_SHARE_MIN_CONFIDENCE));
    HU_ASSERT_FALSE(hu_intrinsic_may_share(0.5));
    HU_ASSERT_FALSE(hu_intrinsic_may_share(0.84));
}

/* ── Bounded runner (AC-3, AC-7, AC-8) ──────────────────────────────────── */

static hu_intrinsic_start_facts_t runner_ripe_facts(void) {
    hu_intrinsic_start_facts_t f;
    f.drive_level = 0.8;
    f.secs_since_user = HU_INTRINSIC_MIN_QUIET_SECS + 1;
    f.secs_since_intrinsic = HU_INTRINSIC_MIN_INTERVAL_SECS + 1;
    f.budget_tokens_remaining = HU_INTRINSIC_DEFAULT_TICK_BUDGET + 1;
    f.user_active = false;
    return f;
}

/* AC-8: disabled config -> DISABLED, nothing touched. */
static void runner_disabled_is_noop(void) {
    hu_intrinsic_drive_t d = {0};
    hu_intrinsic_runtime_cfg_t cfg = {.enabled = false, .per_tick_token_budget = 0};
    hu_intrinsic_start_facts_t f = runner_ripe_facts();
    hu_intrinsic_tick_result_t r;
    hu_intrinsic_run_tick(&d, &cfg, &f, NULL, 5000, &r);
    HU_ASSERT_EQ((int)r.outcome, (int)HU_INTRINSIC_TICK_DISABLED);
    HU_ASSERT_EQ(d.last_intrinsic_ts, 0); /* untouched */
}

/* AC-7: enabled + ripe -> STARTED, audit string records origin + outcome. */
static void runner_enabled_ripe_starts_and_audits(void) {
    hu_intrinsic_drive_t d = {0};
    d.curiosity = 0.9;
    hu_intrinsic_runtime_cfg_t cfg = {.enabled = true, .per_tick_token_budget = 0};
    hu_intrinsic_start_facts_t f = runner_ripe_facts();
    hu_intrinsic_tick_result_t r;
    hu_intrinsic_run_tick(&d, &cfg, &f, NULL, 5000, &r);
    HU_ASSERT_EQ((int)r.outcome, (int)HU_INTRINSIC_TICK_STARTED);
    HU_ASSERT_EQ(d.last_intrinsic_ts, 5000); /* advanced */
    HU_ASSERT_STR_CONTAINS(r.audit, "origin=intrinsic_curiosity");
    HU_ASSERT_STR_CONTAINS(r.audit, "outcome=started");
}

/* AC-4: enabled but a user turn is in flight -> SKIPPED (preemption). */
static void runner_user_active_skips(void) {
    hu_intrinsic_drive_t d = {0};
    hu_intrinsic_runtime_cfg_t cfg = {.enabled = true, .per_tick_token_budget = 0};
    hu_intrinsic_start_facts_t f = runner_ripe_facts();
    f.user_active = true;
    hu_intrinsic_tick_result_t r;
    hu_intrinsic_run_tick(&d, &cfg, &f, NULL, 5000, &r);
    HU_ASSERT_EQ((int)r.outcome, (int)HU_INTRINSIC_TICK_SKIPPED);
    HU_ASSERT_EQ(d.last_intrinsic_ts, 0);
}

/* AC-3: enabled but below the per-tick budget cap -> SKIPPED. */
static void runner_below_tick_budget_skips(void) {
    hu_intrinsic_drive_t d = {0};
    hu_intrinsic_runtime_cfg_t cfg = {.enabled = true, .per_tick_token_budget = 10000};
    hu_intrinsic_start_facts_t f = runner_ripe_facts();
    f.budget_tokens_remaining = 9999; /* below the configured cap */
    hu_intrinsic_tick_result_t r;
    hu_intrinsic_run_tick(&d, &cfg, &f, NULL, 5000, &r);
    HU_ASSERT_EQ((int)r.outcome, (int)HU_INTRINSIC_TICK_SKIPPED);
}

void run_intrinsic_drive_tests(void);
void run_intrinsic_drive_tests(void) {
    HU_TEST_SUITE("intrinsic_drive");
    HU_RUN_TEST(drive_rises_without_user_activity);
    HU_RUN_TEST(drive_decays_on_user_activity);
    HU_RUN_TEST(drive_clamps_to_unit_interval);
    HU_RUN_TEST(start_when_ripe);
    HU_RUN_TEST(start_vetoed_by_user_active);
    HU_RUN_TEST(start_vetoed_by_low_budget);
    HU_RUN_TEST(start_vetoed_by_recent_user);
    HU_RUN_TEST(start_vetoed_by_rate_limit);
    HU_RUN_TEST(start_vetoed_by_low_drive);
    HU_RUN_TEST(start_null_is_false);
    HU_RUN_TEST(goal_origin_is_intrinsic_and_distinct);
    HU_RUN_TEST(share_requires_proposer_confidence);
    HU_RUN_TEST(runner_disabled_is_noop);
    HU_RUN_TEST(runner_enabled_ripe_starts_and_audits);
    HU_RUN_TEST(runner_user_active_skips);
    HU_RUN_TEST(runner_below_tick_budget_skips);
}
