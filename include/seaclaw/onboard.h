#ifndef SC_ONBOARD_H
#define SC_ONBOARD_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>

/**
 * Run the interactive setup wizard.
 * Steps: check config exists, prompt for provider, API key, model, write config.
 * Uses stdin/stdout for prompts.
 * In SC_IS_TEST mode, returns SC_OK immediately without prompting.
 */
sc_error_t sc_onboard_run(sc_allocator_t *alloc);

/**
 * Check if this is the first run (no ~/.seaclaw/config.json exists).
 */
bool sc_onboard_check_first_run(void);

#endif /* SC_ONBOARD_H */
