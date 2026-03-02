#ifndef SC_TOOLS_CRON_UPDATE_H
#define SC_TOOLS_CRON_UPDATE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"

sc_error_t sc_cron_update_create(sc_allocator_t *alloc, sc_tool_t *out);

#endif
