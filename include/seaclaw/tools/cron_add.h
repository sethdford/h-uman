#ifndef SC_TOOLS_CRON_ADD_H
#define SC_TOOLS_CRON_ADD_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_cron_add_create(sc_allocator_t *alloc, sc_tool_t *out);

#endif
