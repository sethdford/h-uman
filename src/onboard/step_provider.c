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
#include <stdio.h>
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

/* ── Sprint 55 Phase 2 — real stdin path ──────────────────────────── */

/* Classify the first non-whitespace byte of a stdin line into a choice.
 * Pure function; tested via the step's vtable run() path with
 * fmemopen-redirected stdin. */
static hu_onboard_provider_choice_t classify_input_line(const char *line) {
    if (!line)
        return HU_PROVIDER_CHOICE_INVALID;
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '\0' || *line == '\n')
        return HU_PROVIDER_CHOICE_INVALID; /* empty line — re-prompt */
    return hu_onboard_provider_classify_byte(*line);
}

/* Persist a valid provider choice to state. Mirrors the test-injection
 * persist path so behavior is identical regardless of whether the
 * choice came from stdin or from user_data injection. */
static void persist_choice(hu_onboard_provider_choice_t choice, hu_onboard_state_t *state) {
    if (choice < HU_PROVIDER_CHOICE_MLX_LOCAL || choice > HU_PROVIDER_CHOICE_OPENAI)
        return;
    if (!state)
        return;
    const char *name = hu_onboard_provider_choice_to_name(choice);
    if (!name || !name[0])
        return;
    size_t cap = sizeof(state->provider.provider_name);
    size_t n = strlen(name);
    if (n >= cap)
        n = cap - 1;
    memcpy(state->provider.provider_name, name, n);
    state->provider.provider_name[n] = '\0';
    /* Phase 2 deferred: provider_smoke_passed flips true only when
     * the smoke-check (US-C3.3 Phase 2) is invoked here. That requires
     * threading (alloc, cfg) through the step vtable, which is a
     * non-trivial API expansion. Phase 3 will add it. Leave false. */
    state->provider.provider_smoke_passed = false;
}

/* Phase 2 step runner — production stdin path. Test path still goes
 * through user_data injection (see step_provider_run vtable wrapper). */
hu_onboard_step_result_t hu_onboard_step_provider_run(hu_onboard_step_t *self,
                                                      hu_onboard_state_t *state) {
    /* Test injection short-circuit (unchanged from Phase 1). */
    if (self && self->user_data) {
        const hu_onboard_provider_test_input_t *inj =
            (const hu_onboard_provider_test_input_t *)self->user_data;
        return apply_injected_input(inj, state);
    }

    /* Render the menu. Display copy is inline; future stories may move
     * to a copy file once we have more than one onboarding step that
     * needs externalized strings. */
    fputs("\n  Choose your AI provider:\n\n", stdout);
    fputs("    1) Local MLX  (on-device, requires Apple Silicon)\n", stdout);
    fputs("    2) Anthropic  (cloud — requires API key)\n", stdout);
    fputs("    3) Gemini     (cloud — requires API key)\n", stdout);
    fputs("    4) OpenAI     (cloud — requires API key)\n", stdout);
    fputs("    q) Quit\n\n  > ", stdout);
    fflush(stdout);

    char line[64];
    if (!fgets(line, sizeof(line), stdin)) {
        /* EOF or read error → clean quit. */
        return HU_ONBOARD_QUIT;
    }

    hu_onboard_provider_choice_t choice = classify_input_line(line);
    switch (choice) {
    case HU_PROVIDER_CHOICE_QUIT:
        return HU_ONBOARD_QUIT;
    case HU_PROVIDER_CHOICE_MLX_LOCAL:
    case HU_PROVIDER_CHOICE_ANTHROPIC:
    case HU_PROVIDER_CHOICE_GEMINI:
    case HU_PROVIDER_CHOICE_OPENAI:
        /* Persist BEFORE returning so a crash post-step preserves the
         * user's selection (state-persistence-before-return pattern from
         * Sprint 54 US-C2.2). */
        persist_choice(choice, state);
        return HU_ONBOARD_NEXT;
    case HU_PROVIDER_CHOICE_INVALID:
    case HU_PROVIDER_CHOICE_NONE:
    default:
        fputs("  Invalid choice. Pick 1, 2, 3, 4, or q.\n", stdout);
        return HU_ONBOARD_REPEAT;
    }
}

/* ── vtable wrapper ───────────────────────────────────────────────── */

static hu_onboard_step_result_t step_provider_run(hu_onboard_step_t *self,
                                                  hu_onboard_state_t *state) {
    return hu_onboard_step_provider_run(self, state);
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
