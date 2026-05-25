#ifndef HU_ONBOARD_STEP_WELCOME_H
#define HU_ONBOARD_STEP_WELCOME_H

#include "human/onboard/step.h"

/**
 * Sprint 51 US-C2.2 — Welcome step.
 *
 * Returns a vtable for the first onboarding step: prints the privacy
 * explanation from docs/copy/onboarding-step1.md, then waits for the
 * user to press Enter/`n` (NEXT) or `q` (QUIT).
 *
 * Test mode (HU_IS_TEST defined OR step->user_data set to non-NULL):
 * stdin is NOT read. Instead `user_data` is interpreted as a
 * `hu_onboard_step_result_t *` and the step returns *user_data.
 * This lets tests inject deterministic results without TTY.
 *
 * The returned step is statically allocated within this TU; the caller
 * does NOT free it. Multiple calls return the same instance.
 */
hu_onboard_step_t *hu_onboard_step_welcome_create(void);

/* Internal — exposed for the test harness only. Returns the path the
 * step will read at run() time. Defaults to
 * "docs/copy/onboarding-step1.md" relative to CWD; can be overridden
 * for tests via hu_onboard_step_welcome_set_copy_path. */
const char *hu_onboard_step_welcome_copy_path(void);
void hu_onboard_step_welcome_set_copy_path(const char *path);

#endif /* HU_ONBOARD_STEP_WELCOME_H */
