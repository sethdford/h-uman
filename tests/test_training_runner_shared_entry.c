/* tests/test_training_runner_shared_entry.c — Spec 2026-05-19 Task 6.
 *
 * Pins AC-RL-2: both reaction-loop triggers (the existing
 * learner-pending check at ~10 W13 signals AND the new DPO pair-count
 * check at ≥ `learning.dpo_pair_training_threshold`) route through
 * `hu_training_runner_enqueue_lora_persona` and produce structurally
 * identical scheduler queue records — only `trigger_reason` differs (a
 * label propagated to the log line, not stored on the queue row).
 *
 * "Structurally identical" means: same `kind`, same `priority`, same
 * `requires_idle` / `requires_ac_power` / `interval_sec` columns in the
 * `scheduler_jobs` table. The trigger_reason is intentionally NOT
 * stored — log analytics distinguish the two paths.
 *
 * The whole test file is gated behind HU_ENABLE_LEARNING because the
 * shared entry is a no-op stub otherwise (per the OFF-build contract
 * pinned in test_dpo_pair_count_trigger.c). See
 * ~/.claude/rules/test-source-gate-symmetry.md — we use the
 * internal-#ifdef-wrap-with-stub-runner pattern so CMake doesn't need
 * a parallel gate. */

#include "test_framework.h"

#if defined(HU_ENABLE_LEARNING) && defined(HU_ENABLE_SQLITE)

#include "human/agent/training_runner_shared.h"
#include "human/agent/world_model_bridge.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

/* Open an in-memory graph + W7 facade + W14 scheduler for one test. The
 * caller must call `teardown_scheduler` to free everything in the right
 * order (scheduler before facade before graph) — the bridge borrows the
 * facade and the facade borrows the graph. */
typedef struct fixture {
    hu_graph_t *g;
    hu_w7_facade_t *f;
    hu_w14_scheduler_t *s;
} fixture_t;

static void setup_scheduler(fixture_t *fx) {
    memset(fx, 0, sizeof(*fx));
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, &fx->g), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(fx->g, A(), &fx->f), HU_OK);
    HU_ASSERT_EQ(hu_w14_scheduler_open(fx->f, A(), &fx->s), HU_OK);
}

static void teardown_scheduler(fixture_t *fx) {
    if (fx->s)
        hu_w14_scheduler_close(fx->s, A());
    if (fx->f)
        hu_w7_facade_close(fx->f, A());
    if (fx->g)
        hu_graph_close(fx->g, A());
}

/* Read the most-recently-inserted (max id) row from scheduler_jobs and
 * fill in the columns we care about for equivalence checks. Returns 0
 * on success, -1 on missing row / SQL error. */
typedef struct queued_record {
    int kind;
    int priority;
    int budget_ms;
    int requires_idle;
    int requires_ac_power;
    int interval_sec;
} queued_record_t;

static int read_last_queue_record(struct sqlite3 *db, queued_record_t *out) {
    static const char *const sql =
        "SELECT kind, priority, budget_ms, requires_idle, requires_ac_power, interval_sec "
        "FROM scheduler_jobs ORDER BY id DESC LIMIT 1";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return -1;
    }
    out->kind = sqlite3_column_int(st, 0);
    out->priority = sqlite3_column_int(st, 1);
    out->budget_ms = sqlite3_column_int(st, 2);
    out->requires_idle = sqlite3_column_int(st, 3);
    out->requires_ac_power = sqlite3_column_int(st, 4);
    out->interval_sec = sqlite3_column_int(st, 5);
    sqlite3_finalize(st);
    return 0;
}

static int count_pending_lora_jobs(struct sqlite3 *db) {
    /* HU_JOB_LORA_TRAINING == 5 per the enum order in
     * include/human/agent/scheduler.h. We don't include that header
     * here (it would pull legacy memory.h into a TU that includes
     * memory/memory.h, the W7 facade — same collision pattern as the
     * other bridge tests). Use the integer constant; if the enum is
     * reordered this assertion will diagnose it. */
    static const char *const sql =
        "SELECT COUNT(*) FROM scheduler_jobs WHERE kind = 5 AND status = 'pending'";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    int n = -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* AC-RL-2: both triggers produce structurally identical scheduler
 * records. We enqueue once with each trigger_reason and assert the
 * fields match exactly. */
static void test_shared_entry_learner_pending_and_pair_count_produce_equivalent_records(void) {
    fixture_t fx;
    setup_scheduler(&fx);

    struct sqlite3 *db = hu_memory_facade_sqlite_db(hu_w7_facade_memory_handle(fx.f));
    HU_ASSERT_NOT_NULL(db);

    /* First: learner-pending trigger. */
    HU_ASSERT_EQ(hu_training_runner_enqueue_lora_persona(fx.s, /*now_ms=*/12345,
                                                         /*budget_ms=*/300000,
                                                         HU_TRAINING_TRIGGER_LEARNER_PENDING, NULL),
                 HU_OK);
    queued_record_t learner_rec;
    HU_ASSERT_EQ(read_last_queue_record(db, &learner_rec), 0);

    /* Second: pair-count trigger. Same now_ms + budget_ms so any
     * structural difference is attributable to the trigger_reason
     * propagation, which MUST NOT affect the scheduler row. */
    HU_ASSERT_EQ(hu_training_runner_enqueue_lora_persona(fx.s, /*now_ms=*/12345,
                                                         /*budget_ms=*/300000,
                                                         HU_TRAINING_TRIGGER_PAIR_COUNT, NULL),
                 HU_OK);
    queued_record_t pair_rec;
    HU_ASSERT_EQ(read_last_queue_record(db, &pair_rec), 0);

    /* Structural equivalence — every scheduler-visible field equal. */
    HU_ASSERT_EQ(learner_rec.kind, pair_rec.kind);
    HU_ASSERT_EQ(learner_rec.priority, pair_rec.priority);
    HU_ASSERT_EQ(learner_rec.budget_ms, pair_rec.budget_ms);
    HU_ASSERT_EQ(learner_rec.requires_idle, pair_rec.requires_idle);
    HU_ASSERT_EQ(learner_rec.requires_ac_power, pair_rec.requires_ac_power);
    HU_ASSERT_EQ(learner_rec.interval_sec, pair_rec.interval_sec);

    /* Both rows are HU_JOB_LORA_TRAINING (kind == 5). */
    HU_ASSERT_EQ(learner_rec.kind, 5);
    HU_ASSERT_EQ(pair_rec.kind, 5);

    /* And exactly two LoRA-training jobs are pending — one per call,
     * no coalescing (spec D-RL-5: AC-RL-6 deferred). */
    HU_ASSERT_EQ(count_pending_lora_jobs(db), 2);

    teardown_scheduler(&fx);
}

/* Defensive: pair-count trigger over an empty queue produces exactly
 * one LoRA-training row. Pinned so a future refactor that accidentally
 * inserts twice (or zero times) is caught at unit-test cost. */
static void test_shared_entry_pair_count_enqueues_exactly_one_record(void) {
    fixture_t fx;
    setup_scheduler(&fx);

    struct sqlite3 *db = hu_memory_facade_sqlite_db(hu_w7_facade_memory_handle(fx.f));
    HU_ASSERT_NOT_NULL(db);

    HU_ASSERT_EQ(count_pending_lora_jobs(db), 0);

    HU_ASSERT_EQ(
        hu_training_runner_enqueue_lora_persona(fx.s, 0, 0, HU_TRAINING_TRIGGER_PAIR_COUNT, NULL),
        HU_OK);

    HU_ASSERT_EQ(count_pending_lora_jobs(db), 1);

    teardown_scheduler(&fx);
}

/* Defensive: NULL scheduler is HU_ERR_INVALID_ARGUMENT, never silent
 * success. Already covered by test_dpo_pair_count_trigger.c but
 * re-pinning here with HU_ENABLE_LEARNING set so the dispatch path is
 * also covered, not just the early NULL check. */
static void test_shared_entry_null_scheduler_returns_invalid_argument(void) {
    HU_ASSERT_EQ(hu_training_runner_enqueue_lora_persona(NULL, 0, 0,
                                                         HU_TRAINING_TRIGGER_LEARNER_PENDING, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

void run_training_runner_shared_entry_tests(void) {
    HU_TEST_SUITE("Training runner shared entry");
    HU_RUN_TEST(test_shared_entry_learner_pending_and_pair_count_produce_equivalent_records);
    HU_RUN_TEST(test_shared_entry_pair_count_enqueues_exactly_one_record);
    HU_RUN_TEST(test_shared_entry_null_scheduler_returns_invalid_argument);
}

#else /* !HU_ENABLE_LEARNING || !HU_ENABLE_SQLITE */

/* Stub runner so the symbol resolves at link time regardless of the
 * build configuration — see ~/.claude/rules/test-source-gate-symmetry.md
 * internal-#ifdef-wrap-with-stub-runner pattern. */
void run_training_runner_shared_entry_tests(void) {
    (void)0;
}

#endif
