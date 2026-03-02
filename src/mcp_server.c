#include "seaclaw/mcp_server.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define MCP_SERVER_NAME "seaclaw"
#define MCP_SERVER_VERSION "0.1.0"
#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_LINE_BUF_INIT 4096

struct sc_mcp_host {
    sc_allocator_t *alloc;
    sc_tool_t *tools;
    size_t tool_count;
    bool initialized;
};

sc_error_t sc_mcp_host_create(sc_allocator_t *alloc,
    sc_tool_t *tools, size_t tool_count,
    sc_mcp_host_t **out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_mcp_host_t *srv = (sc_mcp_host_t *)alloc->alloc(
        alloc->ctx, sizeof(*srv));
    if (!srv) return SC_ERR_OUT_OF_MEMORY;
    srv->alloc = alloc;
    srv->tools = tools;
    srv->tool_count = tool_count;
    srv->initialized = false;
    *out = srv;
    return SC_OK;
}

/* ── JSON-RPC response helpers ─────────────────────────────────────────── */

static sc_error_t write_response(sc_allocator_t *alloc,
    const char *id_raw, const char *result_json)
{
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) return SC_ERR_OUT_OF_MEMORY;

    sc_error_t err = SC_OK;
    err = sc_json_buf_append_raw(&buf, "{\"jsonrpc\":\"2.0\",\"id\":", 22);
    if (err == SC_OK) err = sc_json_buf_append_raw(&buf, id_raw, strlen(id_raw));
    if (err == SC_OK) err = sc_json_buf_append_raw(&buf, ",\"result\":", 10);
    if (err == SC_OK) err = sc_json_buf_append_raw(&buf, result_json, strlen(result_json));
    if (err == SC_OK) err = sc_json_buf_append_raw(&buf, "}\n", 2);
    if (err != SC_OK) { sc_json_buf_free(&buf); return err; }

    fwrite(buf.ptr, 1, buf.len, stdout);
    fflush(stdout);
    sc_json_buf_free(&buf);
    return SC_OK;
}

static sc_error_t write_error(sc_allocator_t *alloc,
    const char *id_raw, int code, const char *message)
{
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) return SC_ERR_OUT_OF_MEMORY;

    char code_buf[16];
    snprintf(code_buf, sizeof(code_buf), "%d", code);

    sc_error_t err = SC_OK;
    err = sc_json_buf_append_raw(&buf, "{\"jsonrpc\":\"2.0\",\"id\":", 22);
    if (err == SC_OK) err = sc_json_buf_append_raw(&buf, id_raw, strlen(id_raw));
    if (err == SC_OK) err = sc_json_buf_append_raw(&buf, ",\"error\":{\"code\":", 17);
    if (err == SC_OK) err = sc_json_buf_append_raw(&buf, code_buf, strlen(code_buf));
    if (err == SC_OK) err = sc_json_buf_append_raw(&buf, ",\"message\":", 11);
    if (err == SC_OK) err = sc_json_append_string(&buf, message, strlen(message));
    if (err == SC_OK) err = sc_json_buf_append_raw(&buf, "}}\n", 3);
    if (err != SC_OK) { sc_json_buf_free(&buf); return err; }

    fwrite(buf.ptr, 1, buf.len, stdout);
    fflush(stdout);
    sc_json_buf_free(&buf);
    return SC_OK;
}

/* ── Handler: initialize ───────────────────────────────────────────────── */

static sc_error_t handle_initialize(sc_mcp_host_t *srv, const char *id_raw)
{
    srv->initialized = true;
    const char *result =
        "{\"protocolVersion\":\"" MCP_PROTOCOL_VERSION "\","
        "\"capabilities\":{\"tools\":{}},"
        "\"serverInfo\":{\"name\":\"" MCP_SERVER_NAME "\","
        "\"version\":\"" MCP_SERVER_VERSION "\"}}";
    return write_response(srv->alloc, id_raw, result);
}

/* ── Handler: tools/list ───────────────────────────────────────────────── */

static sc_error_t handle_tools_list(sc_mcp_host_t *srv, const char *id_raw)
{
    sc_allocator_t *alloc = srv->alloc;
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) return SC_ERR_OUT_OF_MEMORY;

    sc_error_t err = sc_json_buf_append_raw(&buf, "{\"tools\":[", 10);
    for (size_t i = 0; i < srv->tool_count && err == SC_OK; i++) {
        if (i > 0) err = sc_json_buf_append_raw(&buf, ",", 1);
        if (err != SC_OK) break;

        const char *name = srv->tools[i].vtable->name
            ? srv->tools[i].vtable->name(srv->tools[i].ctx) : "";
        const char *desc = srv->tools[i].vtable->description
            ? srv->tools[i].vtable->description(srv->tools[i].ctx) : "";
        const char *params = srv->tools[i].vtable->parameters_json
            ? srv->tools[i].vtable->parameters_json(srv->tools[i].ctx) : "{}";

        err = sc_json_buf_append_raw(&buf, "{\"name\":", 8);
        if (err == SC_OK) err = sc_json_append_string(&buf, name, strlen(name));
        if (err == SC_OK) err = sc_json_buf_append_raw(&buf, ",\"description\":", 15);
        if (err == SC_OK) err = sc_json_append_string(&buf, desc, strlen(desc));
        if (err == SC_OK) err = sc_json_buf_append_raw(&buf, ",\"inputSchema\":", 15);
        if (err == SC_OK) err = sc_json_buf_append_raw(&buf, params, strlen(params));
        if (err == SC_OK) err = sc_json_buf_append_raw(&buf, "}", 1);
    }
    if (err == SC_OK) err = sc_json_buf_append_raw(&buf, "]}", 2);

    if (err != SC_OK) { sc_json_buf_free(&buf); return err; }

    /* Null-terminate for write_response */
    size_t result_len = buf.len;
    char *result = (char *)alloc->alloc(alloc->ctx, result_len + 1);
    if (!result) { sc_json_buf_free(&buf); return SC_ERR_OUT_OF_MEMORY; }
    memcpy(result, buf.ptr, result_len);
    result[result_len] = '\0';
    sc_json_buf_free(&buf);

    err = write_response(alloc, id_raw, result);
    alloc->free(alloc->ctx, result, result_len + 1);
    return err;
}

/* ── Handler: tools/call ───────────────────────────────────────────────── */

static sc_error_t handle_tools_call(sc_mcp_host_t *srv,
    const char *id_raw, sc_json_value_t *params)
{
    sc_allocator_t *alloc = srv->alloc;

    const char *tool_name = sc_json_get_string(params, "name");
    if (!tool_name || strlen(tool_name) == 0)
        return write_error(alloc, id_raw, -32602, "Missing tool name");

    sc_tool_t *tool = NULL;
    for (size_t i = 0; i < srv->tool_count; i++) {
        const char *n = srv->tools[i].vtable->name
            ? srv->tools[i].vtable->name(srv->tools[i].ctx) : "";
        if (strcmp(n, tool_name) == 0) { tool = &srv->tools[i]; break; }
    }
    if (!tool)
        return write_error(alloc, id_raw, -32602, "Unknown tool");

    sc_json_value_t *args = sc_json_object_get(params, "arguments");

    sc_tool_result_t result = {0};
    sc_error_t err = tool->vtable->execute(tool->ctx, alloc, args, &result);
    if (err != SC_OK) {
        sc_tool_result_free(alloc, &result);
        return write_error(alloc, id_raw, -32603, "Tool execution failed");
    }

    /* Build MCP tool result: {content:[{type:"text",text:"..."}], isError:bool} */
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        sc_tool_result_free(alloc, &result);
        return SC_ERR_OUT_OF_MEMORY;
    }

    const char *text = result.success
        ? (result.output ? result.output : "")
        : (result.error_msg ? result.error_msg : "error");
    size_t text_len = result.success ? result.output_len : result.error_msg_len;

    err = sc_json_buf_append_raw(&buf, "{\"content\":[{\"type\":\"text\",\"text\":", 34);
    if (err == SC_OK) err = sc_json_append_string(&buf, text, text_len);
    if (err == SC_OK) {
        if (result.success)
            err = sc_json_buf_append_raw(&buf, "}]}", 3);
        else
            err = sc_json_buf_append_raw(&buf, "}],\"isError\":true}", 18);
    }

    sc_tool_result_free(alloc, &result);

    if (err != SC_OK) { sc_json_buf_free(&buf); return err; }

    char *res_str = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!res_str) { sc_json_buf_free(&buf); return SC_ERR_OUT_OF_MEMORY; }
    memcpy(res_str, buf.ptr, buf.len);
    res_str[buf.len] = '\0';
    size_t res_len = buf.len;
    sc_json_buf_free(&buf);

    err = write_response(alloc, id_raw, res_str);
    alloc->free(alloc->ctx, res_str, res_len + 1);
    return err;
}

/* ── Main loop ─────────────────────────────────────────────────────────── */

static sc_error_t read_stdin_line(sc_allocator_t *alloc, char **out,
    size_t *out_len, size_t *out_alloc_size)
{
    size_t cap = MCP_LINE_BUF_INIT;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
    size_t len = 0;

    int ch;
    while ((ch = fgetc(stdin)) != EOF) {
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (len + 1 >= cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nb) { alloc->free(alloc->ctx, buf, cap); return SC_ERR_OUT_OF_MEMORY; }
            buf = nb;
            cap = new_cap;
        }
        buf[len++] = (char)ch;
    }

    if (ch == EOF && len == 0) {
        alloc->free(alloc->ctx, buf, cap);
        return SC_ERR_IO;
    }

    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    *out_alloc_size = cap;
    return SC_OK;
}

sc_error_t sc_mcp_host_run(sc_mcp_host_t *srv)
{
    if (!srv) return SC_ERR_INVALID_ARGUMENT;
    sc_allocator_t *alloc = srv->alloc;

    for (;;) {
        char *line = NULL;
        size_t line_len = 0;
        size_t line_alloc = 0;
        sc_error_t err = read_stdin_line(alloc, &line, &line_len, &line_alloc);
        if (err == SC_ERR_IO) return SC_OK; /* EOF — clean shutdown */
        if (err != SC_OK) return err;
        if (line_len == 0) {
            alloc->free(alloc->ctx, line, line_alloc);
            continue;
        }

        sc_json_value_t *req = NULL;
        err = sc_json_parse(alloc, line, line_len, &req);
        alloc->free(alloc->ctx, line, line_alloc);

        if (err != SC_OK || !req) continue; /* skip malformed input */

        const char *method = sc_json_get_string(req, "method");
        sc_json_value_t *id_val = sc_json_object_get(req, "id");
        sc_json_value_t *params = sc_json_object_get(req, "params");
        bool is_notification = (id_val == NULL);

        /* Serialize id for response */
        char id_buf[64] = "null";
        if (id_val) {
            if (id_val->type == SC_JSON_NUMBER) {
                snprintf(id_buf, sizeof(id_buf), "%d", (int)id_val->data.number);
            } else if (id_val->type == SC_JSON_STRING) {
                snprintf(id_buf, sizeof(id_buf), "\"%.*s\"",
                    (int)(id_val->data.string.len < 50 ? id_val->data.string.len : 50),
                    id_val->data.string.ptr);
            }
        }

        if (!method) {
            if (!is_notification)
                write_error(alloc, id_buf, -32600, "Missing method");
            sc_json_free(alloc, req);
            continue;
        }

        if (strcmp(method, "initialize") == 0) {
            handle_initialize(srv, id_buf);
        } else if (strcmp(method, "notifications/initialized") == 0) {
            /* No-op notification */
        } else if (strcmp(method, "tools/list") == 0) {
            handle_tools_list(srv, id_buf);
        } else if (strcmp(method, "tools/call") == 0) {
            if (params)
                handle_tools_call(srv, id_buf, params);
            else
                write_error(alloc, id_buf, -32602, "Missing params");
        } else if (strcmp(method, "ping") == 0) {
            write_response(alloc, id_buf, "{}");
        } else {
            if (!is_notification)
                write_error(alloc, id_buf, -32601, "Method not found");
        }

        sc_json_free(alloc, req);
    }
}

void sc_mcp_host_destroy(sc_mcp_host_t *srv)
{
    if (!srv) return;
    sc_allocator_t *alloc = srv->alloc;
    alloc->free(alloc->ctx, srv, sizeof(*srv));
}
