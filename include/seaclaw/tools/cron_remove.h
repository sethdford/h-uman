#ifndef SC_TOOLS_CRON_REMOVE_H
#define SC_TOOLS_CRON_REMOVE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/cron.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_cron_remove_create(sc_allocator_t *alloc, sc_cron_scheduler_t *sched, sc_tool_t *out);

#endif
