#ifndef HU_ONBOARD_DISPATCHER_H
#define HU_ONBOARD_DISPATCHER_H

#include "human/core/error.h"
#include "human/onboard/state.h"
#include "human/onboard/step.h"

/**
 * Sprint 51 US-C2.1 PART 2 — Runtime dispatcher for the onboarding wizard.
 *
 * The dispatcher pumps the state machine: it looks up the current step's
 * vtable, calls enter() then run(), persists state atomically after every
 * transition, and applies the run() result code (NEXT/BACK/REPEAT/QUIT/
 * COMPLETE/ABORT).
 *
 * Step table contract:
 * - step_table[id] holds the vtable for step `id` (indexed by hu_onboard_step_id_t).
 * - step_table[HU_ONBOARD_STEP_COMPLETE] is NEVER invoked — the loop exits when
 *   state->current reaches that sentinel.
 * - The caller owns step_table; the dispatcher does not free any vtable.
 *
 * Return codes:
 * - HU_OK when the wizard completes successfully (state->current == COMPLETE)
 *   OR when the user QUITs (state is persisted; caller can resume).
 * - HU_ERR_INTERNAL when a step returns ABORT (unrecoverable).
 * - HU_ERR_INVALID_ARGUMENT on NULL state/config, missing vtable, or out-of-range step_id.
 *
 * The `state_path` is the absolute path where state is persisted after each
 * transition. NULL disables persistence (used by tests that don't need it).
 */

typedef struct hu_onboard_dispatcher_config {
    hu_onboard_step_t *step_table[HU_ONBOARD_STEP_COMPLETE];
    const char *state_path; /* NULL = no persistence (test/dry-run mode) */
} hu_onboard_dispatcher_config_t;

/**
 * Run the dispatcher loop until terminal state.
 *
 * Pre: state has been initialized (hu_onboard_state_init or loaded from disk).
 * Post: state->current is HU_ONBOARD_STEP_COMPLETE on HU_OK, or a non-terminal
 *       step on HU_OK with QUIT, or unchanged on early-error return codes.
 */
hu_error_t hu_onboard_dispatcher_run(hu_onboard_state_t *state,
                                     const hu_onboard_dispatcher_config_t *config);

/**
 * Compute the default next step from `current`.
 * Returns COMPLETE when called on the last non-terminal step (TESTSEND).
 * Returns COMPLETE if called on COMPLETE itself (idempotent terminal).
 *
 * Pure; safe to call from tests without a running dispatcher.
 */
hu_onboard_step_id_t hu_onboard_next_step(hu_onboard_step_id_t current);

#endif /* HU_ONBOARD_DISPATCHER_H */
