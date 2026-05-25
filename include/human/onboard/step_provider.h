/* include/human/onboard/step_provider.h
 *
 * Sprint 54 US-C2.3 (Phase 1) — Provider setup onboarding step.
 *
 * Second screen of the wizard. Presents 4 provider choices
 * (local-MLX / Anthropic / Gemini / OpenAI), reads the user's
 * selection, persists the choice into state->provider.provider_name,
 * and (in Phase 2) runs the smoke-check from US-C3.3.
 *
 * Phase 1 scope:
 *   - Vtable factory + menu classification + state persistence
 *   - Test injection via user_data (deterministic CI)
 *   - 14 tests pinning the contract
 *
 * Phase 2 (deferred):
 *   - Actual stdin read loop with API-key prompt
 *   - Smoke-check integration via hu_doctor_check_provider's
 *     smoke function (lands when US-C3.3 Phase 2 wires the network call)
 *   - Re-prompt logic on validation failure
 *
 * Test injection contract:
 *   When step->user_data is non-NULL, it's a
 *   `hu_onboard_provider_test_input_t *`. The step bypasses stdin
 *   entirely and applies the test input deterministically:
 *     - .choice (1..4 = provider, 'q' = quit) → result code
 *     - .provider_name → written to state if applicable
 *     - .injected_result → final return value
 *
 * API-key handling (Phase 2): the key is read into a stack buffer,
 * passed to smoke-check, then memset() to zero. NEVER persisted to
 * state.provider, NEVER logged. Phase 1 has no key handling because
 * smoke-check isn't wired yet.
 */
#ifndef HU_ONBOARD_STEP_PROVIDER_H
#define HU_ONBOARD_STEP_PROVIDER_H

#include "human/onboard/step.h"

/* Menu choice → which provider the user picked. */
typedef enum hu_onboard_provider_choice {
    HU_PROVIDER_CHOICE_NONE = 0,  /* No selection yet (default) */
    HU_PROVIDER_CHOICE_MLX_LOCAL, /* '1' */
    HU_PROVIDER_CHOICE_ANTHROPIC, /* '2' */
    HU_PROVIDER_CHOICE_GEMINI,    /* '3' */
    HU_PROVIDER_CHOICE_OPENAI,    /* '4' */
    HU_PROVIDER_CHOICE_QUIT,      /* 'q' */
    HU_PROVIDER_CHOICE_INVALID,   /* Anything else */
} hu_onboard_provider_choice_t;

/* Test-injection input. Set as step->user_data to bypass stdin. */
typedef struct hu_onboard_provider_test_input {
    hu_onboard_provider_choice_t choice;
    hu_onboard_step_result_t injected_result;
} hu_onboard_provider_test_input_t;

/* Vtable factory. Returns a static vtable instance; caller doesn't free. */
hu_onboard_step_t *hu_onboard_step_provider_create(void);

/* Pure helpers (testable in isolation) */

/* Classify a single input byte (or '\n' for "enter") into a choice. */
hu_onboard_provider_choice_t hu_onboard_provider_classify_byte(char c);

/* Map a choice to the canonical provider name (the string that gets
 * written to state.provider.provider_name). Returns "" for NONE,
 * INVALID, or QUIT. */
const char *hu_onboard_provider_choice_to_name(hu_onboard_provider_choice_t c);

/* Phase 1 step runner: deterministic on user_data injection. With
 * no injection, returns REPEAT (Phase 2 will add stdin handling). */
hu_onboard_step_result_t hu_onboard_step_provider_run_phase1(hu_onboard_step_t *self,
                                                             hu_onboard_state_t *state);

#endif /* HU_ONBOARD_STEP_PROVIDER_H */
