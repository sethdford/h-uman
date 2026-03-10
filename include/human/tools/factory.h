#ifndef HU_TOOLS_FACTORY_H
#define HU_TOOLS_FACTORY_H

#include "human/agent/mailbox.h"
#include "human/agent/spawn.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/cron.h"
#include "human/memory.h"
#include "human/security.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_tools_create_default(hu_allocator_t *alloc, const char *workspace_dir,
                                   size_t workspace_dir_len, hu_security_policy_t *policy,
                                   const hu_config_t *config, hu_memory_t *memory,
                                   hu_cron_scheduler_t *cron, hu_agent_pool_t *agent_pool,
                                   hu_mailbox_t *mailbox, hu_tool_t **out_tools, size_t *out_count);

void hu_tools_destroy_default(hu_allocator_t *alloc, hu_tool_t *tools, size_t count);

#endif /* HU_TOOLS_FACTORY_H */
