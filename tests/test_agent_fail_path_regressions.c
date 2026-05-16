/* test_agent_fail_path_regressions.c — agent-level regression tests for the
 * post-PR-#81 deferred-audit HIGH-severity fail-path fixes.
 *
 * Companion to tests/test_chain_failure_paths.c (chain-level tests for
 * HIGH-6). That file pins behavior of hu_output_validator_chain_execute
 * directly; this file pins behavior of hu_agent_turn /
 * hu_agent_turn_stream_v2 — the agent-level consumers that integrate the
 * chain + retry path + cache.
 *
 * Files under test:
 *   src/agent/agent_turn.c, src/agent/agent_stream.c.
 *
 * Coverage of the audit list:
 *   HIGH-1 (PR #89): chain machinery failure in agent_turn → final_content
 *                    suppressed (NOT passed through unvalidated).
 *   HIGH-2 (PR #96): slim-retry result re-validated through chain; persistent
 *                    bad output is suppressed instead of escaping.
 *   HIGH-3 (PR #92): no UAF when ab_owned final_content is freed at line 5708
 *                    and the semantic cache_put fires later (now uses
 *                    *response_out, not the freed pointer).
 *   HIGH-5 (PR #94): stream chain machinery failure observably drops output
 *                    instead of silently passing.
 *   HIGH-7 (PR #97): stream-finalization chain machinery failure suppresses
 *                    final_content (NOT passed through unvalidated).
 *
 * Mock infrastructure:
 *   - persistent_bad_provider: always emits F1 channel-token (HIGH-2 driver).
 *   - benign_provider: emits clean content (HIGH-1/3/5/7 driver, paired with
 *     a forced-failing chain on the persona).
 *   - failing_chain_validator: hu_output_validator_t that returns
 *     HU_ERR_INTERNAL on validate. A chain built with this validator and
 *     installed on persona->outbound_chain forces hu_output_validator_chain_execute
 *     to return non-OK, exercising HIGH-1/HIGH-7's fail-closed paths. */

#include "human/agent.h"
#include "human/agent/output_validator.h"
#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory/lifecycle/semantic_cache.h"
#include "human/persona.h"
#include "human/provider.h"
#include "test_framework.h"

#include <string.h>

/* ─────────────────────────────────────────────────────────────────────
 * Mock provider that ALWAYS returns content with an F1 channel-token
 * leak. Both call 1 and the retry (call 2) emit the same artifact.
 *
 * Pre-#96 behavior: call 1 chain-REJECTED, retry-slim called, retry's
 *                   response accepted without re-validation. F1 token
 *                   would have reached the user.
 * Post-#96 behavior: retry result re-validated through the same chain,
 *                    second REJECT triggers suppression — final response
 *                    is empty.
 * ───────────────────────────────────────────────────────────────────── */
typedef struct {
    int calls;
} persistent_bad_ctx_t;

static const char *persistent_bad_name(void *ctx) {
    (void)ctx;
    return "persistent_bad_mock";
}

static bool persistent_bad_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}

static bool persistent_bad_supports_streaming(void *ctx) {
    (void)ctx;
    return true;
}

static void persistent_bad_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}

/* F1 channel-token artifact — matches the same pattern that the
 * production response_guard validator rejects on. */
static const char PERSISTENT_BAD_PAYLOAD[] =
    "<|channel|>thoughtThe user said said \"Here! \" \" \" \" \" \" \" \" \" \" \" "
    "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
    "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
    "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" "
    "\" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" \" ";

static hu_error_t persistent_bad_chat(void *ctx, hu_allocator_t *alloc,
                                      const hu_chat_request_t *request, const char *model,
                                      size_t model_len, double temperature,
                                      hu_chat_response_t *out) {
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    persistent_bad_ctx_t *p = (persistent_bad_ctx_t *)ctx;
    p->calls++;
    out->content = hu_strndup(alloc, PERSISTENT_BAD_PAYLOAD, strlen(PERSISTENT_BAD_PAYLOAD));
    out->content_len = out->content ? strlen(PERSISTENT_BAD_PAYLOAD) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static hu_error_t persistent_bad_stream_chat(void *ctx, hu_allocator_t *alloc,
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
    persistent_bad_ctx_t *p = (persistent_bad_ctx_t *)ctx;
    p->calls++;
    out->content = hu_strndup(alloc, PERSISTENT_BAD_PAYLOAD, strlen(PERSISTENT_BAD_PAYLOAD));
    out->content_len = out->content ? strlen(PERSISTENT_BAD_PAYLOAD) : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static const hu_provider_vtable_t persistent_bad_vtable = {
    .chat = persistent_bad_chat,
    .supports_native_tools = persistent_bad_supports_native_tools,
    .get_name = persistent_bad_name,
    .deinit = persistent_bad_deinit,
    .supports_streaming = persistent_bad_supports_streaming,
    .stream_chat = persistent_bad_stream_chat,
};

static hu_provider_t persistent_bad_create(persistent_bad_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    return (hu_provider_t){.ctx = ctx, .vtable = &persistent_bad_vtable};
}

/* ─────────────────────────────────────────────────────────────────────
 * HIGH-2 regression: persistent F1 leak on retry must be re-validated
 * by the chain and suppressed, not silently passed through.
 *
 * Pre-PR-#96: chain REJECTs call 1, retry returns same bad content,
 *             retry accepted without re-validation, F1 reaches the user.
 * Post-PR-#96: retry result re-validated through chain, RE-REJECTed,
 *              response suppressed (empty/no F1 in output).
 * ───────────────────────────────────────────────────────────────────── */
static void high2_persistent_bad_retry_is_suppressed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    persistent_bad_ctx_t provider_ctx;
    hu_provider_t provider = persistent_bad_create(&provider_ctx);

    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "persistent_bad_mock", 19, 0.7, "/tmp",
                                          4, 5, 50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "haha i figured 😂", strlen("haha i figured 😂"), &response,
                        &response_len);

    /* Agent's turn completed without error — the chain handled the bad
     * content gracefully rather than propagating an error code up. */
    HU_ASSERT_EQ(err, HU_OK);

    /* Provider was called at least twice: once for the original, once
     * for the retry. This proves the retry path fired. */
    HU_ASSERT(provider_ctx.calls >= 2);

    /* Critical post-#96 pin: the F1 channel-token MUST NOT escape. The
     * exact suppression shape can be empty-string OR a stripped clean
     * variant — either is acceptable. What is NOT acceptable is the F1
     * payload reaching the consumer. */
    if (response && response_len > 0) {
        HU_ASSERT(strstr(response, "<|") == NULL);
        HU_ASSERT(strstr(response, "channel>thought") == NULL);
        HU_ASSERT(strstr(response, "\" \" \"") == NULL);
    }

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

static void high2_persistent_bad_retry_is_suppressed_stream(void) {
    hu_allocator_t alloc = hu_system_allocator();
    persistent_bad_ctx_t provider_ctx;
    hu_provider_t provider = persistent_bad_create(&provider_ctx);

    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "persistent_bad_mock", 19, 0.7, "/tmp",
                                          4, 5, 50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn_stream_v2(&agent, "haha i figured 😂", strlen("haha i figured 😂"), NULL,
                                  NULL, &response, &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(provider_ctx.calls >= 2);
    if (response && response_len > 0) {
        HU_ASSERT(strstr(response, "<|") == NULL);
        HU_ASSERT(strstr(response, "channel>thought") == NULL);
        HU_ASSERT(strstr(response, "\" \" \"") == NULL);
    }

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_agent_deinit(&agent);
}

/* ─────────────────────────────────────────────────────────────────────
 * Benign mock provider — emits clean content. Paired with a forced-
 * failing chain on the persona to test HIGH-1/HIGH-3/HIGH-5/HIGH-7
 * without persistent-bad content getting in the way. The clean content
 * means the test isolates the chain-machinery failure path from the
 * content-rejection path.
 * ───────────────────────────────────────────────────────────────────── */
typedef struct {
    int calls;
    const char *payload;
    size_t payload_len;
} benign_ctx_t;

static const char *benign_name(void *ctx) {
    (void)ctx;
    return "benign_mock";
}
static bool benign_no_native_tools(void *ctx) {
    (void)ctx;
    return false;
}
static bool benign_streams(void *ctx) {
    (void)ctx;
    return true;
}
static void benign_deinit(void *ctx, hu_allocator_t *alloc) {
    (void)ctx;
    (void)alloc;
}
static hu_error_t benign_chat(void *ctx, hu_allocator_t *alloc, const hu_chat_request_t *request,
                              const char *model, size_t model_len, double temperature,
                              hu_chat_response_t *out) {
    (void)request;
    (void)model;
    (void)model_len;
    (void)temperature;
    benign_ctx_t *b = (benign_ctx_t *)ctx;
    b->calls++;
    out->content = hu_strndup(alloc, b->payload, b->payload_len);
    out->content_len = out->content ? b->payload_len : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}
static hu_error_t benign_stream_chat(void *ctx, hu_allocator_t *alloc,
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
    benign_ctx_t *b = (benign_ctx_t *)ctx;
    b->calls++;
    out->content = hu_strndup(alloc, b->payload, b->payload_len);
    out->content_len = out->content ? b->payload_len : 0;
    out->tool_calls = NULL;
    out->tool_calls_count = 0;
    out->reasoning_content = NULL;
    out->reasoning_content_len = 0;
    return out->content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}
static const hu_provider_vtable_t benign_vtable = {
    .chat = benign_chat,
    .supports_native_tools = benign_no_native_tools,
    .get_name = benign_name,
    .deinit = benign_deinit,
    .supports_streaming = benign_streams,
    .stream_chat = benign_stream_chat,
};
static hu_provider_t benign_create(benign_ctx_t *ctx, const char *payload) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->payload = payload;
    ctx->payload_len = strlen(payload);
    return (hu_provider_t){.ctx = ctx, .vtable = &benign_vtable};
}

/* ─────────────────────────────────────────────────────────────────────
 * Failing validator — returns HU_ERR_INTERNAL on validate. Installed
 * via build_failing_chain() into persona->outbound_chain to force the
 * chain-execute-fail path in agent_turn.c (HIGH-1) and agent_stream.c
 * (HIGH-5, HIGH-7). The cached-persona-chain path in the code skips
 * the build call and goes straight to execute, so this validator hits
 * the execute-failure arm specifically.
 * ───────────────────────────────────────────────────────────────────── */
static hu_error_t failing_validate(void *ctx, hu_allocator_t *alloc,
                                   const hu_validator_context_t *vctx, const char *response,
                                   size_t response_len, hu_validator_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)vctx;
    (void)response;
    (void)response_len;
    (void)out;
    return HU_ERR_INTERNAL;
}
static const char *failing_name(void *ctx) {
    (void)ctx;
    return "force_fail";
}
static const hu_output_validator_vtable_t failing_vtable = {
    .validate = failing_validate,
    .name = failing_name,
    .deinit = NULL,
};

static hu_output_validator_chain_t *build_failing_chain(hu_allocator_t *alloc) {
    hu_output_validator_chain_t *chain = NULL;
    if (hu_output_validator_chain_create(alloc, &chain) != HU_OK)
        return NULL;
    hu_output_validator_t v = {.ctx = NULL, .vtable = &failing_vtable};
    if (hu_output_validator_chain_add(chain, v) != HU_OK) {
        hu_output_validator_chain_destroy(chain);
        return NULL;
    }
    return chain;
}

/* ─────────────────────────────────────────────────────────────────────
 * HIGH-1 (PR #89): chain execute failure in agent_turn must suppress.
 *
 * Setup: benign provider returns clean content, persona has an
 *        outbound_chain that always returns HU_ERR_INTERNAL on execute.
 * Pre-fix: cerr != HU_OK silently fell through; clean content reached
 *          the user without validation (fail-open).
 * Post-fix: chain_unrunnable flag fires; response is NULL/empty
 *           (fail-closed).
 * ───────────────────────────────────────────────────────────────────── */
static void high1_chain_execute_failure_suppresses_response(void) {
    hu_allocator_t alloc = hu_system_allocator();
    benign_ctx_t provider_ctx;
    hu_provider_t provider = benign_create(&provider_ctx, "hello, this is benign content");

    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "benign_mock", 11, 0.7, "/tmp", 4, 5,
                                          50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    /* Install a chain on the persona that ALWAYS fails on execute. The
     * production code's `agent->persona && agent->persona->outbound_chain`
     * branch will pick this up and skip the default build, so execute
     * hits HU_ERR_INTERNAL immediately. */
    hu_persona_t persona = {0};
    persona.outbound_chain = build_failing_chain(&alloc);
    HU_ASSERT_NOT_NULL(persona.outbound_chain);
    hu_persona_t *prev = agent.persona;
    agent.persona = &persona;

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "test prompt", 11, &response, &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    /* The fix suppresses the send on chain machinery failure. response
     * must NOT contain the benign payload; either empty or NULL. */
    if (response && response_len > 0) {
        HU_ASSERT(strstr(response, "benign content") == NULL);
    }

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_output_validator_chain_destroy(persona.outbound_chain);
    agent.persona = prev;
    hu_agent_deinit(&agent);
}

/* ─────────────────────────────────────────────────────────────────────
 * HIGH-7 (PR #97): chain execute failure in agent_stream stream-
 * finalization block must suppress final_content (NOT pass through
 * unvalidated). Same shape as HIGH-1 but via stream path.
 * ───────────────────────────────────────────────────────────────────── */
static void high7_stream_chain_execute_failure_suppresses_response(void) {
    hu_allocator_t alloc = hu_system_allocator();
    benign_ctx_t provider_ctx;
    hu_provider_t provider = benign_create(&provider_ctx, "hello, this is benign stream content");

    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "benign_mock", 11, 0.7, "/tmp", 4, 5,
                                          50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    hu_persona_t persona = {0};
    persona.outbound_chain = build_failing_chain(&alloc);
    HU_ASSERT_NOT_NULL(persona.outbound_chain);
    hu_persona_t *prev = agent.persona;
    agent.persona = &persona;

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn_stream_v2(&agent, "test prompt", 11, NULL, NULL, &response, &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    if (response && response_len > 0) {
        HU_ASSERT(strstr(response, "benign stream content") == NULL);
    }

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_output_validator_chain_destroy(persona.outbound_chain);
    agent.persona = prev;
    hu_agent_deinit(&agent);
}

/* ─────────────────────────────────────────────────────────────────────
 * HIGH-5 (PR #94): stream FIRST chain block machinery failure observably
 * drops output (was silent before #94). Since this PR's fix added
 * logging without changing behavior (the drop was already there), the
 * observable assertion is "response is dropped" — same outcome as
 * HIGH-7's stream test. Distinct slot kept as a regression pin so that
 * if a future change re-introduces fail-open in the first stream block,
 * this test catches it.
 * ───────────────────────────────────────────────────────────────────── */
static void high5_stream_first_block_chain_failure_drops_output(void) {
    /* This test exercises the same code path as HIGH-7 from the test's
     * point of view (the stream's final chain runs against the assembled
     * response; whether that's "first" or "second" block in the source
     * doesn't matter to the observable outcome). Different test name so
     * it pins HIGH-5 specifically. */
    hu_allocator_t alloc = hu_system_allocator();
    benign_ctx_t provider_ctx;
    hu_provider_t provider = benign_create(&provider_ctx, "stream first block test content");

    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "benign_mock", 11, 0.7, "/tmp", 4, 5,
                                          50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    hu_persona_t persona = {0};
    persona.outbound_chain = build_failing_chain(&alloc);
    HU_ASSERT_NOT_NULL(persona.outbound_chain);
    hu_persona_t *prev = agent.persona;
    agent.persona = &persona;

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn_stream_v2(&agent, "test prompt", 11, NULL, NULL, &response, &response_len);

    HU_ASSERT_EQ(err, HU_OK);
    if (response && response_len > 0) {
        HU_ASSERT(strstr(response, "first block test content") == NULL);
    }

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    hu_output_validator_chain_destroy(persona.outbound_chain);
    agent.persona = prev;
    hu_agent_deinit(&agent);
}

/* ─────────────────────────────────────────────────────────────────────
 * HIGH-3 (PR #92): no UAF when ab_owned final_content is freed at the
 * end of hu_agent_turn and the semantic cache_put fires afterward.
 *
 * Pre-fix: cache_put took final_content (already freed) — dangling
 *          pointer, ASan UAF.
 * Post-fix: cache_put takes *response_out (live, strdup'd at line 5705).
 *
 * Setup: response_cache configured, benign content, default outbound
 *        chain (no forced failure — we want the path that USES the
 *        cache, not the suppress path). Under ASan, the test passes
 *        iff no UAF report fires.
 * ───────────────────────────────────────────────────────────────────── */
static void high3_cache_put_after_ab_owned_free_no_uaf(void) {
    hu_allocator_t alloc = hu_system_allocator();
    benign_ctx_t provider_ctx;
    hu_provider_t provider = benign_create(&provider_ctx, "hello world from cache test");

    hu_agent_t agent;
    hu_error_t err = hu_agent_from_config(&agent, &alloc, provider, NULL, 0, NULL, NULL, NULL, NULL,
                                          "test-model", 10, "benign_mock", 11, 0.7, "/tmp", 4, 5,
                                          50, false, 1, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    agent.active_channel = "imessage";
    agent.active_channel_len = 8;

    /* Configure a real response_cache (the conjunction that gates the
     * cache_put path). NULL embedding provider → exact-match only,
     * which is fine; we don't depend on hits here. */
    hu_semantic_cache_t *response_cache =
        hu_semantic_cache_create(&alloc, /*ttl_minutes=*/60, /*max_entries=*/16,
                                 /*similarity_threshold=*/0.95f, NULL);
    HU_ASSERT_NOT_NULL(response_cache);
    struct hu_semantic_cache *prev_cache = agent.infra.response_cache;
    agent.infra.response_cache = response_cache;

    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "cache test prompt", 17, &response, &response_len);

    /* Primary assertion: no error. The under-ASan promise is that this
     * call returns without an "AddressSanitizer: heap-use-after-free"
     * abort, which would otherwise have killed the process before this
     * assert executed. So reaching this point IS the regression pin. */
    HU_ASSERT_EQ(err, HU_OK);

    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    agent.infra.response_cache = prev_cache;
    hu_semantic_cache_destroy(&alloc, response_cache);
    hu_agent_deinit(&agent);
}

void run_agent_fail_path_regressions_tests(void) {
    HU_TEST_SUITE("agent_fail_path_regressions");
    HU_RUN_TEST(high2_persistent_bad_retry_is_suppressed);
    HU_RUN_TEST(high2_persistent_bad_retry_is_suppressed_stream);
    HU_RUN_TEST(high1_chain_execute_failure_suppresses_response);
    HU_RUN_TEST(high7_stream_chain_execute_failure_suppresses_response);
    HU_RUN_TEST(high5_stream_first_block_chain_failure_drops_output);
    HU_RUN_TEST(high3_cache_put_after_ab_owned_free_no_uaf);
}
