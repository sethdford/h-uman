/* tests/test_onboard_step_provider.c
 *
 * Sprint 54 US-C2.3 (Phase 1) — Provider setup step tests.
 *
 * Tests cover:
 *   - Pure byte classifier (1/2/3/4/q/junk → choice enum)
 *   - Choice → canonical provider name mapper
 *   - Vtable factory wiring
 *   - run() test-injection contract (with state-persistence side effect)
 *   - No credential leak in any code path
 *
 * Test discipline:
 *   - No allow-silent-pass opt-outs.
 *   - Every assertion exercises a real contract.
 */

#include "test_framework.h"

#include "human/onboard/state.h"
#include "human/onboard/step.h"
#include "human/onboard/step_provider.h"

#include <string.h>

/* ── Pure byte classifier ─────────────────────────────────────────── */

static void test_classify_digit_1_is_mlx_local(void) {
    HU_ASSERT_EQ((int)hu_onboard_provider_classify_byte('1'), (int)HU_PROVIDER_CHOICE_MLX_LOCAL);
}

static void test_classify_digit_2_is_anthropic(void) {
    HU_ASSERT_EQ((int)hu_onboard_provider_classify_byte('2'), (int)HU_PROVIDER_CHOICE_ANTHROPIC);
}

static void test_classify_digit_3_is_gemini(void) {
    HU_ASSERT_EQ((int)hu_onboard_provider_classify_byte('3'), (int)HU_PROVIDER_CHOICE_GEMINI);
}

static void test_classify_digit_4_is_openai(void) {
    HU_ASSERT_EQ((int)hu_onboard_provider_classify_byte('4'), (int)HU_PROVIDER_CHOICE_OPENAI);
}

static void test_classify_lowercase_q_is_quit(void) {
    HU_ASSERT_EQ((int)hu_onboard_provider_classify_byte('q'), (int)HU_PROVIDER_CHOICE_QUIT);
}

static void test_classify_uppercase_Q_is_quit(void) {
    HU_ASSERT_EQ((int)hu_onboard_provider_classify_byte('Q'), (int)HU_PROVIDER_CHOICE_QUIT);
}

static void test_classify_junk_byte_is_invalid(void) {
    /* Anything other than 1/2/3/4/q/Q is INVALID — the step's
     * dispatcher will REPEAT and re-prompt. */
    HU_ASSERT_EQ((int)hu_onboard_provider_classify_byte('x'), (int)HU_PROVIDER_CHOICE_INVALID);
    HU_ASSERT_EQ((int)hu_onboard_provider_classify_byte('5'), (int)HU_PROVIDER_CHOICE_INVALID);
    HU_ASSERT_EQ((int)hu_onboard_provider_classify_byte('\n'), (int)HU_PROVIDER_CHOICE_INVALID);
    HU_ASSERT_EQ((int)hu_onboard_provider_classify_byte('\0'), (int)HU_PROVIDER_CHOICE_INVALID);
}

/* ── Choice → canonical name ──────────────────────────────────────── */

static void test_choice_to_name_mlx_local(void) {
    HU_ASSERT_STR_EQ(hu_onboard_provider_choice_to_name(HU_PROVIDER_CHOICE_MLX_LOCAL), "mlx_local");
}

static void test_choice_to_name_cloud_providers(void) {
    HU_ASSERT_STR_EQ(hu_onboard_provider_choice_to_name(HU_PROVIDER_CHOICE_ANTHROPIC), "anthropic");
    HU_ASSERT_STR_EQ(hu_onboard_provider_choice_to_name(HU_PROVIDER_CHOICE_GEMINI), "gemini");
    HU_ASSERT_STR_EQ(hu_onboard_provider_choice_to_name(HU_PROVIDER_CHOICE_OPENAI), "openai");
}

static void test_choice_to_name_quit_is_empty(void) {
    /* Quit and invalid both return empty string — the caller checks
     * the choice enum, not the name. */
    HU_ASSERT_STR_EQ(hu_onboard_provider_choice_to_name(HU_PROVIDER_CHOICE_QUIT), "");
    HU_ASSERT_STR_EQ(hu_onboard_provider_choice_to_name(HU_PROVIDER_CHOICE_INVALID), "");
    HU_ASSERT_STR_EQ(hu_onboard_provider_choice_to_name(HU_PROVIDER_CHOICE_NONE), "");
}

/* ── Vtable wiring ────────────────────────────────────────────────── */

static void test_create_returns_valid_vtable(void) {
    hu_onboard_step_t *step = hu_onboard_step_provider_create();
    HU_ASSERT_NOT_NULL(step);
    HU_ASSERT_NOT_NULL(step->run);
    HU_ASSERT_STR_EQ(step->name, "provider");
}

/* ── run() with test injection ────────────────────────────────────── */

static void test_run_with_no_user_data_returns_repeat_phase_1(void) {
    /* Phase 1 contract: without injection, step has no stdin handling,
     * so it returns REPEAT (the dispatcher will re-render). Phase 2
     * adds the real interactive read loop. */
    hu_onboard_step_t *step = hu_onboard_step_provider_create();
    step->user_data = NULL;
    hu_onboard_state_t state;
    hu_onboard_state_init(&state);
    HU_ASSERT_EQ((int)step->run(step, &state), (int)HU_ONBOARD_REPEAT);
}

static void test_run_with_injected_mlx_choice_persists_name(void) {
    hu_onboard_step_t *step = hu_onboard_step_provider_create();
    hu_onboard_provider_test_input_t inj = {
        .choice = HU_PROVIDER_CHOICE_MLX_LOCAL,
        .injected_result = HU_ONBOARD_NEXT,
    };
    step->user_data = &inj;
    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)step->run(step, &state), (int)HU_ONBOARD_NEXT);
    /* State persistence happens BEFORE the return — pin both the
     * persisted name AND the result. */
    HU_ASSERT_STR_EQ(state.provider.provider_name, "mlx_local");
    /* Phase 1: smoke-check not wired, so passed flag stays false. */
    HU_ASSERT_TRUE(state.provider.provider_smoke_passed == false);

    step->user_data = NULL;
}

static void test_run_with_injected_anthropic_choice_persists_name(void) {
    hu_onboard_step_t *step = hu_onboard_step_provider_create();
    hu_onboard_provider_test_input_t inj = {
        .choice = HU_PROVIDER_CHOICE_ANTHROPIC,
        .injected_result = HU_ONBOARD_NEXT,
    };
    step->user_data = &inj;
    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)step->run(step, &state), (int)HU_ONBOARD_NEXT);
    HU_ASSERT_STR_EQ(state.provider.provider_name, "anthropic");

    step->user_data = NULL;
}

static void test_run_with_injected_quit_does_not_persist_name(void) {
    /* QUIT is not a provider choice; the name field must stay empty
     * (init value). Pinning this prevents an accidental "you quit but
     * we saved your half-selection anyway" bug. */
    hu_onboard_step_t *step = hu_onboard_step_provider_create();
    hu_onboard_provider_test_input_t inj = {
        .choice = HU_PROVIDER_CHOICE_QUIT,
        .injected_result = HU_ONBOARD_QUIT,
    };
    step->user_data = &inj;
    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)step->run(step, &state), (int)HU_ONBOARD_QUIT);
    /* Name must be empty (init value) — QUIT does NOT persist. */
    HU_ASSERT_STR_EQ(state.provider.provider_name, "");

    step->user_data = NULL;
}

static void test_run_with_injected_invalid_does_not_persist_name(void) {
    /* INVALID = junk byte → no name persisted. The dispatcher will
     * REPEAT the step to re-prompt. */
    hu_onboard_step_t *step = hu_onboard_step_provider_create();
    hu_onboard_provider_test_input_t inj = {
        .choice = HU_PROVIDER_CHOICE_INVALID,
        .injected_result = HU_ONBOARD_REPEAT,
    };
    step->user_data = &inj;
    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ((int)step->run(step, &state), (int)HU_ONBOARD_REPEAT);
    HU_ASSERT_STR_EQ(state.provider.provider_name, "");

    step->user_data = NULL;
}

/* ── Credential-leak defense ──────────────────────────────────────── */

static void test_state_struct_has_no_api_key_field(void) {
    /* The state struct MUST NOT carry an API key field anywhere.
     * Per the design's privacy contract: keys are stack-buffer
     * ephemeral, never persisted to state or config. This test
     * structurally enforces that contract — if a future PR adds an
     * api_key field to state.provider, the static_assert below fails.
     *
     * Approach: assert the sub-struct size doesn't grow beyond its
     * known fields. provider_name[32] + provider_smoke_passed (bool)
     * = ~33 bytes + padding. We allow 64 as a safe upper bound; an
     * api_key field would be ≥128 bytes by convention and bust this. */
    HU_ASSERT_TRUE(sizeof(((hu_onboard_state_t *)0)->provider) <= 64);
}

/* ── runner ───────────────────────────────────────────────────────── */

void run_onboard_step_provider_tests(void) {
    HU_TEST_SUITE("onboard_step_provider");

    HU_RUN_TEST(test_classify_digit_1_is_mlx_local);
    HU_RUN_TEST(test_classify_digit_2_is_anthropic);
    HU_RUN_TEST(test_classify_digit_3_is_gemini);
    HU_RUN_TEST(test_classify_digit_4_is_openai);
    HU_RUN_TEST(test_classify_lowercase_q_is_quit);
    HU_RUN_TEST(test_classify_uppercase_Q_is_quit);
    HU_RUN_TEST(test_classify_junk_byte_is_invalid);

    HU_RUN_TEST(test_choice_to_name_mlx_local);
    HU_RUN_TEST(test_choice_to_name_cloud_providers);
    HU_RUN_TEST(test_choice_to_name_quit_is_empty);

    HU_RUN_TEST(test_create_returns_valid_vtable);

    HU_RUN_TEST(test_run_with_no_user_data_returns_repeat_phase_1);
    HU_RUN_TEST(test_run_with_injected_mlx_choice_persists_name);
    HU_RUN_TEST(test_run_with_injected_anthropic_choice_persists_name);
    HU_RUN_TEST(test_run_with_injected_quit_does_not_persist_name);
    HU_RUN_TEST(test_run_with_injected_invalid_does_not_persist_name);

    HU_RUN_TEST(test_state_struct_has_no_api_key_field);
}
