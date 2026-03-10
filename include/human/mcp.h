#ifndef HU_MCP_H
#define HU_MCP_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct hu_mcp_server_config {
    const char *command;
    const char **args;
    size_t args_count;
} hu_mcp_server_config_t;

typedef struct hu_mcp_server hu_mcp_server_t;

hu_mcp_server_t *hu_mcp_server_create(hu_allocator_t *alloc, const hu_mcp_server_config_t *config);
hu_error_t hu_mcp_server_connect(hu_mcp_server_t *srv);
hu_error_t hu_mcp_server_list_tools(hu_mcp_server_t *srv, hu_allocator_t *alloc, char ***out_names,
                                    char ***out_descriptions, char ***out_params,
                                    size_t *out_count);
hu_error_t hu_mcp_server_call_tool(hu_mcp_server_t *srv, hu_allocator_t *alloc,
                                   const char *tool_name, const char *args_json, char **out_result,
                                   size_t *out_result_len);
void hu_mcp_server_destroy(hu_mcp_server_t *srv);

hu_error_t hu_mcp_init_tools(hu_allocator_t *alloc, const hu_mcp_server_config_t *server_configs,
                             size_t config_count, hu_tool_t **out_tools, size_t *out_count);
void hu_mcp_free_tools(hu_allocator_t *alloc, hu_tool_t *tools, size_t count);

#endif /* HU_MCP_H */
