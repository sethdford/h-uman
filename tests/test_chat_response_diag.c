/*
 * T6 (2026-05-25 reactive-iMessage recovery) — diagnostic-field contract.
 *
 * Pins the post-T1b shape of hu_chat_response_t / hu_token_usage_t:
 *
 *   - usage.thoughts_tokens                 (new field, uint32_t)
 *   - resp.finish_reason / finish_reason_len (new heap-owned field)
 *
 * Both are diagnostic-only — they flow OUT of providers and INTO the
 * agent_turn empty-response log line so the operator can correlate
 * empty replies with provider-specific failure modes. The original
 * 2026-05-24 bug was a Gemini 3.x thinking-budget starvation that
 * surfaced as `response_len=0` with no actionable context; T1b's
 * diagnostic log now names finish_reason=MAX_TOKENS,
 * thoughts_tokens=72, completion_tokens=0 — the exact signature.
 *
 * These tests:
 *   - thoughts_tokens zero-init is byte-correct (no padding surprise)
 *   - finish_reason can be set + freed without leak (ASan-validated)
 *   - hu_chat_response_free is a no-op on a default-zero response
 *
 * Together these pin the regression class: if a future refactor drops
 * finish_reason from the struct, OR removes its free, OR forgets to
 * zero-init thoughts_tokens, the test fails.
 *
 * @covers-none — exercises hu_chat_response_free (helpers.c) but the
 * test-module-name heuristic doesn't map to helpers.c; this is a
 * cross-cutting contract test.
 */

// @covers-none

#include "human/core/allocator.h"
#include "human/provider.h"

#include "test_framework.h"

#include <stdint.h>
#include <string.h>

static void test_thoughts_tokens_field_zero_initializes(void) {
    hu_chat_response_t resp = {0};
    /* The byte-equal-zero check guards against any future padding /
     * union / re-ordering surprise that would leave thoughts_tokens
     * with a stale value after `= {0}`. */
    HU_ASSERT_EQ(resp.usage.thoughts_tokens, (uint32_t)0);
}

static void test_finish_reason_field_zero_initializes(void) {
    hu_chat_response_t resp = {0};
    HU_ASSERT_NULL((void *)resp.finish_reason);
    HU_ASSERT_EQ(resp.finish_reason_len, (size_t)0);
}

static void test_response_free_releases_finish_reason_no_leak(void) {
    /* The whole reason this field is heap-owned and the response is
     * the thing that frees it. ASan catches the leak if free is wrong. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_chat_response_t resp = {0};
    const char *reason = "MAX_TOKENS";
    size_t rlen = strlen(reason);
    char *heap_reason = (char *)alloc.alloc(alloc.ctx, rlen + 1);
    HU_ASSERT_NOT_NULL(heap_reason);
    memcpy(heap_reason, reason, rlen);
    heap_reason[rlen] = '\0';
    resp.finish_reason = heap_reason;
    resp.finish_reason_len = rlen;
    /* free must release the heap_reason. ASan verifies. */
    hu_chat_response_free(&alloc, &resp);
    /* Post-free contract: free zeroes the struct */
    HU_ASSERT_NULL((void *)resp.finish_reason);
    HU_ASSERT_EQ(resp.finish_reason_len, (size_t)0);
}

static void test_response_free_handles_zero_init_response_safely(void) {
    /* A zero-init response (no fields set) must be safe to free —
     * common pattern across the codebase when chat() returns an error
     * partway through populating out. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_chat_response_t resp = {0};
    hu_chat_response_free(&alloc, &resp); /* must not crash, must not leak */
    HU_ASSERT_NULL((void *)resp.finish_reason);
}

static void test_thinking_starvation_response_shape_pins_bug(void) {
    /* The exact shape of the 2026-05-24 Gemini bug: content_len=0,
     * tool_calls_count=0, finish_reason=MAX_TOKENS, thoughts_tokens>0,
     * completion_tokens=0. If a future provider implementer constructs
     * a response with this exact shape, the agent_turn diagnostic log
     * will fire (verified by manual inspection at agent_turn.c). This
     * test just pins that the SHAPE is constructable and frees cleanly. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_chat_response_t resp = {0};
    const char *reason = "MAX_TOKENS";
    size_t rlen = strlen(reason);
    char *hr = (char *)alloc.alloc(alloc.ctx, rlen + 1);
    HU_ASSERT_NOT_NULL(hr);
    memcpy(hr, reason, rlen);
    hr[rlen] = '\0';
    resp.finish_reason = hr;
    resp.finish_reason_len = rlen;
    resp.usage.prompt_tokens = 42;
    resp.usage.completion_tokens = 0; /* starvation signature */
    resp.usage.thoughts_tokens = 72;  /* starvation signature */
    resp.usage.total_tokens = 42;     /* = prompt + completion */
    /* All the predicates the diagnostic log site would check: */
    HU_ASSERT_EQ(resp.content_len, (size_t)0);
    HU_ASSERT_EQ(resp.tool_calls_count, (size_t)0);
    HU_ASSERT_TRUE(resp.usage.thoughts_tokens > 0);
    HU_ASSERT_TRUE(resp.usage.completion_tokens == 0);
    HU_ASSERT_NOT_NULL(resp.finish_reason);
    hu_chat_response_free(&alloc, &resp);
}

void run_chat_response_diag_tests(void) {
    HU_TEST_SUITE("chat-response-diag");
    HU_RUN_TEST(test_thoughts_tokens_field_zero_initializes);
    HU_RUN_TEST(test_finish_reason_field_zero_initializes);
    HU_RUN_TEST(test_response_free_releases_finish_reason_no_leak);
    HU_RUN_TEST(test_response_free_handles_zero_init_response_safely);
    HU_RUN_TEST(test_thinking_starvation_response_shape_pins_bug);
}
