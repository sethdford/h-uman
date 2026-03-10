#ifndef HU_TOOLS_DIFF_H
#define HU_TOOLS_DIFF_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"
#include "human/tool.h"

hu_error_t hu_diff_tool_create(hu_allocator_t *alloc, const char *workspace_dir,
                               size_t workspace_dir_len, hu_security_policy_t *policy,
                               hu_tool_t *out);

#endif
