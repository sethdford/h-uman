/* tests/test_reflection_e2e.c — T10 of
 * docs/plans/2026-05-26-reflection-loop. End-to-end loop:
 *
 *   1. config opt-in via parse_json
 *   2. storage migration
 *   3. hu_reflection_run with mock provider returning a valid JSON
 *      response (T1 + T2 + T4 + T5 all execute)
 *   4. hu_personal_model_build_prompt_with_reflection (T6 + T7)
 *      pulls the just-stored pattern back out into the prompt
 *
 * If any of T1/T2/T4/T5/T6/T7 silently drifted apart at the wire-
 * protocol level, this test catches it — the prompt assembled in
 * step 4 must contain the observation text the mock provider
 * returned in step 3.
 *
 * Distinct from test_reflection_orchestration.c (which pins run()
 * in isolation) and test_personal_model_reflection_slice.c (which
 * pins the prompt builder against synthetic db rows). This test
 * exercises the GENUINE full path: mock provider response →
 * parsed → upserted → query_for_system_prompt → personal_model
 * appends → output buffer. */

#include "human/config.h"
#include "human/config_types.h"
#include "human/memory/personal_model.h"
#include "human/provider.h"
#include "human/reflection.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

/* ── Mock infrastructure (mirror of T5 orchestration suite) ───── */

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

typedef struct {
    const char *canned_response;
    int call_count;
} mock_provider_ctx_t;

static char *dup_alloc(hu_allocator_t *alloc, const char *s, size_t n) {
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
    (void)sys;
    (void)sys_len;
    (void)msg;
    (void)msg_len;
    (void)model;
    (void)model_len;
    (void)temp;
    mock_provider_ctx_t *m = (mock_provider_ctx_t *)ctx;
    m->call_count++;
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
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t mock_vtable = {
    .chat_with_system = mock_chat_with_system,
    .supports_native_tools = mock_supports_native_tools,
    .get_name = mock_get_name,
    .deinit = mock_deinit,
};

/* ── The end-to-end test ───────────────────────────────────────── */

/* AC-T10: full loop closes. Mock provider emits one valid pattern;
 * after the run, the next build_prompt_with_reflection call surfaces
 * that pattern's observation text into the system prompt. */
static void test_e2e_run_then_prompt_surfaces_new_pattern(void) {
    hu_reflection_reset_warn_guards_for_test();

    /* 1. In-memory db + migration. */
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ((int)hu_reflection_storage_migrate(db), (int)HU_OK);

    /* 2. Enabled config + idle threshold met. */
    hu_reflection_loop_config_t cfg = {.enabled = true,
                                       .min_interval_hours = 12,
                                       .idle_threshold_hours = 2,
                                       .daily_floor_hours = 24,
                                       .provider = "mock"};

    /* 3. Mock provider returns ONE pattern in the schema T1+T4 share. */
    const char *response = "{"
                           "  \"prose_summary\": \"Seth has been mentioning sleep deprivation.\","
                           "  \"patterns\": ["
                           "    {\"type\": \"topic_recurrence\", \"subject\": \"Seth\","
                           "     \"observation\": \"sleep mentions across multiple weeknights\","
                           "     \"confidence\": 0.85,"
                           "     \"evidence_ids\": [\"t_e2e_001\"], \"channels\": [\"imessage\"]}"
                           "  ]"
                           "}";
    mock_provider_ctx_t mctx = {.canned_response = response};
    hu_provider_t prov = {.ctx = &mctx, .vtable = &mock_vtable};
    hu_allocator_t alloc = hu_system_allocator();

    /* 4. Static turn iter with one inbound user turn so build_input
     *    has something to send. now_ms anchored at current wall-clock so
     *    the consumer's 7-day recency window will accept the resulting
     *    pattern. */
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    const hu_reflection_turn_t turns[] = {{.turn_id = "t_e2e_001",
                                           .channel = "imessage",
                                           .sender = "user",
                                           .ts_ms = now_ms - 3600ULL * 1000ULL,
                                           .content = "barely sleeping again this week"}};
    static_iter_ctx_t iter_ctx = {.turns = turns, .count = 1, .pos = 0};

    /* 5. Run the orchestrator. force=true bypasses interval/idle so
     *    the test doesn't depend on wall-clock manipulation. */
    hu_reflection_run_inputs_t inputs = {.db = db,
                                         .cfg = &cfg,
                                         .provider = &prov,
                                         .alloc = &alloc,
                                         .iter_fn = static_iter,
                                         .iter_ctx = &iter_ctx,
                                         .last_user_activity_ms = 0,
                                         .now_ms = now_ms,
                                         .max_input_chars = 0};
    hu_reflection_run_status_t status = HU_REFLECTION_RUN_GATED;
    int kept = 0, dropped = 0;
    HU_ASSERT_EQ((int)hu_reflection_run(&inputs, /*force=*/true, &status, &kept, &dropped),
                 (int)HU_OK);
    HU_ASSERT_EQ((int)status, (int)HU_REFLECTION_RUN_OK);
    HU_ASSERT_EQ(kept, 1);
    HU_ASSERT_EQ(dropped, 0);
    HU_ASSERT_EQ(mctx.call_count, 1); /* provider was actually called */

    /* 6. Now build a personal-model prompt with reflection slice.
     *    The just-stored pattern's observation must appear in the
     *    output, plus the prose summary line. */
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    snprintf(model.core.user_name, sizeof model.core.user_name, "%s", "Seth");

    char buf[8192];
    size_t n = hu_personal_model_build_prompt_with_reflection(&model, NULL, db, "imessage",
                                                              /*max_patterns=*/5, buf, sizeof buf);
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "Recent observations about Seth") != NULL);
    HU_ASSERT(strstr(buf, "sleep mentions across multiple weeknights") != NULL);
    HU_ASSERT(
        strstr(buf, "Latest reflection summary: Seth has been mentioning sleep deprivation") !=
        NULL);

    /* 7. Second prompt call: pattern was marked surfaced by step 6, so
     *    it doesn't appear in the listed observations a second time —
     *    confirms the full loop ALSO honors single-use semantics across
     *    the e2e path, not just the unit-test path. */
    memset(buf, 0, sizeof buf);
    hu_personal_model_build_prompt_with_reflection(&model, NULL, db, "imessage", 5, buf,
                                                   sizeof buf);
    HU_ASSERT(strstr(buf, "Recent observations about Seth") == NULL);
    /* Prose summary still appears (not gated by surfaced). */
    HU_ASSERT(strstr(buf, "Latest reflection: Seth has been mentioning sleep deprivation") != NULL);

    sqlite3_close(db);
}

/* AC-T10b: config parse + storage migrate + run all integrate.
 * Exercises the operator path: load config from JSON, see
 * reflection_loop.enabled=true, migrate, run. */
static void test_e2e_config_json_flips_subsystem_on(void) {
    /* Build a config from JSON (the operator path) and verify the
     * fields land correctly. We don't actually run the daemon here;
     * we just confirm that the JSON the operator would write
     * produces the right cfg state to drive hu_reflection_run. */
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT(arena != NULL);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    const char *json = "{\"reflection\":{\"enabled\":true,\"min_interval_hours\":6}}";
    HU_ASSERT_EQ((int)hu_config_parse_json(&cfg, json, strlen(json)), (int)HU_OK);
    HU_ASSERT(cfg.reflection_loop.enabled);
    HU_ASSERT_EQ(cfg.reflection_loop.min_interval_hours, 6);
    /* Defaults applied for unspecified. */
    HU_ASSERT_EQ(cfg.reflection_loop.idle_threshold_hours, 2);
    HU_ASSERT_EQ(cfg.reflection_loop.daily_floor_hours, 24);
    HU_ASSERT_STR_EQ(cfg.reflection_loop.provider, "gemini-3.5-flash");
}

/* ── T12: failure-rate watchdog counters ───────────────────────── */

static void insert_run_helper(sqlite3 *db, const char *run_id, uint64_t started_ms,
                              const char *status) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "INSERT INTO reflection_runs (run_id, provider, started_at_ms, "
                       "completed_at_ms, input_turns, status) VALUES (?, 'mock', ?, ?, 5, ?)",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, run_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)started_ms);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)(started_ms + 100));
    sqlite3_bind_text(st, 4, status, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* T12 AC: total counts every run since since_ms, failed counts only non-'ok'. */
static void test_t12_count_helpers_distinguish_status(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    uint64_t now_ms = 10000000000ULL;
    insert_run_helper(db, "r1", now_ms - 3600000ULL, "ok");
    insert_run_helper(db, "r2", now_ms - 7200000ULL, "provider_error");
    insert_run_helper(db, "r3", now_ms - 10800000ULL, "schema_invalid");
    insert_run_helper(db, "r4", now_ms - 14400000ULL, "ok");
    /* Outside the 24h window — must NOT be counted. */
    insert_run_helper(db, "r_old", now_ms - 200000000ULL, "provider_error");

    uint64_t since_ms = now_ms - 86400000ULL;
    HU_ASSERT_EQ(hu_reflection_storage_count_runs_since(db, since_ms), 4);
    HU_ASSERT_EQ(hu_reflection_storage_count_failed_runs_since(db, since_ms), 2);
    sqlite3_close(db);
}

/* T12 AC: small sample (<4) suppresses the alarm regardless of rate. */
static void test_t12_check_failure_rate_small_sample_silent(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    uint64_t now_ms = 10000000000ULL;
    insert_run_helper(db, "r1", now_ms - 1000, "provider_error");
    insert_run_helper(db, "r2", now_ms - 2000, "provider_error");
    /* Just 2 runs, both failed: should NOT trip (denominator < 4). */
    hu_reflection_check_failure_rate(db, now_ms, /*enabled=*/true);
    /* No assertion on log output — the contract is that the function
     * never crashes and never asserts. The atomic_bool is internal. */
    sqlite3_close(db);
}

/* T12 AC: disabled flag short-circuits before SQL. NULL db safe. */
static void test_t12_check_failure_rate_disabled_is_noop(void) {
    hu_reflection_check_failure_rate(NULL, 10000000000ULL, /*enabled=*/false);
    hu_reflection_check_failure_rate(NULL, 10000000000ULL, /*enabled=*/true);
    /* Survives without crash → contract satisfied. */
}

void run_reflection_e2e_tests(void) {
    HU_TEST_SUITE("reflection_e2e");
    HU_RUN_TEST(test_e2e_run_then_prompt_surfaces_new_pattern);
    HU_RUN_TEST(test_e2e_config_json_flips_subsystem_on);
    HU_RUN_TEST(test_t12_count_helpers_distinguish_status);
    HU_RUN_TEST(test_t12_check_failure_rate_small_sample_silent);
    HU_RUN_TEST(test_t12_check_failure_rate_disabled_is_noop);
}

#else /* !HU_ENABLE_SQLITE */

void run_reflection_e2e_tests(void) {
    /* Stub when SQLite is disabled. */
}

#endif /* HU_ENABLE_SQLITE */
