/*
 * US-7.7 — Test-time persona scoring (best-of-N at inference) tests.
 *
 * Covers acceptance criteria AC-7.7.1, AC-7.7.2, AC-7.7.4, AC-7.7.5, AC-7.7.6
 * plus defensive coverage (passthrough, mid-loop failure, all-unscored
 * cold-start). The doctor-warning case (AC-7.7.3) lives in
 * tests/test_doctor_best_of_n_warning.c.
 *
 * Frozen API pin (AC-7.7.6): this test calls
 * `hu_communication_style_fidelity_score(target, response, response_len)`
 * with the signature in include/human/memory/personal_model.h:535. The
 * implementer pre-flight runs:
 *
 *   git diff sprint-7-digital-twin-dpo -- include/human/memory/personal_model.h \
 *     | grep "fidelity_score"
 *
 * and expects empty output. If this comment block ever moves: keep the pin.
 *
 * No real llama.cpp linkage required — the tests use a mock provider whose
 * `chat()` returns deterministic fixed strings from a fixture vector.
 */

#include "human/agent/best_of_n.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory/personal_model.h"
#include "human/observability/log_observer.h"
#include "human/observer.h"
#include "human/provider.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Mock provider state (per-test reset) ───────────────────────────── */

#define MOCK_MAX_FIXTURES 8

typedef struct mock_state {
    const char *fixtures[MOCK_MAX_FIXTURES];
    size_t fixture_count;
    size_t call_count;
    /* Optional error to inject on a specific call index (HU_OK == no error). */
    hu_error_t inject_err_at_index;
    int inject_err_index; /* -1 == disabled */
} mock_state_t;

static mock_state_t g_mock;

static void mock_reset(void) {
    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.inject_err_index = -1;
    g_mock.inject_err_at_index = HU_OK;
}

static hu_error_t mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                            const char *model, size_t model_len, double temperature,
                            hu_chat_response_t *out) {
    (void)ctx;
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    if (!out || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    size_t idx = g_mock.call_count;
    g_mock.call_count++;
    if (g_mock.inject_err_index >= 0 && idx == (size_t)g_mock.inject_err_index) {
        return g_mock.inject_err_at_index;
    }
    if (idx >= g_mock.fixture_count)
        return HU_ERR_PROVIDER_RESPONSE;
    const char *fix = g_mock.fixtures[idx];
    size_t n = strlen(fix);
    char *dup = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(dup, fix, n);
    dup[n] = '\0';
    out->content = dup;
    out->content_len = n;
    return HU_OK;
}

static const char *mock_get_name_llamacpp(void *ctx) {
    (void)ctx;
    return "llamacpp";
}

static const char *mock_get_name_openai(void *ctx) {
    (void)ctx;
    return "openai";
}

static bool mock_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static void mock_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

static const hu_provider_vtable_t MOCK_VTABLE_LLAMACPP = {
    .chat_with_system = NULL,
    .chat = mock_chat,
    .supports_native_tools = mock_supports_native_tools,
    .get_name = mock_get_name_llamacpp,
    .deinit = mock_deinit,
    .warmup = NULL,
    .chat_with_tools = NULL,
    .supports_streaming = NULL,
    .supports_vision = NULL,
    .supports_vision_for_model = NULL,
    .stream_chat = NULL,
    .load_adapter = NULL,
    .unload_adapter = NULL,
    .active_adapter = NULL,
};

static const hu_provider_vtable_t MOCK_VTABLE_OPENAI = {
    .chat_with_system = NULL,
    .chat = mock_chat,
    .supports_native_tools = mock_supports_native_tools,
    .get_name = mock_get_name_openai,
    .deinit = mock_deinit,
    .warmup = NULL,
    .chat_with_tools = NULL,
    .supports_streaming = NULL,
    .supports_vision = NULL,
    .supports_vision_for_model = NULL,
    .stream_chat = NULL,
    .load_adapter = NULL,
    .unload_adapter = NULL,
    .active_adapter = NULL,
};

/* ── Deterministic monotonic-clock fixture (AC-7.7.5) ───────────────── */

static uint64_t g_clock_steps_ns[16];
static size_t g_clock_steps_len;
static size_t g_clock_step_idx;

static uint64_t mock_clock_stepper(void) {
    if (g_clock_step_idx >= g_clock_steps_len)
        return g_clock_steps_ns[g_clock_steps_len - 1];
    return g_clock_steps_ns[g_clock_step_idx++];
}

static void clock_reset_to(const uint64_t *steps_ms, size_t n) {
    g_clock_steps_len = n;
    g_clock_step_idx = 0;
    for (size_t i = 0; i < n; ++i)
        g_clock_steps_ns[i] = (uint64_t)steps_ms[i] * 1000000ull;
}

/* ── Fixture builders ───────────────────────────────────────────────── */

/* Target style: lowercase, no abbreviations, length ~12 chars. With
 * sample_count > 0 the scorer is enabled. */
static hu_communication_style_t build_target_style(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    s.formality = 0.3f;
    s.verbosity = 0.4f;
    s.emoji_frequency = 0.0f;
    s.humor_receptivity = 0.5f;
    s.lowercase_ratio = 1.0f;
    s.abbreviation_ratio = 0.0f;
    s.avg_message_length = 12u;
    s.sample_count = 10u;
    s.last_observed_at = 1700000000;
    return s;
}

/* Empty-style fingerprint (cold-start) — fidelity scorer returns -1.0. */
static hu_communication_style_t build_empty_style(void) {
    hu_communication_style_t s;
    memset(&s, 0, sizeof(s));
    return s;
}

static hu_chat_request_t build_minimal_request(void) {
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = NULL;
    req.messages_count = 0;
    req.temperature = 0.7;
    return req;
}

static hu_allocator_t alloc(void) {
    return hu_system_allocator();
}

/* ── Tests ──────────────────────────────────────────────────────────── */

/* AC-7.7.1 — Given best_of_n=4 and the llamacpp provider, the wrapper
 * calls chat() exactly 4 times and returns the highest-fidelity candidate. */
static void test_best_of_4_returns_highest_score(void) {
    mock_reset();
    /* Pick four responses with deterministically different scores against
     * the target style (lowercase, no abbrev, length=12).
     *
     * Indices:
     *   0 = "HI" — uppercase, length 2 → low lowercase + low length
     *   1 = "hi there!!!" — lowercase, length 11 ≈ 12 → highest
     *   2 = "Hello world" — mixed case, length 11 → mid
     *   3 = "lol r u ok" — lowercase, has abbrev (u), length 10 → lower abbrev axis
     *
     * We don't pin literal scores; we pin the relative ranking: index 1
     * must beat the others. The fidelity scorer is deterministic so the
     * comparison is stable across runs. */
    g_mock.fixtures[0] = "HI";
    g_mock.fixtures[1] = "hi there!!!";
    g_mock.fixtures[2] = "Hello world";
    g_mock.fixtures[3] = "lol r u ok";
    g_mock.fixture_count = 4;

    hu_provider_t prov = {.ctx = NULL, .vtable = &MOCK_VTABLE_LLAMACPP};
    hu_communication_style_t style = build_target_style();
    hu_chat_request_t req = build_minimal_request();

    hu_best_of_n_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    hu_best_of_n_config_t cfg = {0};
    cfg.provider = &prov;
    cfg.style = &style;
    cfg.request = &req;
    cfg.model = "m";
    cfg.model_len = 1;
    cfg.temperature = 0.7;
    cfg.n = 4;
    cfg.cost_cap_ms = 0;
    cfg.observer = NULL;
    cfg.stats_out = &stats;

    hu_allocator_t a = alloc();
    hu_chat_response_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_best_of_n_chat(&cfg, &a, &out), HU_OK);

    HU_ASSERT_EQ(g_mock.call_count, 4u);
    HU_ASSERT_EQ(stats.n_requested, 4u);
    HU_ASSERT_EQ(stats.n_completed, 4u);
    HU_ASSERT_EQ(stats.picked_index, 1u);
    HU_ASSERT_NOT_NULL(out.content);
    HU_ASSERT_STR_EQ(out.content, "hi there!!!");
    HU_ASSERT_FALSE(stats.cost_cap_hit);
    HU_ASSERT_FALSE(stats.all_unscored);

    hu_chat_response_free(&a, &out);
}

/* AC-7.7.2 — best_of_n=1 → exactly one call, behavior unchanged. */
static void test_best_of_1_is_single_call(void) {
    mock_reset();
    g_mock.fixtures[0] = "only one";
    g_mock.fixture_count = 1;

    hu_provider_t prov = {.ctx = NULL, .vtable = &MOCK_VTABLE_LLAMACPP};
    hu_communication_style_t style = build_target_style();
    hu_chat_request_t req = build_minimal_request();

    hu_best_of_n_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    hu_best_of_n_config_t cfg = {0};
    cfg.provider = &prov;
    cfg.style = &style;
    cfg.request = &req;
    cfg.model = "m";
    cfg.model_len = 1;
    cfg.temperature = 0.5;
    cfg.n = 1;
    cfg.stats_out = &stats;

    hu_allocator_t a = alloc();
    hu_chat_response_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_best_of_n_chat(&cfg, &a, &out), HU_OK);
    HU_ASSERT_EQ(g_mock.call_count, 1u);
    HU_ASSERT_EQ(stats.n_completed, 1u);
    HU_ASSERT_EQ(stats.picked_index, 0u);
    HU_ASSERT_STR_EQ(out.content, "only one");
    hu_chat_response_free(&a, &out);

    /* Repeat with n=0 — also single-call passthrough. */
    mock_reset();
    g_mock.fixtures[0] = "zero passthrough";
    g_mock.fixture_count = 1;
    cfg.n = 0;
    memset(&out, 0, sizeof(out));
    memset(&stats, 0, sizeof(stats));
    HU_ASSERT_EQ(hu_best_of_n_chat(&cfg, &a, &out), HU_OK);
    HU_ASSERT_EQ(g_mock.call_count, 1u);
    HU_ASSERT_STR_EQ(out.content, "zero passthrough");
    hu_chat_response_free(&a, &out);
}

/* AC-7.7.4 — telemetry log line + stats fields. */
static void test_best_of_n_telemetry_emitted(void) {
    mock_reset();
    g_mock.fixtures[0] = "HI";
    g_mock.fixtures[1] = "hi there!!!";
    g_mock.fixtures[2] = "Hello world";
    g_mock.fixtures[3] = "lol r u ok";
    g_mock.fixture_count = 4;

    hu_allocator_t a = alloc();
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&a, f);

    hu_provider_t prov = {.ctx = NULL, .vtable = &MOCK_VTABLE_LLAMACPP};
    hu_communication_style_t style = build_target_style();
    hu_chat_request_t req = build_minimal_request();

    hu_best_of_n_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    hu_best_of_n_config_t cfg = {0};
    cfg.provider = &prov;
    cfg.style = &style;
    cfg.request = &req;
    cfg.model = "m";
    cfg.model_len = 1;
    cfg.temperature = 0.7;
    cfg.n = 4;
    cfg.observer = &obs;
    cfg.stats_out = &stats;

    hu_chat_response_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_best_of_n_chat(&cfg, &a, &out), HU_OK);
    hu_observer_flush(obs);

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    /* Pick line with aggregate fields (AC-7.7.4). */
    HU_ASSERT_STR_CONTAINS(buf, "best_of_n_pick");
    HU_ASSERT_STR_CONTAINS(buf, "n=4");
    HU_ASSERT_STR_CONTAINS(buf, "picked_score=");
    HU_ASSERT_STR_CONTAINS(buf, "min_score=");
    HU_ASSERT_STR_CONTAINS(buf, "max_score=");
    /* Should NOT contain a cost-cap line in this scenario. */
    HU_ASSERT_STR_NOT_CONTAINS(buf, "best_of_n_cost_cap_hit");

    /* Stats: 4 candidates, picked > index 0 (the highest of the four
     * is the lowercase length-11 entry at index 1). */
    HU_ASSERT_EQ(stats.n_completed, 4u);
    HU_ASSERT_EQ(stats.picked_index, 1u);
    HU_ASSERT_TRUE(stats.picked_score >= stats.min_score);
    HU_ASSERT_TRUE(stats.picked_score <= 1.0f);
    HU_ASSERT_TRUE(stats.max_score >= stats.min_score);

    hu_chat_response_free(&a, &out);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

/* AC-7.7.5 — cost cap returns best-so-far. Uses mock clock seam.
 *
 * Clock steps (ms): t_start=0, t_after_1=50, t_after_2=60, t_after_3=130
 * with cost_cap_ms=100. After call 3 the wrapper sees 130 - 0 = 130ms >=
 * 100ms cap and stops. Call 4 must NOT fire. Returned response = best
 * fidelity over candidates 0..2. */
static void test_cost_cap_returns_best_seen(void) {
    mock_reset();
    g_mock.fixtures[0] = "HI";          /* low score */
    g_mock.fixtures[1] = "hi there!!!"; /* highest score */
    g_mock.fixtures[2] = "Hello world"; /* mid score */
    g_mock.fixtures[3] = "should not be called";
    g_mock.fixture_count = 4;

    /* Clock samples consumed: t_start, t_after_call1, t_after_call2,
     * t_after_call3. After call 3 we trip the cap. */
    uint64_t steps_ms[] = {0, 50, 60, 130};
    clock_reset_to(steps_ms, sizeof(steps_ms) / sizeof(steps_ms[0]));
    hu_best_of_n_set_clock_fn_for_test(mock_clock_stepper);

    hu_allocator_t a = alloc();
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&a, f);

    hu_provider_t prov = {.ctx = NULL, .vtable = &MOCK_VTABLE_LLAMACPP};
    hu_communication_style_t style = build_target_style();
    hu_chat_request_t req = build_minimal_request();

    hu_best_of_n_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    hu_best_of_n_config_t cfg = {0};
    cfg.provider = &prov;
    cfg.style = &style;
    cfg.request = &req;
    cfg.model = "m";
    cfg.model_len = 1;
    cfg.temperature = 0.7;
    cfg.n = 4;
    cfg.cost_cap_ms = 100;
    cfg.observer = &obs;
    cfg.stats_out = &stats;

    hu_chat_response_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_best_of_n_chat(&cfg, &a, &out), HU_OK);
    hu_observer_flush(obs);

    HU_ASSERT_EQ(g_mock.call_count, 3u);
    HU_ASSERT_EQ(stats.n_completed, 3u);
    HU_ASSERT_TRUE(stats.cost_cap_hit);
    HU_ASSERT_EQ(stats.picked_index, 1u);
    HU_ASSERT_STR_EQ(out.content, "hi there!!!");

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    HU_ASSERT_STR_CONTAINS(buf, "best_of_n_cost_cap_hit");
    HU_ASSERT_STR_CONTAINS(buf, "cost_cap_ms=100");
    HU_ASSERT_STR_CONTAINS(buf, "n_completed=3");

    hu_chat_response_free(&a, &out);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);

    /* Restore production clock. */
    hu_best_of_n_set_clock_fn_for_test(NULL);
}

/* AC-7.7.6 — signature pin: call the fidelity scorer with the exact
 * 3-arg signature from the public header. If anyone changes the
 * declaration, this fails to compile. */
static void test_fidelity_signature_unchanged(void) {
    hu_communication_style_t style = build_target_style();
    float s = hu_communication_style_fidelity_score(&style, "abc", 3);
    HU_ASSERT_TRUE(s >= 0.0f && s <= 1.0f);

    hu_communication_style_t empty = build_empty_style();
    float u = hu_communication_style_fidelity_score(&empty, "abc", 3);
    HU_ASSERT_TRUE(u < 0.0f);
}

/* Defensive: every candidate scores -1 (cold-start) → return first, log
 * `best_of_n_unscored_fallback`. */
static void test_all_unscored_returns_first(void) {
    mock_reset();
    g_mock.fixtures[0] = "first candidate";
    g_mock.fixtures[1] = "second candidate";
    g_mock.fixtures[2] = "third candidate";
    g_mock.fixture_count = 3;

    hu_allocator_t a = alloc();
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&a, f);

    hu_provider_t prov = {.ctx = NULL, .vtable = &MOCK_VTABLE_LLAMACPP};
    hu_communication_style_t empty = build_empty_style();
    hu_chat_request_t req = build_minimal_request();

    hu_best_of_n_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    hu_best_of_n_config_t cfg = {0};
    cfg.provider = &prov;
    cfg.style = &empty;
    cfg.request = &req;
    cfg.model = "m";
    cfg.model_len = 1;
    cfg.temperature = 0.7;
    cfg.n = 3;
    cfg.observer = &obs;
    cfg.stats_out = &stats;

    hu_chat_response_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_best_of_n_chat(&cfg, &a, &out), HU_OK);
    hu_observer_flush(obs);

    HU_ASSERT_EQ(g_mock.call_count, 3u);
    HU_ASSERT_EQ(stats.picked_index, 0u);
    HU_ASSERT_TRUE(stats.all_unscored);
    HU_ASSERT_STR_EQ(out.content, "first candidate");

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_STR_CONTAINS(buf, "best_of_n_unscored_fallback");

    hu_chat_response_free(&a, &out);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

/* Defensive: first-call hard error propagates, nothing returned. */
static void test_first_call_error_propagates(void) {
    mock_reset();
    g_mock.fixture_count = 4;
    g_mock.inject_err_index = 0;
    g_mock.inject_err_at_index = HU_ERR_PROVIDER_RESPONSE;

    hu_provider_t prov = {.ctx = NULL, .vtable = &MOCK_VTABLE_LLAMACPP};
    hu_communication_style_t style = build_target_style();
    hu_chat_request_t req = build_minimal_request();

    hu_best_of_n_config_t cfg = {0};
    cfg.provider = &prov;
    cfg.style = &style;
    cfg.request = &req;
    cfg.model = "m";
    cfg.model_len = 1;
    cfg.n = 4;

    hu_allocator_t a = alloc();
    hu_chat_response_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_best_of_n_chat(&cfg, &a, &out), HU_ERR_PROVIDER_RESPONSE);
    HU_ASSERT_EQ(g_mock.call_count, 1u);
    HU_ASSERT_NULL(out.content);
}

/* Defensive: mid-loop error keeps best-so-far, logs partial failure. */
static void test_mid_loop_error_returns_best_so_far(void) {
    mock_reset();
    g_mock.fixtures[0] = "HI";
    g_mock.fixtures[1] = "hi there!!!";
    g_mock.fixtures[2] = "Hello world";
    g_mock.fixtures[3] = "should not be reached";
    g_mock.fixture_count = 4;
    g_mock.inject_err_index = 2;
    g_mock.inject_err_at_index = HU_ERR_PROVIDER_RESPONSE;

    hu_allocator_t a = alloc();
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    hu_observer_t obs = hu_log_observer_create(&a, f);

    hu_provider_t prov = {.ctx = NULL, .vtable = &MOCK_VTABLE_LLAMACPP};
    hu_communication_style_t style = build_target_style();
    hu_chat_request_t req = build_minimal_request();

    hu_best_of_n_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    hu_best_of_n_config_t cfg = {0};
    cfg.provider = &prov;
    cfg.style = &style;
    cfg.request = &req;
    cfg.model = "m";
    cfg.model_len = 1;
    cfg.n = 4;
    cfg.observer = &obs;
    cfg.stats_out = &stats;

    hu_chat_response_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_best_of_n_chat(&cfg, &a, &out), HU_OK);
    hu_observer_flush(obs);

    /* Call 3 (index 2) errors; calls 1 and 2 completed; call 4 never fires. */
    HU_ASSERT_EQ(g_mock.call_count, 3u);
    HU_ASSERT_EQ(stats.n_completed, 2u);
    /* Best of {index0, index1} is index 1. */
    HU_ASSERT_EQ(stats.picked_index, 1u);
    HU_ASSERT_STR_EQ(out.content, "hi there!!!");

    char buf[4096];
    memset(buf, 0, sizeof(buf));
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    HU_ASSERT_STR_CONTAINS(buf, "best_of_n_partial_failure");

    hu_chat_response_free(&a, &out);
    if (obs.vtable && obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

/* Defensive: provider with non-llamacpp name still works through the
 * decorator (the eligibility check lives at the agent_turn site, not in
 * the decorator). When invoked directly, the decorator just does its job
 * for whatever provider it's handed — making it usable for future
 * non-llamacpp local providers (Bridge B.1) without code changes. */
static void test_decorator_works_against_any_provider(void) {
    mock_reset();
    g_mock.fixtures[0] = "HI";
    g_mock.fixtures[1] = "hi there!!!";
    g_mock.fixture_count = 2;

    hu_provider_t prov = {.ctx = NULL, .vtable = &MOCK_VTABLE_OPENAI};
    hu_communication_style_t style = build_target_style();
    hu_chat_request_t req = build_minimal_request();

    hu_best_of_n_config_t cfg = {0};
    cfg.provider = &prov;
    cfg.style = &style;
    cfg.request = &req;
    cfg.model = "m";
    cfg.model_len = 1;
    cfg.n = 2;

    hu_allocator_t a = alloc();
    hu_chat_response_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(hu_best_of_n_chat(&cfg, &a, &out), HU_OK);
    HU_ASSERT_EQ(g_mock.call_count, 2u);
    HU_ASSERT_STR_EQ(out.content, "hi there!!!");
    hu_chat_response_free(&a, &out);
}

void run_llamacpp_best_of_n_tests(void) {
    HU_TEST_SUITE("BestOfN (US-7.7 test-time persona scoring)");
    HU_RUN_TEST(test_best_of_4_returns_highest_score);
    HU_RUN_TEST(test_best_of_1_is_single_call);
    HU_RUN_TEST(test_best_of_n_telemetry_emitted);
    HU_RUN_TEST(test_cost_cap_returns_best_seen);
    HU_RUN_TEST(test_fidelity_signature_unchanged);
    HU_RUN_TEST(test_all_unscored_returns_first);
    HU_RUN_TEST(test_first_call_error_propagates);
    HU_RUN_TEST(test_mid_loop_error_returns_best_so_far);
    HU_RUN_TEST(test_decorator_works_against_any_provider);
}
