#ifndef HU_TOOLS_NOTEBOOK_H
#define HU_TOOLS_NOTEBOOK_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"
#include "human/tool.h"

hu_error_t hu_notebook_create(hu_allocator_t *alloc, hu_security_policy_t *policy, hu_tool_t *out);

#endif
