#include "human/mcp_server.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/mcp_resources.h"
#include "human/memory.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MCP_SERVER_NAME      "human"
#define MCP_SERVER_VERSION   "0.5.0"
#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_LINE_BUF_INIT    4096

struct hu_mcp_engine {
    hu_allocator_t *alloc;
    hu_tool_t *tools;
    size_t tool_count;
    hu_memory_t *memory;
    hu_mcp_resource_registry_t *resources;
    hu_mcp_prompt_registry_t *prompts;
    bool initialized;
};

hu_error_t hu_mcp_engine_create(hu_allocator_t *alloc, hu_tool_t *tools, size_t tool_count,
                              hu_memory_t *memory, hu_mcp_engine_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_mcp_engine_t *srv = (hu_mcp_engine_t *)alloc->alloc(alloc->ctx, sizeof(*srv));
    if (!srv)
        return HU_ERR_OUT_OF_MEMORY;
    srv->alloc = alloc;
    srv->tools = tools;
    srv->tool_count = tool_count;
    srv->memory = memory;
    srv->resources = NULL;
    srv->prompts = NULL;
    srv->initialized = false;
    *out = srv;
    return HU_OK;
}

void hu_mcp_engine_set_resources(hu_mcp_engine_t *srv, hu_mcp_resource_registry_t *resources) {
    if (srv)
        srv->resources = resources;
}

void hu_mcp_engine_set_prompts(hu_mcp_engine_t *srv, hu_mcp_prompt_registry_t *prompts) {
    if (srv)
        srv->prompts = prompts;
}

/* ── JSON-RPC response helpers ─────────────────────────────────────────── */

static hu_error_t write_response(hu_allocator_t *alloc, const char *id_raw,
                                 const char *result_json) {
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;

    hu_error_t err = HU_OK;
    err = hu_json_buf_append_raw(&buf, "{\"jsonrpc\":\"2.0\",\"id\":", 22);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, id_raw, strlen(id_raw));
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, ",\"result\":", 10);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, result_json, strlen(result_json));
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, "}\n", 2);
    if (err != HU_OK) {
        hu_json_buf_free(&buf);
        return err;
    }

    fwrite(buf.ptr, 1, buf.len, stdout);
    fflush(stdout);
    hu_json_buf_free(&buf);
    return HU_OK;
}

static hu_error_t write_error(hu_allocator_t *alloc, const char *id_raw, int code,
                              const char *message) {
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;

    char code_buf[16];
    snprintf(code_buf, sizeof(code_buf), "%d", code);

    hu_error_t err = HU_OK;
    err = hu_json_buf_append_raw(&buf, "{\"jsonrpc\":\"2.0\",\"id\":", 22);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, id_raw, strlen(id_raw));
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, ",\"error\":{\"code\":", 17);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, code_buf, strlen(code_buf));
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, ",\"message\":", 11);
    if (err == HU_OK)
        err = hu_json_append_string(&buf, message, strlen(message));
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, "}}\n", 3);
    if (err != HU_OK) {
        hu_json_buf_free(&buf);
        return err;
    }

    fwrite(buf.ptr, 1, buf.len, stdout);
    fflush(stdout);
    hu_json_buf_free(&buf);
    return HU_OK;
}

/* ── Handler: initialize ───────────────────────────────────────────────── */

static hu_error_t handle_initialize(hu_mcp_engine_t *srv, const char *id_raw) {
    srv->initialized = true;
    const char *result = "{\"protocolVersion\":\"" MCP_PROTOCOL_VERSION "\","
                         "\"capabilities\":{\"tools\":{},\"resources\":{},\"prompts\":{}},"
                         "\"serverInfo\":{\"name\":\"" MCP_SERVER_NAME "\","
                         "\"version\":\"" MCP_SERVER_VERSION "\"}}";
    return write_response(srv->alloc, id_raw, result);
}

/* ── Handler: tools/list ───────────────────────────────────────────────── */

static hu_error_t handle_tools_list(hu_mcp_engine_t *srv, const char *id_raw) {
    hu_allocator_t *alloc = srv->alloc;
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;

    hu_error_t err = hu_json_buf_append_raw(&buf, "{\"tools\":[", 10);
    for (size_t i = 0; i < srv->tool_count && err == HU_OK; i++) {
        if (i > 0)
            err = hu_json_buf_append_raw(&buf, ",", 1);
        if (err != HU_OK)
            break;

        const char *name =
            srv->tools[i].vtable->name ? srv->tools[i].vtable->name(srv->tools[i].ctx) : "";
        const char *desc = srv->tools[i].vtable->description
                               ? srv->tools[i].vtable->description(srv->tools[i].ctx)
                               : "";
        const char *params = srv->tools[i].vtable->parameters_json
                                 ? srv->tools[i].vtable->parameters_json(srv->tools[i].ctx)
                                 : "{}";

        err = hu_json_buf_append_raw(&buf, "{\"name\":", 8);
        if (err == HU_OK)
            err = hu_json_append_string(&buf, name, strlen(name));
        if (err == HU_OK)
            err = hu_json_buf_append_raw(&buf, ",\"description\":", 15);
        if (err == HU_OK)
            err = hu_json_append_string(&buf, desc, strlen(desc));
        if (err == HU_OK)
            err = hu_json_buf_append_raw(&buf, ",\"inputSchema\":", 15);
        if (err == HU_OK)
            err = hu_json_buf_append_raw(&buf, params, strlen(params));
        if (err == HU_OK)
            err = hu_json_buf_append_raw(&buf, "}", 1);
    }
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, "]}", 2);

    if (err != HU_OK) {
        hu_json_buf_free(&buf);
        return err;
    }

    /* Null-terminate for write_response */
    size_t result_len = buf.len;
    char *result = (char *)alloc->alloc(alloc->ctx, result_len + 1);
    if (!result) {
        hu_json_buf_free(&buf);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(result, buf.ptr, result_len);
    result[result_len] = '\0';
    hu_json_buf_free(&buf);

    err = write_response(alloc, id_raw, result);
    alloc->free(alloc->ctx, result, result_len + 1);
    return err;
}

/* ── Handler: tools/call ───────────────────────────────────────────────── */

static hu_error_t handle_tools_call(hu_mcp_engine_t *srv, const char *id_raw,
                                    hu_json_value_t *params) {
    hu_allocator_t *alloc = srv->alloc;

    const char *tool_name = hu_json_get_string(params, "name");
    if (!tool_name || strlen(tool_name) == 0)
        return write_error(alloc, id_raw, -32602, "Missing tool name");

    hu_tool_t *tool = NULL;
    for (size_t i = 0; i < srv->tool_count; i++) {
        const char *n =
            srv->tools[i].vtable->name ? srv->tools[i].vtable->name(srv->tools[i].ctx) : "";
        if (strcmp(n, tool_name) == 0) {
            tool = &srv->tools[i];
            break;
        }
    }
    if (!tool)
        return write_error(alloc, id_raw, -32602, "Unknown tool");

    hu_json_value_t *args = hu_json_object_get(params, "arguments");

    hu_tool_result_t result = {0};
    hu_error_t err = tool->vtable->execute(tool->ctx, alloc, args, &result);
    if (err != HU_OK) {
        hu_tool_result_free(alloc, &result);
        return write_error(alloc, id_raw, -32603, "Tool execution failed");
    }

    /* Build MCP tool result: {content:[{type:"text",text:"..."}], isError:bool} */
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK) {
        hu_tool_result_free(alloc, &result);
        return HU_ERR_OUT_OF_MEMORY;
    }

    const char *text = result.success ? (result.output ? result.output : "")
                                      : (result.error_msg ? result.error_msg : "error");
    size_t text_len = result.success ? result.output_len : result.error_msg_len;

    err = hu_json_buf_append_raw(&buf, "{\"content\":[{\"type\":\"text\",\"text\":", 34);
    if (err == HU_OK)
        err = hu_json_append_string(&buf, text, text_len);
    if (err == HU_OK) {
        if (result.success)
            err = hu_json_buf_append_raw(&buf, "}]}", 3);
        else
            err = hu_json_buf_append_raw(&buf, "}],\"isError\":true}", 18);
    }

    hu_tool_result_free(alloc, &result);

    if (err != HU_OK) {
        hu_json_buf_free(&buf);
        return err;
    }

    char *res_str = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!res_str) {
        hu_json_buf_free(&buf);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(res_str, buf.ptr, buf.len);
    res_str[buf.len] = '\0';
    size_t res_len = buf.len;
    hu_json_buf_free(&buf);

    err = write_response(alloc, id_raw, res_str);
    alloc->free(alloc->ctx, res_str, res_len + 1);
    return err;
}

/* ── Handler: resources/list ─────────────────────────────────────────────── */

static hu_error_t handle_resources_list(hu_mcp_engine_t *srv, const char *id_raw) {
    hu_allocator_t *alloc = srv->alloc;
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;

    hu_error_t err = hu_json_buf_append_raw(&buf, "{\"resources\":[", 14);

    /* Always expose config resource */
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf,
                                     "{\"uri\":\"human://config\","
                                     "\"name\":\"Configuration\","
                                     "\"description\":\"Current human configuration\","
                                     "\"mimeType\":\"application/json\"}",
                                     125);

    /* If memory is available, expose memory resource */
    if (err == HU_OK && srv->memory && srv->memory->vtable) {
        err = hu_json_buf_append_raw(&buf,
                                     ",{\"uri\":\"human://memory\","
                                     "\"name\":\"Memory\","
                                     "\"description\":\"Agent memory entries\","
                                     "\"mimeType\":\"application/json\"}",
                                     110);
    }

    /* Append any resources from the MCP resource registry */
    if (err == HU_OK && srv->resources) {
        for (size_t i = 0; i < srv->resources->resource_count && err == HU_OK; i++) {
            const hu_mcp_resource_t *r = &srv->resources->resources[i];
            err = hu_json_buf_append_raw(&buf, ",{\"uri\":", 8);
            if (err == HU_OK)
                err = hu_json_append_string(&buf, r->uri, strlen(r->uri));
            if (err == HU_OK)
                err = hu_json_buf_append_raw(&buf, ",\"name\":", 8);
            if (err == HU_OK)
                err = hu_json_append_string(&buf, r->name, strlen(r->name));
            if (err == HU_OK)
                err = hu_json_buf_append_raw(&buf, ",\"description\":", 15);
            if (err == HU_OK)
                err = hu_json_append_string(&buf, r->description, strlen(r->description));
            if (err == HU_OK)
                err = hu_json_buf_append_raw(&buf, ",\"mimeType\":", 12);
            if (err == HU_OK)
                err = hu_json_append_string(&buf, r->mime_type, strlen(r->mime_type));
            if (err == HU_OK)
                err = hu_json_buf_append_raw(&buf, "}", 1);
        }
    }

    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, "]}", 2);

    if (err != HU_OK) {
        hu_json_buf_free(&buf);
        return err;
    }

    char *result = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!result) {
        hu_json_buf_free(&buf);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(result, buf.ptr, buf.len);
    result[buf.len] = '\0';
    size_t result_len = buf.len;
    hu_json_buf_free(&buf);

    err = write_response(alloc, id_raw, result);
    alloc->free(alloc->ctx, result, result_len + 1);
    return err;
}

/* ── Handler: resources/read ─────────────────────────────────────────────── */

static hu_error_t handle_resources_read(hu_mcp_engine_t *srv, const char *id_raw,
                                        hu_json_value_t *params) {
    hu_allocator_t *alloc = srv->alloc;
    const char *uri = hu_json_get_string(params, "uri");
    if (!uri)
        return write_error(alloc, id_raw, -32602, "Missing uri");

    if (strcmp(uri, "human://config") == 0) {
        const char *result =
            "{\"contents\":[{\"uri\":\"human://config\","
            "\"mimeType\":\"application/json\","
            "\"text\":\"{\\\"version\\\":\\\"0.3.0\\\",\\\"status\\\":\\\"running\\\"}\"}]}";
        return write_response(alloc, id_raw, result);
    }

    if (strcmp(uri, "human://memory") == 0) {
        if (!srv->memory || !srv->memory->vtable || !srv->memory->vtable->recall) {
            return write_response(alloc, id_raw,
                                  "{\"contents\":[{\"uri\":\"human://memory\","
                                  "\"mimeType\":\"application/json\","
                                  "\"text\":\"[]\"}]}");
        }
        hu_memory_entry_t *entries = NULL;
        size_t count = 0;
        hu_error_t err = srv->memory->vtable->recall(srv->memory->ctx, alloc, "", 0, 10, NULL, 0,
                                                     &entries, &count);
        if (err != HU_OK || count == 0) {
            if (entries) {
                for (size_t i = 0; i < count; i++)
                    hu_memory_entry_free_fields(alloc, &entries[i]);
                alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
            }
            return write_response(alloc, id_raw,
                                  "{\"contents\":[{\"uri\":\"human://memory\","
                                  "\"mimeType\":\"application/json\","
                                  "\"text\":\"[]\"}]}");
        }
        /* Build JSON with entry count; free entries */
        for (size_t i = 0; i < count; i++)
            hu_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));

        hu_json_buf_t buf;
        if (hu_json_buf_init(&buf, alloc) != HU_OK)
            return HU_ERR_OUT_OF_MEMORY;
        err = hu_json_buf_append_raw(&buf,
                                     "{\"contents\":[{\"uri\":\"human://memory\","
                                     "\"mimeType\":\"application/json\",\"text\":\"",
                                     77);
        char count_str[32];
        int n = snprintf(count_str, sizeof(count_str), "[%zu entries]", count);
        if (err == HU_OK && n > 0)
            err = hu_json_buf_append_raw(&buf, count_str, (size_t)n);
        if (err == HU_OK)
            err = hu_json_buf_append_raw(&buf, "\"}]}", 4);
        if (err != HU_OK) {
            hu_json_buf_free(&buf);
            return err;
        }
        char *result = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
        if (!result) {
            hu_json_buf_free(&buf);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(result, buf.ptr, buf.len);
        result[buf.len] = '\0';
        size_t result_len = buf.len;
        hu_json_buf_free(&buf);
        err = write_response(alloc, id_raw, result);
        alloc->free(alloc->ctx, result, result_len + 1);
        return err;
    }

    return write_error(alloc, id_raw, -32602, "Unknown resource URI");
}

/* ── Handler: prompts/list ──────────────────────────────────────────────── */

static hu_error_t handle_prompts_list(hu_mcp_engine_t *srv, const char *id_raw) {
    hu_allocator_t *alloc = srv->alloc;

    if (!srv->prompts || srv->prompts->prompt_count == 0)
        return write_response(alloc, id_raw, "{\"prompts\":[]}");

    char *json = NULL;
    size_t json_len = 0;
    hu_error_t err = hu_mcp_prompt_list_json(alloc, srv->prompts, &json, &json_len);
    if (err != HU_OK)
        return write_response(alloc, id_raw, "{\"prompts\":[]}");

    err = write_response(alloc, id_raw, json);
    alloc->free(alloc->ctx, json, json_len);
    return err;
}

/* ── Handler: prompts/get ──────────────────────────────────────────────── */

static hu_error_t handle_prompts_get(hu_mcp_engine_t *srv, const char *id_raw,
                                     hu_json_value_t *params) {
    hu_allocator_t *alloc = srv->alloc;
    const char *name = hu_json_get_string(params, "name");
    if (!name)
        return write_error(alloc, id_raw, -32602, "Missing prompt name");

    if (!srv->prompts)
        return write_error(alloc, id_raw, -32602, "No prompts registered");

    const hu_mcp_prompt_t *prompt = NULL;
    for (size_t i = 0; i < srv->prompts->prompt_count; i++) {
        if (strcmp(srv->prompts->prompts[i].name, name) == 0) {
            prompt = &srv->prompts->prompts[i];
            break;
        }
    }
    if (!prompt)
        return write_error(alloc, id_raw, -32602, "Unknown prompt");

    hu_json_value_t *args_obj = hu_json_object_get(params, "arguments");
    const char *arg_names[HU_MCP_MAX_PROMPT_ARGS] = {0};
    const char *arg_values[HU_MCP_MAX_PROMPT_ARGS] = {0};
    size_t arg_count = 0;

    if (args_obj && args_obj->type == HU_JSON_OBJECT) {
        for (size_t i = 0; i < args_obj->data.object.len && arg_count < HU_MCP_MAX_PROMPT_ARGS;
             i++) {
            hu_json_pair_t *p = &args_obj->data.object.pairs[i];
            if (p->value && p->value->type == HU_JSON_STRING) {
                arg_names[arg_count] = p->key;
                arg_values[arg_count] = p->value->data.string.ptr;
                arg_count++;
            }
        }
    }

    char *rendered = NULL;
    size_t rendered_len = 0;
    hu_error_t err = hu_mcp_prompt_render(alloc, prompt, arg_names, arg_values, arg_count,
                                          &rendered, &rendered_len);
    if (err != HU_OK)
        return write_error(alloc, id_raw, -32603, "Prompt render failed");

    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK) {
        alloc->free(alloc->ctx, rendered, rendered_len);
        return HU_ERR_OUT_OF_MEMORY;
    }

    err = hu_json_buf_append_raw(&buf, "{\"description\":", 15);
    if (err == HU_OK)
        err = hu_json_append_string(&buf, prompt->description, strlen(prompt->description));
    if (err == HU_OK)
        err = hu_json_buf_append_raw(
            &buf, ",\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":", 63);
    if (err == HU_OK)
        err = hu_json_append_string(&buf, rendered, rendered_len);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, "}}]}", 4);

    alloc->free(alloc->ctx, rendered, rendered_len);

    if (err != HU_OK) {
        hu_json_buf_free(&buf);
        return err;
    }

    char *result = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!result) {
        hu_json_buf_free(&buf);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(result, buf.ptr, buf.len);
    result[buf.len] = '\0';
    size_t result_len = buf.len;
    hu_json_buf_free(&buf);

    err = write_response(alloc, id_raw, result);
    alloc->free(alloc->ctx, result, result_len + 1);
    return err;
}

/* ── Main loop ─────────────────────────────────────────────────────────── */

static hu_error_t read_stdin_line(hu_allocator_t *alloc, char **out, size_t *out_len,
                                  size_t *out_alloc_size) {
    size_t cap = MCP_LINE_BUF_INIT;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t len = 0;

    int ch;
    while ((ch = fgetc(stdin)) != EOF) {
        if (ch == '\n')
            break;
        if (ch == '\r')
            continue;
        if (len + 1 >= cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nb) {
                alloc->free(alloc->ctx, buf, cap);
                return HU_ERR_OUT_OF_MEMORY;
            }
            buf = nb;
            cap = new_cap;
        }
        buf[len++] = (char)ch;
    }

    if (ch == EOF && len == 0) {
        alloc->free(alloc->ctx, buf, cap);
        return HU_ERR_IO;
    }

    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    *out_alloc_size = cap;
    return HU_OK;
}

hu_error_t hu_mcp_engine_run(hu_mcp_engine_t *srv) {
    if (!srv)
        return HU_ERR_INVALID_ARGUMENT;
    hu_allocator_t *alloc = srv->alloc;

    for (;;) {
        char *line = NULL;
        size_t line_len = 0;
        size_t line_alloc = 0;
        hu_error_t err = read_stdin_line(alloc, &line, &line_len, &line_alloc);
        if (err == HU_ERR_IO)
            return HU_OK; /* EOF — clean shutdown */
        if (err != HU_OK)
            return err;
        if (line_len == 0) {
            alloc->free(alloc->ctx, line, line_alloc);
            continue;
        }

        hu_json_value_t *req = NULL;
        err = hu_json_parse(alloc, line, line_len, &req);
        alloc->free(alloc->ctx, line, line_alloc);

        if (err != HU_OK || !req)
            continue; /* skip malformed input */

        const char *method = hu_json_get_string(req, "method");
        hu_json_value_t *id_val = hu_json_object_get(req, "id");
        hu_json_value_t *params = hu_json_object_get(req, "params");
        bool is_notification = (id_val == NULL);

        /* Serialize id for response */
        char id_buf[64] = "null";
        if (id_val) {
            if (id_val->type == HU_JSON_NUMBER) {
                snprintf(id_buf, sizeof(id_buf), "%d", (int)id_val->data.number);
            } else if (id_val->type == HU_JSON_STRING) {
                snprintf(id_buf, sizeof(id_buf), "\"%.*s\"",
                         (int)(id_val->data.string.len < 50 ? id_val->data.string.len : 50),
                         id_val->data.string.ptr);
            }
        }

        if (!method) {
            if (!is_notification)
                write_error(alloc, id_buf, -32600, "Missing method");
            hu_json_free(alloc, req);
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
        } else if (strcmp(method, "resources/list") == 0) {
            handle_resources_list(srv, id_buf);
        } else if (strcmp(method, "resources/read") == 0) {
            if (params)
                handle_resources_read(srv, id_buf, params);
            else
                write_error(alloc, id_buf, -32602, "Missing params");
        } else if (strcmp(method, "prompts/list") == 0) {
            handle_prompts_list(srv, id_buf);
        } else if (strcmp(method, "prompts/get") == 0) {
            if (params)
                handle_prompts_get(srv, id_buf, params);
            else
                write_error(alloc, id_buf, -32602, "Missing params");
        } else if (strcmp(method, "ping") == 0) {
            write_response(alloc, id_buf, "{}");
        } else {
            if (!is_notification)
                write_error(alloc, id_buf, -32601, "Method not found");
        }

        hu_json_free(alloc, req);
    }
}

void hu_mcp_engine_destroy(hu_mcp_engine_t *srv) {
    if (!srv)
        return;
    hu_allocator_t *alloc = srv->alloc;
    alloc->free(alloc->ctx, srv, sizeof(*srv));
}
