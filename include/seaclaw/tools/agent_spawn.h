#ifndef SC_TOOLS_AGENT_SPAWN_H
#define SC_TOOLS_AGENT_SPAWN_H

#include "seaclaw/agent/spawn.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/tool.h"

sc_error_t sc_agent_spawn_tool_create(sc_allocator_t *alloc, sc_agent_pool_t *pool, sc_tool_t *out);

#endif
