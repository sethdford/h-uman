#ifndef SC_MCP_H
#define SC_MCP_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/tool.h"
#include <stddef.h>
#include <stdbool.h>

typedef struct sc_mcp_server_config {
    const char *command;
    const char **args;
    size_t args_count;
} sc_mcp_server_config_t;

typedef struct sc_mcp_server sc_mcp_server_t;

sc_mcp_server_t *sc_mcp_server_create(sc_allocator_t *alloc, const sc_mcp_server_config_t *config);
sc_error_t sc_mcp_server_connect(sc_mcp_server_t *srv);
sc_error_t sc_mcp_server_list_tools(sc_mcp_server_t *srv, sc_allocator_t *alloc,
    char ***out_names, char ***out_descriptions, size_t *out_count);
sc_error_t sc_mcp_server_call_tool(sc_mcp_server_t *srv, sc_allocator_t *alloc,
    const char *tool_name, const char *args_json,
    char **out_result, size_t *out_result_len);
void sc_mcp_server_destroy(sc_mcp_server_t *srv);

sc_error_t sc_mcp_init_tools(sc_allocator_t *alloc,
    const sc_mcp_server_config_t *server_configs, size_t config_count,
    sc_tool_t **out_tools, size_t *out_count);
void sc_mcp_free_tools(sc_allocator_t *alloc, sc_tool_t *tools, size_t count);

#endif /* SC_MCP_H */
