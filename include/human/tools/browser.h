#ifndef HU_TOOLS_BROWSER_H
#define HU_TOOLS_BROWSER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>

hu_error_t hu_browser_create(hu_allocator_t *alloc, bool enabled, hu_security_policy_t *policy,
                             hu_tool_t *out);

#endif
