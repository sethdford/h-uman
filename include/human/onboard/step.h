#ifndef HU_ONBOARD_STEP_H
#define HU_ONBOARD_STEP_H

#include "human/onboard/state.h"

/**
 * Return values from a step's run() function.
 * These control the dispatcher's state-machine transitions.
 */
typedef enum hu_onboard_step_result {
    HU_ONBOARD_NEXT = 0,     /* advance to next_default step */
    HU_ONBOARD_BACK = 1,     /* pop step history (previous step) */
    HU_ONBOARD_REPEAT = 2,   /* re-render current step (e.g., validation failed) */
    HU_ONBOARD_QUIT = 3,     /* save state, exit cleanly (resume-able) */
    HU_ONBOARD_COMPLETE = 4, /* terminal — wizard finished successfully */
    HU_ONBOARD_ABORT = 5,    /* unrecoverable — print docs link, exit nonzero */
} hu_onboard_step_result_t;

/**
 * Opaque step vtable.
 * Instantiated by each step (step_welcome.c, step_provider.c, etc.)
 * and registered in the dispatcher's step_table[].
 */
typedef struct hu_onboard_step {
    const char *name;         /* kebab-case, stable identifier */
    const char *display_name; /* human-readable: "Provider setup" */

    /**
     * Render prompt + read user input + apply to state.
     * Returns one of the hu_onboard_step_result_t codes above.
     *
     * Contract: MUST persist its result into state BEFORE returning NEXT
     * (so a crash post-step preserves the answer). Tested by injecting
     * a fault between run() returning and dispatcher transitioning.
     *
     * Network I/O MUST gate on HU_IS_TEST (same discipline as Sprint 50 C3.3).
     */
    hu_onboard_step_result_t (*run)(struct hu_onboard_step *self, hu_onboard_state_t *state);

    /**
     * Optional pre-validation hook called when entering a step.
     * Used to render any saved answers (resume path) or run pre-flight checks.
     * May be NULL if the step has no enter-time setup.
     */
    void (*enter)(struct hu_onboard_step *self, hu_onboard_state_t *state);

    /** Optional user data (e.g., mutable config or test fixtures). */
    void *user_data;
} hu_onboard_step_t;

#endif /* HU_ONBOARD_STEP_H */
