/* test_chain_failure_paths.c — regression tests for the deferred-audit fail-path fixes.
 *
 * Files under test: src/agent/output_validator_chain.c
 *
 * Pins behavior introduced by the HIGH-severity audit fixes:
 *   HIGH-6 (PR #95): chain final_decision is driven by rewrite_count, not by
 *                    current_owned. A validator returning REWRITE with
 *                    text_owned=false (e.g. a slice of the input as a
 *                    non-allocating optimization) must be reported as REWRITE
 *                    by the chain and the final text must be owned afterward
 *                    so downstream consumers can rely on a single contract.
 *
 * Negative pin (regression guard against re-introducing the bug):
 *   - A pure-PASS chain (no validator rewrote) must still report PASS with
 *     final_text = input and final_text_owned = false. The HIGH-6 fix must
 *     not turn PASS into REWRITE.
 *
 * Future test slots in this file: HIGH-1/HIGH-3/HIGH-5/HIGH-7 regression
 * tests will land here once a mock-agent harness exists; they need
 * hu_agent_turn / hu_agent_turn_stream_v2 fixtures that are out of scope
 * for this initial chain-level harness. */

#include "human/agent/output_validator.h"
#include "human/agent/output_validator_chain.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"

#include <string.h>

static hu_allocator_t A(void) {
    return hu_system_allocator();
}

/* ─────────────────────────────────────────────────────────────────────
 * Mock validator: returns REWRITE with text_owned=false (slice of input).
 *
 * This is the exact shape HIGH-6 fixes: before the fix, the chain reported
 * PASS because current_owned stayed false. After the fix, the chain reports
 * REWRITE and the text is copied into an owned buffer.
 * ───────────────────────────────────────────────────────────────────── */
static hu_error_t nonowned_rewrite_validate(void *ctx, hu_allocator_t *alloc,
                                            const hu_validator_context_t *vctx,
                                            const char *response, size_t response_len,
                                            hu_validator_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)vctx;
    memset(out, 0, sizeof(*out));
    /* "Strip leading whitespace" optimization: point at a slice of input,
     * do NOT allocate. text_owned stays false. */
    size_t skip = 0;
    while (skip < response_len && response[skip] == ' ')
        skip++;
    out->decision = HU_VALIDATOR_REWRITE;
    out->text = response + skip;
    out->text_len = response_len - skip;
    out->text_owned = false;
    return HU_OK;
}
static const char *nonowned_rewrite_name(void *ctx) {
    (void)ctx;
    return "nonowned_rewrite";
}
static const hu_output_validator_vtable_t nonowned_rewrite_vtable = {
    .validate = nonowned_rewrite_validate,
    .name = nonowned_rewrite_name,
    .deinit = NULL,
};

/* ─────────────────────────────────────────────────────────────────────
 * Pass-through mock: never rewrites, always PASS.
 * ───────────────────────────────────────────────────────────────────── */
static hu_error_t pass_validate(void *ctx, hu_allocator_t *alloc,
                                const hu_validator_context_t *vctx, const char *r, size_t rl,
                                hu_validator_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)vctx;
    (void)r;
    (void)rl;
    memset(out, 0, sizeof(*out));
    out->decision = HU_VALIDATOR_PASS;
    return HU_OK;
}
static const char *pass_name(void *ctx) {
    (void)ctx;
    return "pass_through";
}
static const hu_output_validator_vtable_t pass_vtable = {
    .validate = pass_validate,
    .name = pass_name,
    .deinit = NULL,
};

/* ─────────────────────────────────────────────────────────────────────
 * HIGH-6 positive: non-owned REWRITE -> chain reports REWRITE + owned text.
 * ───────────────────────────────────────────────────────────────────── */
static void high6_nonowned_rewrite_yields_owned_rewrite_decision(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_output_validator_chain_create(&alloc, &chain), HU_OK);
    hu_output_validator_t v = {.ctx = NULL, .vtable = &nonowned_rewrite_vtable};
    HU_ASSERT_EQ(hu_output_validator_chain_add(chain, v), HU_OK);

    const char *input = "   hello world";
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, input, strlen(input), &cr),
                 HU_OK);

    /* Before HIGH-6: cr.final_decision == HU_VALIDATOR_PASS (bug).
     * After  HIGH-6: cr.final_decision == HU_VALIDATOR_REWRITE. */
    HU_ASSERT_EQ((int)cr.final_decision, (int)HU_VALIDATOR_REWRITE);
    /* Chain must have copied the non-owned slice into an owned buffer so
     * consumers can rely on the contract "REWRITE always carries owned text". */
    HU_ASSERT(cr.final_text_owned);
    HU_ASSERT_NOT_NULL(cr.final_text);
    HU_ASSERT_EQ((int)cr.final_text_len, (int)strlen("hello world"));
    HU_ASSERT(memcmp(cr.final_text, "hello world", cr.final_text_len) == 0);
    HU_ASSERT_EQ((int)cr.rewrite_count, 1);

    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* ─────────────────────────────────────────────────────────────────────
 * HIGH-6 negative pin: pure PASS chain still reports PASS with non-owned
 * input pointer. Guards against the fix accidentally promoting every chain
 * outcome to REWRITE.
 * ───────────────────────────────────────────────────────────────────── */
static void high6_pure_pass_still_yields_pass_with_unowned_input(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_output_validator_chain_create(&alloc, &chain), HU_OK);
    hu_output_validator_t v = {.ctx = NULL, .vtable = &pass_vtable};
    HU_ASSERT_EQ(hu_output_validator_chain_add(chain, v), HU_OK);

    const char *input = "hello world";
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, input, strlen(input), &cr),
                 HU_OK);

    HU_ASSERT_EQ((int)cr.final_decision, (int)HU_VALIDATOR_PASS);
    /* On PASS, no copy: final_text aliases the input and is NOT owned. */
    HU_ASSERT(!cr.final_text_owned);
    HU_ASSERT(cr.final_text == input);
    HU_ASSERT_EQ((int)cr.rewrite_count, 0);

    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

/* ─────────────────────────────────────────────────────────────────────
 * HIGH-6 chain ownership transfer: non-owned REWRITE followed by another
 * non-owned REWRITE — the chain must end with ONE owned final_text, not
 * two (or zero) allocations. Verifies the in-loop ownership accounting
 * survives intermediate non-owning rewrites.
 * ───────────────────────────────────────────────────────────────────── */
static void high6_two_nonowned_rewrites_yields_single_owned_final(void) {
    hu_allocator_t alloc = A();
    hu_output_validator_chain_t *chain = NULL;
    HU_ASSERT_EQ(hu_output_validator_chain_create(&alloc, &chain), HU_OK);
    hu_output_validator_t v1 = {.ctx = NULL, .vtable = &nonowned_rewrite_vtable};
    hu_output_validator_t v2 = {.ctx = NULL, .vtable = &nonowned_rewrite_vtable};
    HU_ASSERT_EQ(hu_output_validator_chain_add(chain, v1), HU_OK);
    HU_ASSERT_EQ(hu_output_validator_chain_add(chain, v2), HU_OK);

    const char *input = "  hello";
    hu_chain_result_t cr;
    memset(&cr, 0, sizeof(cr));
    HU_ASSERT_EQ(hu_output_validator_chain_execute(chain, &alloc, NULL, input, strlen(input), &cr),
                 HU_OK);

    HU_ASSERT_EQ((int)cr.final_decision, (int)HU_VALIDATOR_REWRITE);
    HU_ASSERT(cr.final_text_owned);
    HU_ASSERT_EQ((int)cr.final_text_len, (int)strlen("hello"));
    HU_ASSERT(memcmp(cr.final_text, "hello", cr.final_text_len) == 0);
    /* Both validators rewrote, so rewrite_count is 2. */
    HU_ASSERT_EQ((int)cr.rewrite_count, 2);

    /* If ASan reports no leak/double-free here, the in-loop accounting is sound. */
    hu_chain_result_free(&alloc, &cr);
    hu_output_validator_chain_destroy(chain);
}

void run_chain_failure_paths_tests(void) {
    HU_TEST_SUITE("chain_failure_paths");
    HU_RUN_TEST(high6_nonowned_rewrite_yields_owned_rewrite_decision);
    HU_RUN_TEST(high6_pure_pass_still_yields_pass_with_unowned_input);
    HU_RUN_TEST(high6_two_nonowned_rewrites_yields_single_owned_final);
}
