/* test_persona_fidelity_validator — smoke test for the M3 stub validator.
 *
 * src/agent/validators/persona_fidelity_validator.c is the on-disk slot
 * for the future on-device persona-fidelity classifier. Today it
 * unconditionally returns HU_VALIDATOR_PASS so chain composition can
 * include it without changing behavior. This test pins that contract
 * so a future change to the stub (real classifier, threshold, lookup)
 * doesn't silently bypass the existing chain.
 *
 * Pre-existed in the tree without test coverage; flagged by
 * scripts/check-untested.sh on CI for PR #104.
 */

#include "human/agent/output_validator.h"
#include "human/agent/validators/builtin.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static void persona_fidelity_validator_create_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_output_validator_t v = {0};
    HU_ASSERT_EQ(hu_validator_persona_fidelity_create(&alloc, &v), HU_OK);
    HU_ASSERT_NOT_NULL(v.vtable);
    HU_ASSERT_NOT_NULL(v.vtable->name);
    HU_ASSERT_STR_EQ(v.vtable->name(v.ctx), "persona_fidelity");
}

static void persona_fidelity_validator_rejects_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_validator_persona_fidelity_create(&alloc, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void persona_fidelity_validator_validate_always_passes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_output_validator_t v = {0};
    HU_ASSERT_EQ(hu_validator_persona_fidelity_create(&alloc, &v), HU_OK);
    hu_validator_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.channel_id = "imessage";
    ctx.channel_id_len = 8;
    hu_validator_result_t out;
    memset(&out, 0, sizeof(out));
    const char *resp = "hey sounds good";
    HU_ASSERT_EQ(v.vtable->validate(v.ctx, &alloc, &ctx, resp, strlen(resp), &out), HU_OK);
    HU_ASSERT_EQ((long)out.decision, (long)HU_VALIDATOR_PASS);
}

static void persona_fidelity_validator_passes_on_empty_response(void) {
    /* Stub validator must tolerate empty responses without crashing —
     * the chain feeds whatever the LLM returns, including empty strings
     * from rare provider edge cases. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_output_validator_t v = {0};
    HU_ASSERT_EQ(hu_validator_persona_fidelity_create(&alloc, &v), HU_OK);
    hu_validator_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    hu_validator_result_t out;
    memset(&out, 0, sizeof(out));
    HU_ASSERT_EQ(v.vtable->validate(v.ctx, &alloc, &ctx, "", 0, &out), HU_OK);
    HU_ASSERT_EQ((long)out.decision, (long)HU_VALIDATOR_PASS);
}

void run_persona_fidelity_validator_tests(void) {
    HU_TEST_SUITE("persona_fidelity_validator");
    HU_RUN_TEST(persona_fidelity_validator_create_succeeds);
    HU_RUN_TEST(persona_fidelity_validator_rejects_null_out);
    HU_RUN_TEST(persona_fidelity_validator_validate_always_passes);
    HU_RUN_TEST(persona_fidelity_validator_passes_on_empty_response);
}
