#ifndef HU_ONBOARD_H
#define HU_ONBOARD_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>

/**
 * Run the interactive setup wizard.
 * Steps: check config exists, prompt for provider, API key, model, write config.
 * Uses stdin/stdout for prompts.
 * In HU_IS_TEST mode, returns HU_OK immediately without prompting.
 */
hu_error_t hu_onboard_run(hu_allocator_t *alloc);

/**
 * Check if this is the first run (no ~/.human/config.json exists).
 */
bool hu_onboard_check_first_run(void);

#endif /* HU_ONBOARD_H */
