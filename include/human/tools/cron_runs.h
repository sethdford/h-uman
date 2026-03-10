#ifndef HU_TOOLS_CRON_RUNS_H
#define HU_TOOLS_CRON_RUNS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/cron.h"
#include "human/tool.h"

hu_error_t hu_cron_runs_create(hu_allocator_t *alloc, hu_cron_scheduler_t *sched, hu_tool_t *out);

#endif
