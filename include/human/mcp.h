#ifndef HU_MCP_H
#define HU_MCP_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * MCP CLIENT — connects to an external MCP server child process via stdio
 * pipes and acts as a JSON-RPC client.
 *
 * Historical note: this type used to be called `hu_mcp_server_t`. That name
 * has been freed for the upcoming MCP server-mode initiative (init #12),
 * which will publish a real h-uman-as-server vtable under that symbol.
 * Do NOT reintroduce a typedef for the old name here — it is reserved.
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_mcp_client_config {
    const char *command;
    const char **args;
    size_t args_count;
} hu_mcp_client_config_t;

typedef struct hu_mcp_client hu_mcp_client_t;

hu_mcp_client_t *hu_mcp_client_create(hu_allocator_t *alloc, const hu_mcp_client_config_t *config);
hu_error_t hu_mcp_client_connect(hu_mcp_client_t *client);
hu_error_t hu_mcp_client_list_tools(hu_mcp_client_t *client, hu_allocator_t *alloc,
                                    char ***out_names, char ***out_descriptions, char ***out_params,
                                    size_t *out_count);
hu_error_t hu_mcp_client_call_tool(hu_mcp_client_t *client, hu_allocator_t *alloc,
                                   const char *tool_name, const char *args_json, char **out_result,
                                   size_t *out_result_len);
void hu_mcp_client_destroy(hu_mcp_client_t *client);

/* Reconnect to MCP server (kill + restart child process, re-handshake).
 * Useful when the server process has crashed or become unresponsive. */
hu_error_t hu_mcp_client_reconnect(hu_mcp_client_t *client);

/* Re-discover tools from a connected server (hot-reload after server update).
 * Frees any previously cached tool list and re-issues tools/list. */
hu_error_t hu_mcp_client_refresh_tools(hu_mcp_client_t *client, hu_allocator_t *alloc,
                                       hu_tool_t **out_tools, size_t *out_count);

hu_error_t hu_mcp_init_tools(hu_allocator_t *alloc, const hu_mcp_client_config_t *server_configs,
                             size_t config_count, hu_tool_t **out_tools, size_t *out_count);
void hu_mcp_free_tools(hu_allocator_t *alloc, hu_tool_t *tools, size_t count);

#endif /* HU_MCP_H */
