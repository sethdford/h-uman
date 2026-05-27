/* tests/test_reflection_orchestration.c — Orchestration (T5).
 *
 * Pins the gate truth table + the four-layer failure-mode contract
 * from docs/plans/2026-05-26-reflection-loop/{design.md,tasks.md}
 * Task 5:
 *
 *   AC-T5.1  pure gate returns DISABLED whenever cfg->enabled is false
 *   AC-T5.2  force bypasses interval + idle but NOT disabled
 *   AC-T5.3  min_interval gate blocks "too soon after last run"
 *   AC-T5.4  idle threshold + min_interval both ok → RUN_IDLE
 *   AC-T5.5  daily_floor override fires even when not idle
 *   AC-T5.6  run() with gated verdict → status=GATED, no SQL writes
 *   AC-T5.7  run() with zero turns → status=NO_INPUT, no SQL writes
 *   AC-T5.8  run() happy path → status=OK, run row + pattern rows landed
 *   AC-T5.9  run() with provider error → status=PROVIDER_ERROR, run row
 *            marked provider_error with error_message captured
 *   AC-T5.10 run() with malformed model output → status=SCHEMA_INVALID,
 *            run row marked schema_invalid
 *   AC-T5.11 run() drops patterns with confidence < 0.5 (storage layer's
 *            floor) while keeping the others
 *
 * The mock provider is a minimal hu_provider_t with a configurable
 * canned response + forced-error-code so all four failure modes can
 * be exercised without spinning up real network or a real LLM. */

#include "human/config_types.h"
#include "human/provider.h"
#include "human/reflection.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

/* ── Static-array turn iter (same shape as T4 tests) ────────────── */

typedef struct {
    const hu_reflection_turn_t *turns;
    int count;
    int pos;
} static_iter_ctx_t;

static bool static_iter(void *vctx, hu_reflection_turn_t *out_turn) {
    static_iter_ctx_t *c = (static_iter_ctx_t *)vctx;
    if (c->pos >= c->count)
        return false;
    *out_turn = c->turns[c->pos++];
    return true;
}

/* ── Mock provider ──────────────────────────────────────────────── */

typedef struct {
    const char *canned_response; /* what chat_with_system returns */
    hu_error_t forced_error;     /* HU_OK = success; otherwise pretend the provider failed */
    int call_count;
    char *last_system; /* heap-copy captured each call so tests can inspect */
    char *last_message;
} mock_provider_ctx_t;

static char *dup_alloc(hu_allocator_t *alloc, const char *s, size_t n) {
    if (!s)
        return NULL;
    char *p = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!p)
        return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static hu_error_t mock_chat_with_system(void *ctx, hu_allocator_t *alloc, const char *sys,
                                        size_t sys_len, const char *msg, size_t msg_len,
                                        const char *model, size_t model_len, double temp,
                                        char **out, size_t *out_len) {
    (void)model;
    (void)model_len;
    (void)temp;
    mock_provider_ctx_t *m = (mock_provider_ctx_t *)ctx;
    m->call_count++;
    if (m->last_system)
        alloc->free(alloc->ctx, m->last_system, strlen(m->last_system) + 1);
    if (m->last_message)
        alloc->free(alloc->ctx, m->last_message, strlen(m->last_message) + 1);
    m->last_system = dup_alloc(alloc, sys, sys_len);
    m->last_message = dup_alloc(alloc, msg, msg_len);

    if (m->forced_error != HU_OK) {
        *out = NULL;
        *out_len = 0;
        return m->forced_error;
    }

    size_t n = strlen(m->canned_response);
    *out = dup_alloc(alloc, m->canned_response, n);
    *out_len = n;
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static bool mock_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}
static const char *mock_get_name(void *ctx) {
    (void)ctx;
    return "mock";
}
static void mock_deinit(void *ctx, hu_allocator_t *alloc) {
    mock_provider_ctx_t *m = (mock_provider_ctx_t *)ctx;
    if (!m)
        return;
    if (m->last_system)
        alloc->free(alloc->ctx, m->last_system, strlen(m->last_system) + 1);
    if (m->last_message)
        alloc->free(alloc->ctx, m->last_message, strlen(m->last_message) + 1);
    m->last_system = NULL;
    m->last_message = NULL;
}

static const hu_provider_vtable_t mock_vtable = {
    .chat_with_system = mock_chat_with_system,
    .supports_native_tools = mock_supports_native_tools,
    .get_name = mock_get_name,
    .deinit = mock_deinit,
};

/* ── Test fixture helpers ───────────────────────────────────────── */

static void default_enabled_cfg(hu_reflection_loop_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
    cfg->min_interval_hours = 12;
    cfg->idle_threshold_hours = 2;
    cfg->daily_floor_hours = 24;
    snprintf(cfg->provider, sizeof cfg->provider, "%s", "mock");
}

static const uint64_t HOUR_MS = 3600ULL * 1000ULL;

/* ── AC-T5.1: disabled ───────────────────────────────────────────── */

static void test_should_run_disabled_returns_disabled(void) {
    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    cfg.enabled = false;
    HU_ASSERT_EQ((int)hu_reflection_should_run(&cfg, 0, 0, 1000, false),
                 (int)HU_REFLECTION_GATE_DISABLED);
    /* force=true CANNOT bypass disabled (AC-T5.2 second half). */
    HU_ASSERT_EQ((int)hu_reflection_should_run(&cfg, 0, 0, 1000, true),
                 (int)HU_REFLECTION_GATE_DISABLED);
    /* NULL cfg also treated as disabled. */
    HU_ASSERT_EQ((int)hu_reflection_should_run(NULL, 0, 0, 1000, false),
                 (int)HU_REFLECTION_GATE_DISABLED);
}

/* ── AC-T5.2: force bypasses interval + idle ──────────────────────── */

static void test_should_run_force_bypasses_interval_and_idle(void) {
    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    uint64_t now = 1000 * HOUR_MS;
    /* Last run was 1 second ago (way inside interval) AND no idle. */
    HU_ASSERT_EQ((int)hu_reflection_should_run(&cfg, now - 1000, now, now, true),
                 (int)HU_REFLECTION_GATE_RUN_FORCED);
}

/* ── AC-T5.3: interval gate ───────────────────────────────────────── */

static void test_should_run_inside_interval_returns_interval(void) {
    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    uint64_t now = 1000 * HOUR_MS;
    /* Last completed 6h ago, min_interval is 12h → INTERVAL. */
    HU_ASSERT_EQ(
        (int)hu_reflection_should_run(&cfg, now - 6 * HOUR_MS, now - 5 * HOUR_MS, now, false),
        (int)HU_REFLECTION_GATE_INTERVAL);
}

/* ── AC-T5.4: idle + interval ok → RUN_IDLE ───────────────────────── */

static void test_should_run_idle_and_interval_ok_returns_run_idle(void) {
    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    uint64_t now = 1000 * HOUR_MS;
    /* Last completed 13h ago (≥12h interval), last user activity 3h ago (≥2h idle). */
    HU_ASSERT_EQ(
        (int)hu_reflection_should_run(&cfg, now - 13 * HOUR_MS, now - 3 * HOUR_MS, now, false),
        (int)HU_REFLECTION_GATE_RUN_IDLE);
}

static void test_should_run_no_prior_run_with_idle_runs(void) {
    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    uint64_t now = 1000 * HOUR_MS;
    /* No prior run (last_completed_ms=0), idle ≥ threshold → RUN_IDLE. */
    HU_ASSERT_EQ((int)hu_reflection_should_run(&cfg, 0, now - 3 * HOUR_MS, now, false),
                 (int)HU_REFLECTION_GATE_RUN_IDLE);
}

static void test_should_run_not_idle_returns_not_idle(void) {
    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    uint64_t now = 1000 * HOUR_MS;
    /* Interval ok (13h) but only 30 min of idle (< 2h threshold) and not yet
     * past daily_floor (24h). */
    HU_ASSERT_EQ(
        (int)hu_reflection_should_run(&cfg, now - 13 * HOUR_MS, now - HOUR_MS / 2, now, false),
        (int)HU_REFLECTION_GATE_NOT_IDLE);
}

/* ── AC-T5.5: daily_floor override ────────────────────────────────── */

static void test_should_run_daily_floor_bypasses_idle(void) {
    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    uint64_t now = 1000 * HOUR_MS;
    /* Last completed 25h ago — past daily_floor (24h). User active 1 min ago
     * (not idle). The floor should fire RUN_FORCED anyway. */
    HU_ASSERT_EQ(
        (int)hu_reflection_should_run(&cfg, now - 25 * HOUR_MS, now - 60 * 1000, now, false),
        (int)HU_REFLECTION_GATE_RUN_FORCED);
}

/* ── Run-level tests with mock provider ──────────────────────────── */

/* Sample valid response that should produce 2 kept patterns + 1 dropped. */
static const char *k_valid_response =
    "{"
    "  \"prose_summary\": \"Seth replies tersely at night and reads jazz pieces.\","
    "  \"patterns\": ["
    "    {\"type\": \"behavioral_shift\", \"subject\": \"Seth\","
    "     \"observation\": \"terser replies after 9pm weeknights\","
    "     \"confidence\": 0.82,"
    "     \"evidence_ids\": [\"t_001\"], \"channels\": [\"imessage\"]},"
    "    {\"type\": \"preference\", \"subject\": \"Seth\","
    "     \"observation\": \"jazz music between 3-5pm\","
    "     \"confidence\": 0.71,"
    "     \"evidence_ids\": [\"t_003\"], \"channels\": [\"imessage\"]},"
    "    {\"type\": \"preference\", \"subject\": \"Seth\","
    "     \"observation\": \"weak signal about coffee\","
    "     \"confidence\": 0.3,"
    "     \"evidence_ids\": [\"t_002\"], \"channels\": [\"imessage\"]}"
    "  ]"
    "}";

static const hu_reflection_turn_t k_two_turns[] = {
    {.turn_id = "t_001",
     .channel = "imessage",
     .sender = "user",
     .ts_ms = 1779840000000ULL,
     .content = "barely slept this week"},
    {.turn_id = "t_002",
     .channel = "imessage",
     .sender = "user",
     .ts_ms = 1779843600000ULL,
     .content = "work has been wild"},
};

static int count_rows(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    int rc = sqlite3_step(st);
    int n = (rc == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

/* AC-T5.6: gated verdict → no run row, no pattern row. */
static void test_run_gated_returns_gated_status_no_writes(void) {
    hu_reflection_reset_warn_guards_for_test();
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ((int)hu_reflection_storage_migrate(db), (int)HU_OK);

    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    cfg.enabled = false; /* gate via DISABLED for simplicity */

    mock_provider_ctx_t mctx = {.canned_response = k_valid_response};
    hu_provider_t prov = {.ctx = &mctx, .vtable = &mock_vtable};
    hu_allocator_t alloc = hu_system_allocator();

    static_iter_ctx_t iter_ctx = {.turns = k_two_turns, .count = 2, .pos = 0};
    hu_reflection_run_inputs_t inputs = {
        .db = db,
        .cfg = &cfg,
        .provider = &prov,
        .alloc = &alloc,
        .iter_fn = static_iter,
        .iter_ctx = &iter_ctx,
        .last_user_activity_ms = 0,
        .now_ms = 1000 * HOUR_MS,
        .max_input_chars = 0,
    };
    hu_reflection_run_status_t status = HU_REFLECTION_RUN_OK;
    int kept = 99, dropped = 99;
    HU_ASSERT_EQ((int)hu_reflection_run(&inputs, false, &status, &kept, &dropped), (int)HU_OK);
    HU_ASSERT_EQ((int)status, (int)HU_REFLECTION_RUN_GATED);
    HU_ASSERT_EQ(kept, 0);
    HU_ASSERT_EQ(dropped, 0);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM reflection_runs"), 0);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM reflection_patterns"), 0);
    HU_ASSERT_EQ(mctx.call_count, 0); /* provider never called */

    mock_deinit(&mctx, &alloc);
    sqlite3_close(db);
}

/* AC-T5.7: zero-turn iter → NO_INPUT, no run row. */
static void test_run_no_input_returns_no_input_no_row_inserted(void) {
    hu_reflection_reset_warn_guards_for_test();
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    mock_provider_ctx_t mctx = {.canned_response = k_valid_response};
    hu_provider_t prov = {.ctx = &mctx, .vtable = &mock_vtable};
    hu_allocator_t alloc = hu_system_allocator();
    static_iter_ctx_t iter_ctx = {.turns = NULL, .count = 0, .pos = 0};
    hu_reflection_run_inputs_t inputs = {
        .db = db,
        .cfg = &cfg,
        .provider = &prov,
        .alloc = &alloc,
        .iter_fn = static_iter,
        .iter_ctx = &iter_ctx,
        .last_user_activity_ms = 0,
        .now_ms = 1000 * HOUR_MS,
        .max_input_chars = 0,
    };
    hu_reflection_run_status_t status = HU_REFLECTION_RUN_OK;
    HU_ASSERT_EQ((int)hu_reflection_run(&inputs, true, &status, NULL, NULL), (int)HU_OK);
    HU_ASSERT_EQ((int)status, (int)HU_REFLECTION_RUN_NO_INPUT);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM reflection_runs"), 0);
    HU_ASSERT_EQ(mctx.call_count, 0);

    mock_deinit(&mctx, &alloc);
    sqlite3_close(db);
}

/* AC-T5.8 + AC-T5.11: happy path keeps high-conf patterns, drops low-conf. */
static void test_run_happy_path_keeps_high_conf_drops_low_conf(void) {
    hu_reflection_reset_warn_guards_for_test();
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    mock_provider_ctx_t mctx = {.canned_response = k_valid_response};
    hu_provider_t prov = {.ctx = &mctx, .vtable = &mock_vtable};
    hu_allocator_t alloc = hu_system_allocator();
    static_iter_ctx_t iter_ctx = {.turns = k_two_turns, .count = 2, .pos = 0};
    hu_reflection_run_inputs_t inputs = {
        .db = db,
        .cfg = &cfg,
        .provider = &prov,
        .alloc = &alloc,
        .iter_fn = static_iter,
        .iter_ctx = &iter_ctx,
        .last_user_activity_ms = 0,
        .now_ms = 1000 * HOUR_MS,
        .max_input_chars = 0,
    };
    hu_reflection_run_status_t status = HU_REFLECTION_RUN_GATED;
    int kept = 0, dropped = 0;
    HU_ASSERT_EQ((int)hu_reflection_run(&inputs, true, &status, &kept, &dropped), (int)HU_OK);
    HU_ASSERT_EQ((int)status, (int)HU_REFLECTION_RUN_OK);
    /* 2 patterns kept (conf 0.82, 0.71), 1 dropped (conf 0.3). */
    HU_ASSERT_EQ(kept, 2);
    HU_ASSERT_EQ(dropped, 1);
    /* Run row inserted + status='ok'. */
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM reflection_runs WHERE status='ok'"), 1);
    /* 2 pattern rows landed (the 0.3-confidence one dropped at storage). */
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM reflection_patterns"), 2);
    /* Provider called exactly once with our system prompt + user message. */
    HU_ASSERT_EQ(mctx.call_count, 1);
    HU_ASSERT(mctx.last_system != NULL);
    HU_ASSERT(strstr(mctx.last_system, "topic_recurrence") != NULL); /* prompt was the real one */
    HU_ASSERT(mctx.last_message != NULL);
    HU_ASSERT(strstr(mctx.last_message, "[id=t_001]") != NULL); /* turns reached the model */

    mock_deinit(&mctx, &alloc);
    sqlite3_close(db);
}

/* AC-T5.9: provider error → run row marked provider_error. */
static void test_run_provider_error_records_provider_error_row(void) {
    hu_reflection_reset_warn_guards_for_test();
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    mock_provider_ctx_t mctx = {.canned_response = NULL,
                                .forced_error = HU_ERR_PROVIDER_UNAVAILABLE};
    hu_provider_t prov = {.ctx = &mctx, .vtable = &mock_vtable};
    hu_allocator_t alloc = hu_system_allocator();
    static_iter_ctx_t iter_ctx = {.turns = k_two_turns, .count = 2, .pos = 0};
    hu_reflection_run_inputs_t inputs = {
        .db = db,
        .cfg = &cfg,
        .provider = &prov,
        .alloc = &alloc,
        .iter_fn = static_iter,
        .iter_ctx = &iter_ctx,
        .last_user_activity_ms = 0,
        .now_ms = 1000 * HOUR_MS,
        .max_input_chars = 0,
    };
    hu_reflection_run_status_t status = HU_REFLECTION_RUN_OK;
    HU_ASSERT_EQ((int)hu_reflection_run(&inputs, true, &status, NULL, NULL), (int)HU_OK);
    HU_ASSERT_EQ((int)status, (int)HU_REFLECTION_RUN_PROVIDER_ERROR);
    HU_ASSERT_EQ(
        count_rows(db, "SELECT COUNT(*) FROM reflection_runs WHERE status='provider_error'"), 1);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM reflection_patterns"), 0);

    mock_deinit(&mctx, &alloc);
    sqlite3_close(db);
}

/* AC-T5.10: malformed response → run row marked schema_invalid. */
static void test_run_schema_invalid_records_schema_invalid_row(void) {
    hu_reflection_reset_warn_guards_for_test();
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    hu_reflection_loop_config_t cfg;
    default_enabled_cfg(&cfg);
    mock_provider_ctx_t mctx = {.canned_response = "not json at all"};
    hu_provider_t prov = {.ctx = &mctx, .vtable = &mock_vtable};
    hu_allocator_t alloc = hu_system_allocator();
    static_iter_ctx_t iter_ctx = {.turns = k_two_turns, .count = 2, .pos = 0};
    hu_reflection_run_inputs_t inputs = {
        .db = db,
        .cfg = &cfg,
        .provider = &prov,
        .alloc = &alloc,
        .iter_fn = static_iter,
        .iter_ctx = &iter_ctx,
        .last_user_activity_ms = 0,
        .now_ms = 1000 * HOUR_MS,
        .max_input_chars = 0,
    };
    hu_reflection_run_status_t status = HU_REFLECTION_RUN_OK;
    HU_ASSERT_EQ((int)hu_reflection_run(&inputs, true, &status, NULL, NULL), (int)HU_OK);
    HU_ASSERT_EQ((int)status, (int)HU_REFLECTION_RUN_SCHEMA_INVALID);
    HU_ASSERT_EQ(
        count_rows(db, "SELECT COUNT(*) FROM reflection_runs WHERE status='schema_invalid'"), 1);
    HU_ASSERT_EQ(count_rows(db, "SELECT COUNT(*) FROM reflection_patterns"), 0);

    mock_deinit(&mctx, &alloc);
    sqlite3_close(db);
}

/* NULL-input defense. */
static void test_run_null_inputs_rejected(void) {
    HU_ASSERT_NEQ((int)hu_reflection_run(NULL, false, NULL, NULL, NULL), (int)HU_OK);
    /* All required pointers NULL but inputs struct itself non-NULL → still rejected. */
    hu_reflection_run_inputs_t empty = {0};
    HU_ASSERT_NEQ((int)hu_reflection_run(&empty, false, NULL, NULL, NULL), (int)HU_OK);
}

void run_reflection_orchestration_tests(void) {
    HU_TEST_SUITE("reflection_orchestration");
    HU_RUN_TEST(test_should_run_disabled_returns_disabled);
    HU_RUN_TEST(test_should_run_force_bypasses_interval_and_idle);
    HU_RUN_TEST(test_should_run_inside_interval_returns_interval);
    HU_RUN_TEST(test_should_run_idle_and_interval_ok_returns_run_idle);
    HU_RUN_TEST(test_should_run_no_prior_run_with_idle_runs);
    HU_RUN_TEST(test_should_run_not_idle_returns_not_idle);
    HU_RUN_TEST(test_should_run_daily_floor_bypasses_idle);
    HU_RUN_TEST(test_run_gated_returns_gated_status_no_writes);
    HU_RUN_TEST(test_run_no_input_returns_no_input_no_row_inserted);
    HU_RUN_TEST(test_run_happy_path_keeps_high_conf_drops_low_conf);
    HU_RUN_TEST(test_run_provider_error_records_provider_error_row);
    HU_RUN_TEST(test_run_schema_invalid_records_schema_invalid_row);
    HU_RUN_TEST(test_run_null_inputs_rejected);
}

#else /* !HU_ENABLE_SQLITE */

void run_reflection_orchestration_tests(void) {
    /* Stub when SQLite is disabled; matches the gate-symmetry pattern
     * used by test_reflection_storage.c / test_reflection_prompt.c. */
}

#endif /* HU_ENABLE_SQLITE */
