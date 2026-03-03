#ifndef SC_TOOLS_NOTEBOOK_H
#define SC_TOOLS_NOTEBOOK_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/security.h"
#include "seaclaw/tool.h"

sc_error_t sc_notebook_create(sc_allocator_t *alloc,
    sc_security_policy_t *policy, sc_tool_t *out);

#endif
