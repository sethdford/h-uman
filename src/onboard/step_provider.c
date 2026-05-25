/* src/onboard/step_provider.c
 *
 * Sprint 54 US-C2.3 (Phase 1) — Provider setup onboarding step.
 *
 * Phase 1 scope: vtable + menu classification + state persistence +
 * test-injection. Phase 2 (deferred) adds the real stdin/key prompt
 * and the smoke-check call.
 *
 * State write contract:
 *   - For a valid provider choice, write the canonical name to
 *     state->provider.provider_name BEFORE returning NEXT.
 *   - Set state->provider.provider_smoke_passed = false in Phase 1;
 *     Phase 2 sets it true only after the smoke-check returns PASS.
 */

#include "human/onboard/step_provider.h"

#include "human/onboard/state.h"
#include "human/onboard/step.h"

#include <stddef.h>
#include <string.h>

/* ── Pure helpers (testable in isolation) ─────────────────────────── */

hu_onboard_provider_choice_t hu_onboard_provider_classify_byte(char c) {
    switch (c) {
    case '1':
        return HU_PROVIDER_CHOICE_MLX_LOCAL;
    case '2':
        return HU_PROVIDER_CHOICE_ANTHROPIC;
    case '3':
        return HU_PROVIDER_CHOICE_GEMINI;
    case '4':
        return HU_PROVIDER_CHOICE_OPENAI;
    case 'q':
    case 'Q':
        return HU_PROVIDER_CHOICE_QUIT;
    default:
        return HU_PROVIDER_CHOICE_INVALID;
    }
}

const char *hu_onboard_provider_choice_to_name(hu_onboard_provider_choice_t c) {
    switch (c) {
    case HU_PROVIDER_CHOICE_MLX_LOCAL:
        return "mlx_local";
    case HU_PROVIDER_CHOICE_ANTHROPIC:
        return "anthropic";
    case HU_PROVIDER_CHOICE_GEMINI:
        return "gemini";
    case HU_PROVIDER_CHOICE_OPENAI:
        return "openai";
    case HU_PROVIDER_CHOICE_NONE:
    case HU_PROVIDER_CHOICE_QUIT:
    case HU_PROVIDER_CHOICE_INVALID:
    default:
        return "";
    }
}

/* Apply a test-injected choice to state and return the injected result.
 * Pure modulo the state write.
 *
 * Encapsulates the state-persistence contract: write the provider name
 * to state BEFORE returning the result so a crash mid-flow preserves
 * the user's choice (crash-safety pattern from welcome step). */
static hu_onboard_step_result_t apply_injected_input(const hu_onboard_provider_test_input_t *inj,
                                                     hu_onboard_state_t *state) {
    if (inj->choice >= HU_PROVIDER_CHOICE_MLX_LOCAL && inj->choice <= HU_PROVIDER_CHOICE_OPENAI) {
        const char *name = hu_onboard_provider_choice_to_name(inj->choice);
        if (state && name && name[0]) {
            /* Persist BEFORE returning so a crash post-step preserves
             * the user's selection. NUL-terminate explicitly. */
            size_t cap = sizeof(state->provider.provider_name);
            size_t n = strlen(name);
            if (n >= cap)
                n = cap - 1;
            memcpy(state->provider.provider_name, name, n);
            state->provider.provider_name[n] = '\0';
            /* Phase 1: smoke-check isn't wired yet, so we leave
             * provider_smoke_passed = false (its init value). Phase 2
             * will flip this to true after PASS. */
            state->provider.provider_smoke_passed = false;
        }
    }
    return inj->injected_result;
}

hu_onboard_step_result_t hu_onboard_step_provider_run_phase1(hu_onboard_step_t *self,
                                                             hu_onboard_state_t *state) {
    /* Test injection: if user_data is set, treat it as the test input
     * struct. The Phase 1 step doesn't read stdin without injection;
     * Phase 2 adds the real stdin read loop. */
    if (self && self->user_data) {
        const hu_onboard_provider_test_input_t *inj =
            (const hu_onboard_provider_test_input_t *)self->user_data;
        return apply_injected_input(inj, state);
    }

    /* No injection in Phase 1 → cannot proceed without stdin handling
     * that's deferred to Phase 2. REPEAT to signal "step ran but
     * nothing happened" without advancing or aborting. */
    return HU_ONBOARD_REPEAT;
}

/* ── vtable wrapper ───────────────────────────────────────────────── */

static hu_onboard_step_result_t step_provider_run(hu_onboard_step_t *self,
                                                  hu_onboard_state_t *state) {
    return hu_onboard_step_provider_run_phase1(self, state);
}

hu_onboard_step_t *hu_onboard_step_provider_create(void) {
    static hu_onboard_step_t step = {
        .name = "provider",
        .display_name = "Provider setup",
        .run = step_provider_run,
        .enter = NULL,
        .user_data = NULL,
    };
    return &step;
}
