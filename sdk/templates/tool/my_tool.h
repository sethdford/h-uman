#ifndef HU_MY_TOOL_H
#define HU_MY_TOOL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/tool.h"
#include <stddef.h>

/**
 * Create a custom tool implementing hu_tool_t.
 *
 * Add to your tool array and pass to hu_agent_from_config:
 *   hu_tool_t tools[N];
 *   hu_my_tool_create(&alloc, &tools[i]);
 *   hu_agent_from_config(&agent, &alloc, provider, tools, N, ...);
 *
 * Or add to src/tools/factory.c in hu_tools_create_default.
 */
hu_error_t hu_my_tool_create(hu_allocator_t *alloc, hu_tool_t *out);

#endif /* HU_MY_TOOL_H */
