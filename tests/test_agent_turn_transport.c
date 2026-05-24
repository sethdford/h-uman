/* tests/test_agent_turn_transport.c — Transport-error fast-fail in agent_turn.
 *
 * The M4 verifier on 2026-05-24 observed gateway requests timing out at 90s
 * with no MLX log entry. Root cause: agent_turn's tool-loop iterates
 * max_tool_iterations times when the underlying provider transport is
 * unreachable (connection refused → HU_ERR_IO), each iteration burning a
 * fresh round-trip and a fresh persona-prompt build. The fix added a
 * pure predicate (hu_agent_internal_is_transport_error) plus a fallback
 * builder (hu_agent_internal_build_unavailable_fallback) and wired them
 * into the tool-loop so the second consecutive transport error bails out
 * with HU_ERR_PROVIDER_UNAVAILABLE.
 *
 * These tests pin three contracts:
 *   1. Predicate: HU_ERR_IO and HU_ERR_TIMEOUT classify as transport
 *      errors; HU_ERR_PROVIDER_RESPONSE / HU_OK / HU_ERR_INVALID_ARGUMENT
 *      do not (would falsely bail out otherwise).
 *   2. Fallback builder: returns a canonical short string the gateway
 *      can show or suppress, NULL-safe on bad args.
 *   3. Wire: agent_turn bails after exactly 2 consecutive transport
 *      errors (1 call + 1 retry) and recovers when call 2 succeeds.
 *
 * Companion fix to commit 220db26d (JSON double-escape) and 9cc8aac6
 * (MLX launchd lifecycle) — together they close the M4 audit chain.
 */
#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "test_framework.h"
#include <stdint.h>
#include <string.h>

/* Forward-declare the internal helpers (tests/ isn't on src/agent's include path). */
bool hu_agent_internal_is_transport_error(hu_error_t err);
hu_error_t hu_agent_internal_build_unavailable_fallback(hu_allocator_t *alloc, char **out,
                                                        size_t *out_len);

/* ============================================================================
 * Pure-predicate tests — no agent setup, fast, exhaustive.
 * ============================================================================ */

static void is_transport_error_returns_true_for_io(void) {
    HU_ASSERT(hu_agent_internal_is_transport_error(HU_ERR_IO));
}

static void is_transport_error_returns_true_for_timeout(void) {
    HU_ASSERT(hu_agent_internal_is_transport_error(HU_ERR_TIMEOUT));
}

/* HU_ERR_PROVIDER_UNAVAILABLE is the canonical "transport failed" code
 * produced by the degradation layer (src/agent/degradation.c) when all
 * its retry attempts hit transport errors. The classifier MUST include
 * this case — without it, the agent_turn fast-fail never fires in
 * production because hu_agent_from_config enables degradation by
 * default (src/agent/agent.c:820). */
static void is_transport_error_returns_true_for_provider_unavailable(void) {
    HU_ASSERT(hu_agent_internal_is_transport_error(HU_ERR_PROVIDER_UNAVAILABLE));
}

/* Provider-response errors are APPLICATION-level (the provider responded,
 * the response was bad). They must NOT classify as transport — otherwise
 * the fast-fail would prematurely bail on recoverable cases like a
 * temporarily-malformed JSON response that a retry might fix. */
static void is_transport_error_returns_false_for_provider_response(void) {
    HU_ASSERT_TRUE(!hu_agent_internal_is_transport_error(HU_ERR_PROVIDER_RESPONSE));
}

static void is_transport_error_returns_false_for_provider_auth(void) {
    /* Auth errors are 401/403 from the provider — the round-trip
     * completed, the response said "no." Not a transport failure. */
    HU_ASSERT_TRUE(!hu_agent_internal_is_transport_error(HU_ERR_PROVIDER_AUTH));
}

static void is_transport_error_returns_false_for_ok(void) {
    HU_ASSERT_TRUE(!hu_agent_internal_is_transport_error(HU_OK));
}

static void is_transport_error_returns_false_for_invalid_argument(void) {
    /* Caller-side error, not a transport issue. */
    HU_ASSERT_TRUE(!hu_agent_internal_is_transport_error(HU_ERR_INVALID_ARGUMENT));
}

static void is_transport_error_returns_false_for_oom(void) {
    HU_ASSERT_TRUE(!hu_agent_internal_is_transport_error(HU_ERR_OUT_OF_MEMORY));
}

/* ============================================================================
 * Fallback-builder tests — pin the wording + memory contract.
 * ============================================================================ */

static void build_unavailable_fallback_returns_canonical_text(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ((int)hu_agent_internal_build_unavailable_fallback(&alloc, &out, &out_len),
                 (int)HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT(out_len > 0);
    /* Pin the exact wording. If a future change wants to alter it, this
     * test forces an explicit decision (rather than silent drift). */
    HU_ASSERT_STR_EQ(out, "having trouble connecting, try again in a moment");
    HU_ASSERT_EQ(out_len, strlen("having trouble connecting, try again in a moment"));
    /* The buffer is heap-owned; free per the documented contract. */
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void build_unavailable_fallback_null_alloc_returns_invalid(void) {
    char *out = NULL;
    size_t out_len = 99; /* sentinel to verify we don't touch on err */
    HU_ASSERT_EQ((int)hu_agent_internal_build_unavailable_fallback(NULL, &out, &out_len),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

static void build_unavailable_fallback_null_out_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 0;
    HU_ASSERT_EQ((int)hu_agent_internal_build_unavailable_fallback(&alloc, NULL, &out_len),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

static void build_unavailable_fallback_null_out_len_returns_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    HU_ASSERT_EQ((int)hu_agent_internal_build_unavailable_fallback(&alloc, &out, NULL),
                 (int)HU_ERR_INVALID_ARGUMENT);
}

/* ============================================================================
 * Integration tests — drive agent_turn through a scripted mock provider.
 * Guarded by HU_ENABLE_SQLITE because the b9 fixture pattern requires the
 * memory graph backend.
 * ============================================================================ */
#ifdef HU_ENABLE_SQLITE
#include "human/agent/world_model_bridge.h"
#include "human/memory/graph.h"

/* Scripted mock provider: each call returns the next error code in `errs`,
 * advancing `calls_made`. When the script returns HU_OK, we populate a
 * canned response so the agent loop has something to process. */
typedef struct ttf_mock_state {
    hu_error_t errs[8];
    size_t script_count;
    size_t calls_made;
} ttf_mock_state_t;

static hu_error_t ttf_mock_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *req,
                                const char *model, size_t model_len, double temperature,
                                hu_chat_response_t *out) {
    (void)req;
    (void)model;
    (void)model_len;
    (void)temperature;
    ttf_mock_state_t *st = (ttf_mock_state_t *)ctx;
    memset(out, 0, sizeof(*out));
    if (st->calls_made >= st->script_count) {
        /* Off-script: should never happen in a well-bounded test. */
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_error_t err = st->errs[st->calls_made];
    st->calls_made++;
    if (err == HU_OK) {
        const char *body = "ok mock reply";
        size_t n = strlen(body);
        char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
        if (!buf)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(buf, body, n + 1);
        out->content = buf;
        out->content_len = n;
    }
    return err;
}

static const char *ttf_mock_get_name(void *ctx) {
    (void)ctx;
    return "ttf_mock";
}

static hu_provider_vtable_t ttf_mock_vtable = {
    .chat = ttf_mock_chat,
    .get_name = ttf_mock_get_name,
};

typedef struct ttf_fixture {
    hu_allocator_t alloc;
    hu_provider_t prov;
    hu_graph_t *g;
    hu_w7_facade_t *wf;
    hu_agent_t agent;
    ttf_mock_state_t mock;
} ttf_fixture_t;

static void ttf_open(ttf_fixture_t *f) {
    memset(f, 0, sizeof(*f));
    f->alloc = hu_system_allocator();
    f->prov.ctx = &f->mock;
    f->prov.vtable = &ttf_mock_vtable;
    HU_ASSERT_EQ(hu_graph_open(&f->alloc, NULL, 0, &f->g), HU_OK);
    HU_ASSERT_EQ(hu_w7_facade_open(f->g, &f->alloc, &f->wf), HU_OK);
    HU_ASSERT_EQ(hu_agent_from_config(&f->agent, &f->alloc, f->prov, NULL, 0, NULL, NULL, NULL,
                                      NULL, "ttf-mock-model", 14, "ttf_mock", 8, 0.7, ".", 1,
                                      /*max_tool_iterations=*/5, 50, false, 0, NULL, 0, NULL, 0,
                                      NULL),
                 HU_OK);
    f->agent.verifier_graph = f->g;
    f->agent.w7_facade = f->wf;
    f->agent.memory_session_id = "ttf-session";
    f->agent.memory_session_id_len = 11;
    f->agent.active_channel = "imessage";
    f->agent.active_channel_len = 8;
}

static void ttf_close(ttf_fixture_t *f) {
    hu_agent_deinit(&f->agent);
    hu_graph_close(f->g, &f->alloc);
}

/* Spec: two consecutive transport errors must bail after exactly 2 chat calls
 * (1 call + 1 retry), NOT after max_tool_iterations (which would burn 5x
 * the latency). agent_turn must return HU_ERR_PROVIDER_UNAVAILABLE so the
 * gateway can map it to HTTP 503. */
static void agent_turn_transport_error_bails_after_one_retry(void) {
    ttf_fixture_t f;
    ttf_open(&f);
    f.mock.errs[0] = HU_ERR_IO;
    f.mock.errs[1] = HU_ERR_IO;
    /* Pre-fill the rest of the script with INVALID_ARGUMENT — if the loop
     * ever calls past iteration 2, the next call returns an off-script err
     * and the test will see calls_made > 2 (= regression). */
    for (size_t i = 2; i < 8; i++)
        f.mock.errs[i] = HU_ERR_INVALID_ARGUMENT;
    f.mock.script_count = 8;

    char *resp = NULL;
    size_t resp_len = 0;
    hu_error_t err = hu_agent_turn(&f.agent, "hi", 2, &resp, &resp_len);

    /* Bail return code distinguishes "the agent could not run" from
     * "the network is down". The gateway maps this to HTTP 503. */
    HU_ASSERT_EQ((int)err, (int)HU_ERR_PROVIDER_UNAVAILABLE);
    /* Exactly 2 chat calls (= 1 retry), not max_tool_iterations (= 5). */
    HU_ASSERT_EQ(f.mock.calls_made, 2u);

    if (resp)
        f.alloc.free(f.alloc.ctx, resp, resp_len + 1);
    ttf_close(&f);
}

/* Spec: a single transient transport error must NOT bail — the retry on
 * call 2 should succeed and the agent should produce a normal response.
 * Without this contract, transient network blips would degrade every turn. */
static void agent_turn_transport_error_recovers_on_second_call(void) {
    ttf_fixture_t f;
    ttf_open(&f);
    f.mock.errs[0] = HU_ERR_IO; /* transient blip */
    f.mock.errs[1] = HU_OK;     /* recovered */
    f.mock.script_count = 2;

    char *resp = NULL;
    size_t resp_len = 0;
    hu_error_t err = hu_agent_turn(&f.agent, "hi", 2, &resp, &resp_len);

    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(f.mock.calls_made, 2u);
    HU_ASSERT_NOT_NULL(resp);

    if (resp)
        f.alloc.free(f.alloc.ctx, resp, resp_len + 1);
    ttf_close(&f);
}
#endif /* HU_ENABLE_SQLITE */

void run_agent_turn_transport_tests(void) {
    HU_TEST_SUITE("agent_turn transport");
    HU_RUN_TEST(is_transport_error_returns_true_for_io);
    HU_RUN_TEST(is_transport_error_returns_true_for_timeout);
    HU_RUN_TEST(is_transport_error_returns_true_for_provider_unavailable);
    HU_RUN_TEST(is_transport_error_returns_false_for_provider_response);
    HU_RUN_TEST(is_transport_error_returns_false_for_provider_auth);
    HU_RUN_TEST(is_transport_error_returns_false_for_ok);
    HU_RUN_TEST(is_transport_error_returns_false_for_invalid_argument);
    HU_RUN_TEST(is_transport_error_returns_false_for_oom);

    HU_RUN_TEST(build_unavailable_fallback_returns_canonical_text);
    HU_RUN_TEST(build_unavailable_fallback_null_alloc_returns_invalid);
    HU_RUN_TEST(build_unavailable_fallback_null_out_returns_invalid);
    HU_RUN_TEST(build_unavailable_fallback_null_out_len_returns_invalid);

#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(agent_turn_transport_error_bails_after_one_retry);
    HU_RUN_TEST(agent_turn_transport_error_recovers_on_second_call);
#endif
}
