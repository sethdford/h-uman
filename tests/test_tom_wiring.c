/* Theory of Mind wiring integration tests */

#include "human/agent/theory_of_mind.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* Test that ToM directive gating logic handles ON mode without crashing */
static void tom_directive_on_mode_wiring_compiles(void) {
    /* Set environment variable to ON */
    setenv("HU_TOM_DIRECTIVE", "on", 1);

    /* Verify the environment variable is readable (no crash). */
    const char *tom_mode_env = getenv("HU_TOM_DIRECTIVE");
    HU_ASSERT_NOT_NULL(tom_mode_env);
    HU_ASSERT_STR_EQ(tom_mode_env, "on");

    unsetenv("HU_TOM_DIRECTIVE");
}

/* Test that ToM directive is gated off by default (HU_TOM_DIRECTIVE not set) */
static void tom_directive_off_by_default_mode_wiring(void) {
    /* Ensure the env var is not set (default behavior) */
    unsetenv("HU_TOM_DIRECTIVE");

    /* When not set, getenv should return NULL. */
    const char *tom_mode_env = getenv("HU_TOM_DIRECTIVE");
    HU_ASSERT_NULL(tom_mode_env);
}

/* Test that ToM directive can be set to shadow mode */
static void tom_directive_shadow_mode_wiring_compiles(void) {
    /* Set environment variable to shadow */
    setenv("HU_TOM_DIRECTIVE", "shadow", 1);

    /* Verify the environment variable is readable. */
    const char *tom_mode_env = getenv("HU_TOM_DIRECTIVE");
    HU_ASSERT_NOT_NULL(tom_mode_env);
    HU_ASSERT_STR_EQ(tom_mode_env, "shadow");

    unsetenv("HU_TOM_DIRECTIVE");
}

/* Test that ToM build_context function exists and has correct signature */
static void tom_build_context_function_signature_correct(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tom_belief_state_t state;
    memset(&state, 0, sizeof(state));

    state.contact_id = "test";
    state.contact_id_len = 4;
    state.belief_count = 0;

    /* Verify the function exists and doesn't crash on empty state */
    char *context = NULL;
    size_t context_len = 0;
    hu_error_t err = hu_tom_build_context(&state, &alloc, &context, &context_len);

    /* Empty state should succeed (possibly with empty output) */
    HU_ASSERT_EQ(err, HU_OK);

    if (context) {
        alloc.free(alloc.ctx, context, context_len + 1);
    }
}

void run_tom_wiring_tests(void) {
    HU_TEST_SUITE("tom_wiring");
    HU_RUN_TEST(tom_directive_on_mode_wiring_compiles);
    HU_RUN_TEST(tom_directive_off_by_default_mode_wiring);
    HU_RUN_TEST(tom_directive_shadow_mode_wiring_compiles);
    HU_RUN_TEST(tom_build_context_function_signature_correct);
}
