/* tests/test_agent_llm_latency_wall_clock.c
 *
 * Pins the measurement contract behind M3's `latency_ms`:
 *
 *   A provider call that BLOCKS for >= N milliseconds must record
 *   latency_ms >= N.
 *
 * The bug this test was written against (2026-07-27, CI run 30232892253
 * job 89874953923, "M3 Closed-Loop Smoke / Full live-fire"):
 *   agent_turn.c and agent_stream.c timed the provider call with
 *   `clock()`, which returns PROCESS CPU TIME, not wall clock. A
 *   blocking HTTP round trip parks the process in poll()/recv() burning
 *   ~zero CPU, so scripts/stub_mlx_server.py's deliberate 150ms sleep
 *   was recorded as 27ms — the CPU actually spent serializing a
 *   2081-token prompt and parsing the reply.
 *
 *   Downstream, scripts/m3_outcome_driver.py drops any outcome with
 *   latency < MIN_LATENCY_MS (50) as "cached/stub path; not
 *   representative". Fed CPU time instead of wall time, that filter
 *   discarded EVERY outcome — in production as well as in CI, since a
 *   3-second cloud call also burns only ~30ms of local CPU.
 *
 * Test discipline (per .claude/rules/tests-that-pin-bugs.md):
 *   The mock provider below burns NO CPU — it only nanosleep()s. That
 *   is what makes the assertion non-vacuous: under CPU-clock timing the
 *   recorded latency is ~0ms and these tests FAIL; only true wall-clock
 *   timing passes. `latency_ms >= 0` would be a tautology and is
 *   explicitly banned.
 *
 * Production-symbol coverage (per
 * .claude/rules/test-references-production-symbol.md):
 *   Drives the real hu_agent_turn() and asserts on the two surfaces fed
 *   by the same `llm_duration_ms` local that is handed to
 *   hu_agent_m3_record_chat_outcome(): the HU_OBSERVER_EVENT_LLM_RESPONSE
 *   event and the self-model behavior log written by
 *   hu_agent_m3_on_provider_success().
 */

#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/observer.h"
#include "human/provider.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The mock blocks this long; assertions use a floor below it to absorb
 * millisecond truncation, and a ceiling so the test can't pass by
 * accidentally measuring the whole turn. The broken (CPU-clock)
 * implementation produced ~0-30ms, so the floor separates cleanly. */
#define LAT_SLEEP_MS   150u
#define LAT_FLOOR_MS   120u
#define LAT_CEILING_MS 5000u

/* ============================================================================
 * Structural guard — covers BOTH provider call sites.
 *
 * The behavioral test below drives the batch path (hu_agent_turn). The
 * streaming path (agent_stream.c) has the identical timing site and the
 * identical bug, but standing up a stream_chat vtable + callback plumbing
 * to assert one scalar is disproportionate. This grep-level invariant
 * pins that neither path regresses to CPU-clock timing.
 * ============================================================================
 */

static char *lat_slurp(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    return buf;
}

/* Different build trees launch tests from different cwds (build/ vs repo
 * root vs worktrees). Mirrors find_source_with_needle in
 * tests/test_m3_route_per_turn_call_sites.c. */
static char *lat_read_agent_source(const char *rel) {
    char path[512];
    const char *prefixes[] = {"", "../", "../../"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        snprintf(path, sizeof(path), "%s%s", prefixes[i], rel);
        char *buf = lat_slurp(path);
        if (buf)
            return buf;
    }
    return NULL;
}

/* Positive contract: the provider call is timed with the monotonic
 * wall-clock helper. Negative contract: it is NOT timed with clock().
 * The negative half is what fails before the fix. */
static void lat_assert_source_times_provider_with_wall_clock(const char *rel) {
    char *src = lat_read_agent_source(rel);
    HU_ASSERT_NOT_NULL(src);
    HU_ASSERT_NOT_NULL(strstr(src, "llm_start_ms = hu_agent_internal_monotonic_ms()"));
    /* `clock()` is CPU time — it cannot see a blocking round trip. */
    HU_ASSERT_TRUE(strstr(src, "clock_t llm_start = clock()") == NULL);
    free(src);
}

static void agent_turn_times_provider_with_wall_clock(void) {
    lat_assert_source_times_provider_with_wall_clock("src/agent/agent_turn.c");
}

static void agent_stream_times_provider_with_wall_clock(void) {
    lat_assert_source_times_provider_with_wall_clock("src/agent/agent_stream.c");
}

/* ============================================================================
 * Behavioral test — drive the real agent against a provider that blocks.
 * Guarded by HU_ENABLE_SQLITE because the agent fixture needs the memory
 * graph backend (mirrors tests/test_agent_turn_transport.c).
 * ============================================================================
 */
#ifdef HU_ENABLE_SQLITE
#include "human/agent/world_model_bridge.h"
#include "human/memory/graph.h"

/* Blocking mock provider. nanosleep() burns NO CPU — exactly the shape of
 * a real HTTP round trip, and exactly what CPU-clock timing cannot see. */
static hu_error_t lat_mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                                const char *model, size_t model_len, double temperature,
                                hu_chat_response_t *out) {
    (void)req;
    (void)model;
    (void)model_len;
    (void)temperature;
    unsigned *calls = (unsigned *)ctx;
    (*calls)++;
    memset(out, 0, sizeof(*out));

    struct timespec nap = {.tv_sec = 0, .tv_nsec = (long)LAT_SLEEP_MS * 1000000L};
    nanosleep(&nap, NULL);

    const char *body = "ok slow reply";
    size_t n = strlen(body);
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, body, n + 1);
    out->content = buf;
    out->content_len = n;
    return HU_OK;
}

static const char *lat_mock_get_name(void *ctx) {
    (void)ctx;
    return "lat_mock";
}

static hu_provider_vtable_t lat_mock_vtable = {
    .chat = lat_mock_chat,
    .get_name = lat_mock_get_name,
};

/* Capture the duration reported on HU_OBSERVER_EVENT_LLM_RESPONSE — the
 * same `llm_duration_ms` local that agent_turn hands to
 * hu_agent_m3_record_chat_outcome a few lines later. */
typedef struct lat_obs_state {
    uint64_t llm_duration_ms;
    unsigned llm_response_events;
} lat_obs_state_t;

static void lat_obs_record_event(void *ctx, const hu_observer_event_t *event) {
    lat_obs_state_t *st = (lat_obs_state_t *)ctx;
    if (!st || !event || event->tag != HU_OBSERVER_EVENT_LLM_RESPONSE)
        return;
    st->llm_response_events++;
    st->llm_duration_ms = event->data.llm_response.duration_ms;
}

static const char *lat_obs_name(void *ctx) {
    (void)ctx;
    return "lat_obs";
}

static hu_observer_vtable_t lat_obs_vtable = {
    .record_event = lat_obs_record_event,
    .name = lat_obs_name,
};

typedef struct lat_fixture {
    hu_allocator_t alloc;
    hu_provider_t prov;
    hu_graph_t *g;
    hu_w7_facade_t *wf;
    hu_agent_t agent;
    unsigned calls;
    lat_obs_state_t obs_state;
    hu_observer_t obs;
} lat_fixture_t;

static void lat_open(lat_fixture_t *f) {
    memset(f, 0, sizeof(*f));
    f->alloc = hu_system_allocator();
    f->prov.ctx = &f->calls;
    f->prov.vtable = &lat_mock_vtable;
    HU_ASSERT_EQ(hu_graph_open(&f->alloc, NULL, 0, &f->g), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(f->g, &f->alloc, &f->wf), HU_OK);
    HU_ASSERT_EQ(hu_agent_from_config(&f->agent, &f->alloc, f->prov, NULL, 0, NULL, NULL, NULL,
                                      NULL, "lat-mock-model", 14, "lat_mock", 8, 0.7, ".", 1,
                                      /*max_tool_iterations=*/5, 50, false, 0, NULL, 0, NULL, 0,
                                      NULL),
                 HU_OK);
    f->agent.verifier_graph = f->g;
    f->agent.w7_facade = f->wf;
    f->agent.memory_session_id = "lat-session";
    f->agent.memory_session_id_len = 11;
    f->agent.active_channel = "imessage";
    f->agent.active_channel_len = 8;
    f->obs.ctx = &f->obs_state;
    f->obs.vtable = &lat_obs_vtable;
    f->agent.observer = &f->obs;
}

static void lat_close(lat_fixture_t *f) {
    hu_agent_deinit(&f->agent);
    hu_graph_close(f->g, &f->alloc);
}

/* THE contract. A provider that blocks LAT_SLEEP_MS must be recorded as
 * having taken at least LAT_FLOOR_MS. Under the pre-fix CPU-clock timing
 * this records ~0ms and the assertion fails. */
static void provider_latency_covers_blocking_round_trip(void) {
    lat_fixture_t f;
    lat_open(&f);

    char *resp = NULL;
    size_t resp_len = 0;
    hu_error_t err = hu_agent_turn(&f.agent, "hi", 2, &resp, &resp_len);

    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(f.calls, 1u);
    HU_ASSERT_EQ(f.obs_state.llm_response_events, 1u);

    /* Wall clock, not CPU clock: the blocked 150ms must be visible. */
    HU_ASSERT_TRUE(f.obs_state.llm_duration_ms >= (uint64_t)LAT_FLOOR_MS);
    /* Sanity ceiling — we're timing the provider call, not the whole turn. */
    HU_ASSERT_TRUE(f.obs_state.llm_duration_ms <= (uint64_t)LAT_CEILING_MS);

    if (resp)
        f.alloc.free(f.alloc.ctx, resp, resp_len + 1);
    lat_close(&f);
}

#ifdef HU_ENABLE_SELF_MODEL
/* The self-model behavior log is written by hu_agent_m3_on_provider_success
 * from the same stashed `llm_duration_ms`. Asserting here proves the value
 * that reaches the M3 write path — not merely the observability event — is
 * wall-clock. */
static void behavior_log_latency_covers_blocking_round_trip(void) {
    lat_fixture_t f;
    lat_open(&f);

    char *resp = NULL;
    size_t resp_len = 0;
    HU_ASSERT_EQ((int)hu_agent_turn(&f.agent, "hi", 2, &resp, &resp_len), (int)HU_OK);

    hu_agent_behavior_record_t rec[4];
    size_t rec_count = 0;
    HU_ASSERT_EQ(hu_agent_behavior_log_snapshot(&f.agent.behavior_log, rec, 4, &rec_count), HU_OK);
    HU_ASSERT_TRUE(rec_count >= 1);

    /* Most recent record last (chronological order, per AC-SM-1). */
    const hu_agent_behavior_record_t *last = &rec[rec_count - 1];
    HU_ASSERT_TRUE(last->response_latency_ms >= (uint32_t)LAT_FLOOR_MS);
    HU_ASSERT_TRUE(last->response_latency_ms <= (uint32_t)LAT_CEILING_MS);

    if (resp)
        f.alloc.free(f.alloc.ctx, resp, resp_len + 1);
    lat_close(&f);
}
#endif /* HU_ENABLE_SELF_MODEL */
#endif /* HU_ENABLE_SQLITE */

void run_agent_llm_latency_wall_clock_tests(void);
void run_agent_llm_latency_wall_clock_tests(void) {
    HU_TEST_SUITE("agent llm latency wall clock");
    HU_RUN_TEST(agent_turn_times_provider_with_wall_clock);
    HU_RUN_TEST(agent_stream_times_provider_with_wall_clock);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(provider_latency_covers_blocking_round_trip);
#ifdef HU_ENABLE_SELF_MODEL
    HU_RUN_TEST(behavior_log_latency_covers_blocking_round_trip);
#endif
#endif
}
