#ifndef SC_TOOLS_SCREENSHOT_H
#define SC_TOOLS_SCREENSHOT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/security.h"
#include "seaclaw/tool.h"
#include <stdbool.h>
#include <stddef.h>

sc_error_t sc_screenshot_create(sc_allocator_t *alloc, bool enabled,
    sc_security_policy_t *policy, sc_tool_t *out);

#endif
