#include "human/mcp_manager.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/tool.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * MCP Tool Wrapper Utilities
 *
 * This file provides helper functions for working with MCP tools that have been
 * wrapped as hu_tool_vtable_t instances by the MCP manager. It bridges the
 * gap between the manager's mcp__<server>__<tool> naming convention and
 * callers that need to parse or construct these names.
 */

/* ── Name parsing ─────────────────────────────────────────────────────── */

bool hu_mcp_tool_parse_name(const char *prefixed_name, const char **out_server,
                            size_t *out_server_len, const char **out_tool,
                            size_t *out_tool_len) {
    if (!prefixed_name || !out_server || !out_server_len || !out_tool || !out_tool_len)
        return false;

    /* Must start with "mcp__" */
    if (strncmp(prefixed_name, "mcp__", 5) != 0)
        return false;

    const char *server_start = prefixed_name + 5;
    const char *sep = strstr(server_start, "__");
    if (!sep || sep == server_start)
        return false;

    *out_server = server_start;
    *out_server_len = (size_t)(sep - server_start);
    *out_tool = sep + 2;
    *out_tool_len = strlen(sep + 2);
    return (*out_tool_len > 0);
}

hu_error_t hu_mcp_tool_build_name(hu_allocator_t *alloc, const char *server_name,
                                  const char *tool_name, char **out, size_t *out_len) {
    if (!alloc || !server_name || !tool_name || !out)
        return HU_ERR_INVALID_ARGUMENT;

    /* "mcp__" + server_name + "__" + tool_name + NUL */
    size_t slen = strlen(server_name);
    size_t tlen = strlen(tool_name);
    size_t total = 5 + slen + 2 + tlen;

    char *buf = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    memcpy(buf, "mcp__", 5);
    memcpy(buf + 5, server_name, slen);
    memcpy(buf + 5 + slen, "__", 2);
    memcpy(buf + 5 + slen + 2, tool_name, tlen);
    buf[total] = '\0';

    *out = buf;
    if (out_len)
        *out_len = total;
    return HU_OK;
}

bool hu_mcp_tool_is_mcp(const char *tool_name) {
    return tool_name && strncmp(tool_name, "mcp__", 5) == 0;
}
