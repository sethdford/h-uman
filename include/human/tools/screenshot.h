#ifndef HU_TOOLS_SCREENSHOT_H
#define HU_TOOLS_SCREENSHOT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>

hu_error_t hu_screenshot_create(hu_allocator_t *alloc, bool enabled, hu_security_policy_t *policy,
                                hu_tool_t *out);

#endif
