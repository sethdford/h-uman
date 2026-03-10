#ifndef HU_TOOLS_BROWSER_OPEN_H
#define HU_TOOLS_BROWSER_OPEN_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_browser_open_create(hu_allocator_t *alloc, const char *const *allowed_domains,
                                  size_t allowed_count, hu_security_policy_t *policy,
                                  hu_tool_t *out);

#endif
