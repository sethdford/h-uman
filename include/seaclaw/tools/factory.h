#ifndef SC_TOOLS_FACTORY_H
#define SC_TOOLS_FACTORY_H

#include "seaclaw/agent/spawn.h"
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/cron.h"
#include "seaclaw/memory.h"
#include "seaclaw/security.h"
#include "seaclaw/tool.h"
#include <stddef.h>

sc_error_t sc_tools_create_default(sc_allocator_t *alloc, const char *workspace_dir,
                                   size_t workspace_dir_len, sc_security_policy_t *policy,
                                   const sc_config_t *config, sc_memory_t *memory,
                                   sc_cron_scheduler_t *cron, sc_agent_pool_t *agent_pool,
                                   sc_tool_t **out_tools, size_t *out_count);

void sc_tools_destroy_default(sc_allocator_t *alloc, sc_tool_t *tools, size_t count);

#endif /* SC_TOOLS_FACTORY_H */
