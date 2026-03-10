#ifndef HU_TOOLS_AGENT_QUERY_H
#define HU_TOOLS_AGENT_QUERY_H

#include "human/agent/spawn.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"

hu_error_t hu_agent_query_tool_create(hu_allocator_t *alloc, hu_agent_pool_t *pool, hu_tool_t *out);

#endif
