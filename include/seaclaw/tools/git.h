#ifndef SC_TOOLS_GIT_H
#define SC_TOOLS_GIT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/security.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_git_create(sc_allocator_t *alloc,
    const char *workspace_dir, size_t workspace_dir_len,
    sc_security_policy_t *policy, sc_tool_t *out);

#endif
