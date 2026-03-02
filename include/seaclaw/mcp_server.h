#ifndef SC_MCP_SERVER_H
#define SC_MCP_SERVER_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include "seaclaw/tool.h"
#include <stddef.h>

typedef struct sc_mcp_host sc_mcp_host_t;

sc_error_t sc_mcp_host_create(sc_allocator_t *alloc,
    sc_tool_t *tools, size_t tool_count,
    sc_memory_t *memory,
    sc_mcp_host_t **out);

sc_error_t sc_mcp_host_run(sc_mcp_host_t *srv);

void sc_mcp_host_destroy(sc_mcp_host_t *srv);

#endif /* SC_MCP_SERVER_H */
