#ifndef SC_TOOLS_DELEGATE_H
#define SC_TOOLS_DELEGATE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_delegate_create(sc_allocator_t *alloc, sc_tool_t *out);

#endif
