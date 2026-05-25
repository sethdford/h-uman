/* src/onboard/dispatcher.c
 *
 * Sprint 51 US-C2.1 PART 2 — runtime dispatcher for the onboarding wizard.
 *
 * On every loop iteration:
 *   1. Look up the current step's vtable in step_table[].
 *   2. Call step->enter() if non-NULL (resume rendering / pre-flight).
 *   3. Call step->run() to get a hu_onboard_step_result_t.
 *   4. Atomically save state to disk (if state_path != NULL).
 *   5. Apply the result code: NEXT advances + pushes history;
 *      BACK pops history; REPEAT no-ops; QUIT/COMPLETE/ABORT exit.
 *
 * The save in step 4 happens AFTER run() returns but BEFORE the state
 * transition — so a crash between save and the next iteration loses at
 * most one step's worth of in-flight transition state, not the user's
 * answer. The save honors the architecture.md atomic-save contract via
 * hu_onboard_state_save (tmp + fwrite + fflush + fsync + rename).
 */

#include "human/onboard/dispatcher.h"

#include "human/core/error.h"
#include "human/onboard/state.h"
#include "human/onboard/step.h"

#include <stddef.h>

/* ── Pure helpers ─────────────────────────────────────────────────── */

hu_onboard_step_id_t hu_onboard_next_step(hu_onboard_step_id_t current) {
    switch (current) {
    case HU_ONBOARD_STEP_WELCOME:
        return HU_ONBOARD_STEP_PROVIDER;
    case HU_ONBOARD_STEP_PROVIDER:
        return HU_ONBOARD_STEP_PERSONA;
    case HU_ONBOARD_STEP_PERSONA:
        return HU_ONBOARD_STEP_CHANNELS;
    case HU_ONBOARD_STEP_CHANNELS:
        return HU_ONBOARD_STEP_TESTSEND;
    case HU_ONBOARD_STEP_TESTSEND:
        return HU_ONBOARD_STEP_COMPLETE;
    case HU_ONBOARD_STEP_COMPLETE:
    default:
        return HU_ONBOARD_STEP_COMPLETE;
    }
}

/* Push current onto history stack. Bounded at 10 (struct invariant).
 * Full → slide left and append, dropping the oldest entry. */
static void history_push(hu_onboard_state_t *state, hu_onboard_step_id_t step) {
    const size_t cap = sizeof(state->history) / sizeof(state->history[0]);
    if (state->history_depth < cap) {
        state->history[state->history_depth++] = step;
        return;
    }
    for (size_t i = 1; i < cap; i++) {
        state->history[i - 1] = state->history[i];
    }
    state->history[cap - 1] = step;
}

/* Pop top of history. Returns current step unchanged if empty (BACK from
 * step 0 is a no-op, not an error). */
static hu_onboard_step_id_t history_pop(hu_onboard_state_t *state) {
    if (state->history_depth == 0) {
        return state->current;
    }
    state->history_depth--;
    return state->history[state->history_depth];
}

/* ── Dispatcher loop ──────────────────────────────────────────────── */

hu_error_t hu_onboard_dispatcher_run(hu_onboard_state_t *state,
                                     const hu_onboard_dispatcher_config_t *config) {
    if (!state || !config) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Bounded iteration cap as a safety net against runaway REPEAT loops. */
    const int max_iterations = 200;
    int iterations = 0;

    while (state->current != HU_ONBOARD_STEP_COMPLETE) {
        if (iterations++ >= max_iterations) {
            return HU_ERR_INTERNAL;
        }

        if ((int)state->current < 0 || state->current >= HU_ONBOARD_STEP_COMPLETE) {
            return HU_ERR_INVALID_ARGUMENT;
        }

        hu_onboard_step_t *step = config->step_table[state->current];
        if (!step || !step->run) {
            return HU_ERR_INVALID_ARGUMENT;
        }

        if (step->enter) {
            step->enter(step, state);
        }

        hu_onboard_step_result_t result = step->run(step, state);

        if (config->state_path) {
            hu_error_t save_err = hu_onboard_state_save(state, config->state_path);
            if (save_err != HU_OK && result != HU_ONBOARD_REPEAT) {
                return save_err;
            }
        }

        switch (result) {
        case HU_ONBOARD_NEXT:
            history_push(state, state->current);
            state->current = hu_onboard_next_step(state->current);
            break;
        case HU_ONBOARD_BACK:
            state->current = history_pop(state);
            break;
        case HU_ONBOARD_REPEAT:
            /* Stay on current step. */
            break;
        case HU_ONBOARD_QUIT:
            return HU_OK;
        case HU_ONBOARD_COMPLETE:
            state->current = HU_ONBOARD_STEP_COMPLETE;
            break;
        case HU_ONBOARD_ABORT:
        default:
            return HU_ERR_INTERNAL;
        }
    }

    return HU_OK;
}
