/* test_agent_fail_path_regressions.c — agent-level regression tests for the
 * post-PR-#81 deferred-audit HIGH-severity fail-path fixes.
 *
 * Companion to tests/test_chain_failure_paths.c (chain-level tests for
 * HIGH-6). That file pins behavior of hu_output_validator_chain_execute
 * directly; this file pins behavior of hu_agent_turn /
 * hu_agent_turn_stream_v2 — the agent-level consumers that integrate the
 * chain + retry path + cache.
 *
 * Pattern: a mock provider returns content that triggers chain REJECT.
 * The agent dispatches retry. The mock returns the SAME bad content on
 * retry. The fix (PR #96) re-validates the retry result against the chain,
 * so the second REJECT triggers suppression instead of letting the bad
 * content escape.
 *
 * Files under test:
 *   src/agent/agent_turn.c, src/agent/agent_stream.c.
 *
 * Closes the [ ] follow-up regression-test slot for HIGH-2 (slim-retry
 * re-validation against chain). Slots for HIGH-1, HIGH-3, HIGH-5, HIGH-7
 * are documented as TODO in the body — they need additional mock infra
 * (fault-injectable allocator for HIGH-1/HIGH-5/HIGH-7; response_cache
 * fixture for HIGH-3). */

#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
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

void run_agent_fail_path_regressions_tests(void) {
    HU_TEST_SUITE("agent_fail_path_regressions");
    HU_RUN_TEST(high2_persistent_bad_retry_is_suppressed);
    HU_RUN_TEST(high2_persistent_bad_retry_is_suppressed_stream);
}
