#ifndef HU_MCP_SERVER_H
#define HU_MCP_SERVER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/tool.h"
#include <stddef.h>

typedef struct hu_mcp_host hu_mcp_host_t;

hu_error_t hu_mcp_host_create(hu_allocator_t *alloc, hu_tool_t *tools, size_t tool_count,
                              hu_memory_t *memory, hu_mcp_host_t **out);

hu_error_t hu_mcp_host_run(hu_mcp_host_t *srv);

void hu_mcp_host_destroy(hu_mcp_host_t *srv);

#endif /* HU_MCP_SERVER_H */
