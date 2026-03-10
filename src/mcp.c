#include "human/mcp.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define MCP_LINE_BUF_INIT    256
#define MCP_REQUEST_BUF_INIT 512

struct hu_mcp_server {
    hu_allocator_t *alloc;
    hu_mcp_server_config_t config;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    pid_t child_pid;
#endif
    int stdin_fd;
    int stdout_fd;
    uint32_t next_id;
    bool connected;
};

/* ── send_request helper (only used when !HU_IS_TEST) ─────────────────────── */

#if !defined(HU_IS_TEST) || HU_IS_TEST == 0

static hu_error_t read_line(hu_mcp_server_t *srv, hu_allocator_t *alloc, char **out_line,
                            size_t *out_len) {
    size_t cap = MCP_LINE_BUF_INIT;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t len = 0;

    while (1) {
        char byte;
        ssize_t n = read(srv->stdout_fd, &byte, 1);
        if (n <= 0) {
            alloc->free(alloc->ctx, buf, cap);
            return n == 0 ? HU_ERR_IO : HU_ERR_IO;
        }
        if (byte == '\n' || byte == '\r') {
            if (byte == '\r')
                continue;
            break;
        }
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
        buf[len++] = byte;
    }
    buf[len] = '\0';
    *out_line = buf;
    *out_len = len;
    return HU_OK;
}

static hu_error_t send_request(hu_mcp_server_t *srv, hu_allocator_t *alloc, const char *method,
                               const char *params_json, bool is_notification,
                               hu_json_value_t **out_result) {
    hu_json_buf_t req_buf;
    if (hu_json_buf_init(&req_buf, alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;

    hu_error_t err;
    if (is_notification) {
        err = hu_json_buf_append_raw(&req_buf, "{\"jsonrpc\":\"2.0\",\"method\":\"", 27);
    } else {
        char id_buf[16];
        int id_len = snprintf(id_buf, sizeof(id_buf), "%u", (unsigned)srv->next_id);
        err = hu_json_buf_append_raw(&req_buf, "{\"jsonrpc\":\"2.0\",\"id\":", 22);
        if (err == HU_OK)
            err = hu_json_buf_append_raw(&req_buf, id_buf, (size_t)id_len);
        if (err == HU_OK)
            err = hu_json_buf_append_raw(&req_buf, ",\"method\":\"", 11);
    }
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&req_buf, method, strlen(method));
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&req_buf, "\",\"params\":", 11);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&req_buf, params_json ? params_json : "{}",
                                 params_json ? strlen(params_json) : 2);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&req_buf, "}\n", 2);
    if (err != HU_OK)
        goto fail;

    size_t req_len = req_buf.len;
    ssize_t written = write(srv->stdin_fd, req_buf.ptr, req_len);
    hu_json_buf_free(&req_buf);
    if (written < 0 || (size_t)written != req_len)
        return HU_ERR_IO;

    if (is_notification) {
        *out_result = NULL;
        return HU_OK;
    }

    srv->next_id++;

    char *line = NULL;
    size_t line_len = 0;
    err = read_line(srv, alloc, &line, &line_len);
    if (err != HU_OK)
        return err;

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, line, line_len, &parsed);
    alloc->free(alloc->ctx, line, line_len + 1);
    if (err != HU_OK)
        return err;

    hu_json_value_t *err_val = parsed ? hu_json_object_get(parsed, "error") : NULL;
    if (err_val) {
        hu_json_free(alloc, parsed);
        return HU_ERR_TOOL_EXECUTION;
    }

    hu_json_value_t *result = parsed ? hu_json_object_get(parsed, "result") : NULL;
    if (!result) {
        hu_json_free(alloc, parsed);
        return HU_ERR_TOOL_EXECUTION;
    }

    *out_result = parsed;
    return HU_OK;
fail:
    hu_json_buf_free(&req_buf);
    return err;
}

#endif /* !HU_IS_TEST */

/* ── Public API ───────────────────────────────────────────────────────────── */

hu_mcp_server_t *hu_mcp_server_create(hu_allocator_t *alloc, const hu_mcp_server_config_t *config) {
    if (!alloc || !config || !config->command)
        return NULL;
    hu_mcp_server_t *srv = (hu_mcp_server_t *)alloc->alloc(alloc->ctx, sizeof(*srv));
    if (!srv)
        return NULL;
    memset(srv, 0, sizeof(*srv));
    srv->alloc = alloc;
    srv->config = *config;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    srv->child_pid = -1;
#endif
    srv->stdin_fd = -1;
    srv->stdout_fd = -1;
    srv->next_id = 1;
    srv->connected = false;
    return srv;
}

hu_error_t hu_mcp_server_connect(hu_mcp_server_t *srv) {
    if (!srv)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST != 0
    srv->connected = true;
    return HU_OK;
#else
    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0)
        return HU_ERR_IO;

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return HU_ERR_IO;
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        size_t argc = 1 + srv->config.args_count;
        char **argv = (char **)srv->alloc->alloc(srv->alloc->ctx, (argc + 1) * sizeof(char *));
        if (!argv)
            _exit(127);
        argv[0] = (char *)srv->config.command;
        for (size_t i = 0; i < srv->config.args_count; i++)
            argv[i + 1] = (char *)srv->config.args[i];
        argv[argc] = NULL;

        execvp(srv->config.command, (char *const *)argv);
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    srv->stdin_fd = stdin_pipe[1];
    srv->stdout_fd = stdout_pipe[0];
    srv->child_pid = pid;

    /* Send initialize request */
    const char *init_params = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{}}";
    hu_json_value_t *init_resp = NULL;
    hu_error_t err = send_request(srv, srv->alloc, "initialize", init_params, false, &init_resp);
    if (err != HU_OK) {
        hu_mcp_server_destroy(srv);
        return err;
    }

    if (!init_resp) {
        hu_mcp_server_destroy(srv);
        return HU_ERR_IO;
    }
    hu_json_value_t *result = hu_json_object_get(init_resp, "result");
    if (!result || result->type != HU_JSON_OBJECT) {
        hu_json_free(srv->alloc, init_resp);
        hu_mcp_server_destroy(srv);
        return HU_ERR_IO;
    }
    if (!hu_json_object_get(result, "protocolVersion")) {
        hu_json_free(srv->alloc, init_resp);
        hu_mcp_server_destroy(srv);
        return HU_ERR_IO;
    }
    hu_json_free(srv->alloc, init_resp);

    /* Send initialized notification */
    err = send_request(srv, srv->alloc, "notifications/initialized", "{}", true, NULL);
    if (err != HU_OK) {
        hu_mcp_server_destroy(srv);
        return err;
    }

    srv->connected = true;
    return HU_OK;
#endif
}

hu_error_t hu_mcp_server_list_tools(hu_mcp_server_t *srv, hu_allocator_t *alloc, char ***out_names,
                                    char ***out_descriptions, char ***out_params,
                                    size_t *out_count) {
    if (!srv || !alloc || !out_names || !out_descriptions || !out_params || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST != 0
    *out_count = 1;
    char **names = (char **)alloc->alloc(alloc->ctx, sizeof(char *));
    char **descs = (char **)alloc->alloc(alloc->ctx, sizeof(char *));
    char **params = (char **)alloc->alloc(alloc->ctx, sizeof(char *));
    if (!names || !descs || !params) {
        if (names)
            alloc->free(alloc->ctx, names, sizeof(char *));
        if (descs)
            alloc->free(alloc->ctx, descs, sizeof(char *));
        if (params)
            alloc->free(alloc->ctx, params, sizeof(char *));
        return HU_ERR_OUT_OF_MEMORY;
    }
    const char *mock_name = "mock_tool";
    const char *mock_desc = "Mock MCP tool";
    const char *mock_params =
        "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"}}}";
    names[0] = (char *)alloc->alloc(alloc->ctx, strlen(mock_name) + 1);
    descs[0] = (char *)alloc->alloc(alloc->ctx, strlen(mock_desc) + 1);
    params[0] = (char *)alloc->alloc(alloc->ctx, strlen(mock_params) + 1);
    if (!names[0] || !descs[0] || !params[0]) {
        if (names[0])
            alloc->free(alloc->ctx, names[0], strlen(mock_name) + 1);
        if (descs[0])
            alloc->free(alloc->ctx, descs[0], strlen(mock_desc) + 1);
        if (params[0])
            alloc->free(alloc->ctx, params[0], strlen(mock_params) + 1);
        alloc->free(alloc->ctx, names, sizeof(char *));
        alloc->free(alloc->ctx, descs, sizeof(char *));
        alloc->free(alloc->ctx, params, sizeof(char *));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(names[0], mock_name, strlen(mock_name) + 1);
    memcpy(descs[0], mock_desc, strlen(mock_desc) + 1);
    memcpy(params[0], mock_params, strlen(mock_params) + 1);
    *out_names = names;
    *out_descriptions = descs;
    *out_params = params;
    return HU_OK;
#else
    if (!srv->connected)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *resp = NULL;
    hu_error_t err = send_request(srv, alloc, "tools/list", "{}", false, &resp);
    if (err != HU_OK || !resp)
        return err;

    hu_json_value_t *result = hu_json_object_get(resp, "result");
    if (!result || result->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, resp);
        return HU_ERR_TOOL_EXECUTION;
    }
    hu_json_value_t *tools_arr = hu_json_object_get(result, "tools");
    if (!tools_arr || tools_arr->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, resp);
        return HU_ERR_TOOL_EXECUTION;
    }

    size_t n = tools_arr->data.array.len;
    char **names = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
    char **descs = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
    char **params = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
    if (!names || !descs || !params) {
        if (names)
            alloc->free(alloc->ctx, names, n * sizeof(char *));
        if (descs)
            alloc->free(alloc->ctx, descs, n * sizeof(char *));
        if (params)
            alloc->free(alloc->ctx, params, n * sizeof(char *));
        hu_json_free(alloc, resp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(names, 0, n * sizeof(char *));
    memset(descs, 0, n * sizeof(char *));
    memset(params, 0, n * sizeof(char *));

    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = tools_arr->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;

        hu_json_value_t *name_val = hu_json_object_get(item, "name");
        const char *name_str =
            (name_val && name_val->type == HU_JSON_STRING) ? name_val->data.string.ptr : "";
        size_t name_len =
            (name_val && name_val->type == HU_JSON_STRING) ? name_val->data.string.len : 0;

        hu_json_value_t *desc_val = hu_json_object_get(item, "description");
        const char *desc_str =
            (desc_val && desc_val->type == HU_JSON_STRING) ? desc_val->data.string.ptr : "";
        size_t desc_len =
            (desc_val && desc_val->type == HU_JSON_STRING) ? desc_val->data.string.len : 0;

        hu_json_value_t *schema_val = hu_json_object_get(item, "inputSchema");
        char *schema_str = NULL;
        size_t schema_len = 0;
        if (schema_val && schema_val->type == HU_JSON_OBJECT)
            hu_json_stringify(alloc, schema_val, &schema_str, &schema_len);

        names[i] = (char *)alloc->alloc(alloc->ctx, name_len + 1);
        descs[i] = (char *)alloc->alloc(alloc->ctx, desc_len + 1);
        if (!schema_str) {
            params[i] = (char *)alloc->alloc(alloc->ctx, 3);
            if (params[i])
                memcpy(params[i], "{}", 3);
        } else {
            params[i] = schema_str;
        }
        if (!names[i] || !descs[i] || !params[i]) {
            for (size_t j = 0; j <= i; j++) {
                if (names[j])
                    alloc->free(alloc->ctx, names[j], strlen(names[j]) + 1);
                if (descs[j])
                    alloc->free(alloc->ctx, descs[j], strlen(descs[j]) + 1);
                if (params[j])
                    alloc->free(alloc->ctx, params[j], strlen(params[j]) + 1);
            }
            alloc->free(alloc->ctx, names, n * sizeof(char *));
            alloc->free(alloc->ctx, descs, n * sizeof(char *));
            alloc->free(alloc->ctx, params, n * sizeof(char *));
            hu_json_free(alloc, resp);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memcpy(names[i], name_str, name_len + 1);
        memcpy(descs[i], desc_str, desc_len + 1);
    }

    hu_json_free(alloc, resp);
    *out_names = names;
    *out_descriptions = descs;
    *out_params = params;
    *out_count = n;
    return HU_OK;
#endif
}

hu_error_t hu_mcp_server_call_tool(hu_mcp_server_t *srv, hu_allocator_t *alloc,
                                   const char *tool_name, const char *args_json, char **out_result,
                                   size_t *out_result_len) {
    if (!srv || !alloc || !tool_name || !out_result || !out_result_len)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST != 0
    (void)args_json;
    const char *mock = "mock result";
    size_t len = strlen(mock);
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, mock, len + 1);
    *out_result = buf;
    *out_result_len = len;
    return HU_OK;
#else
    if (!srv->connected)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_buf_t params_buf;
    if (hu_json_buf_init(&params_buf, alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;
    hu_error_t err = hu_json_buf_append_raw(&params_buf, "{\"name\":", 8);
    if (err == HU_OK)
        err = hu_json_append_string(&params_buf, tool_name, strlen(tool_name));
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&params_buf, ",\"arguments\":", 13);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&params_buf, args_json ? args_json : "{}",
                                     args_json ? strlen(args_json) : 2);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&params_buf, "}", 1);
    if (err != HU_OK) {
        hu_json_buf_free(&params_buf);
        return err;
    }

    hu_json_value_t *resp = NULL;
    err = send_request(srv, alloc, "tools/call", params_buf.ptr, false, &resp);
    hu_json_buf_free(&params_buf);
    if (err != HU_OK || !resp)
        return err;

    hu_json_value_t *result = hu_json_object_get(resp, "result");
    if (!result || result->type != HU_JSON_OBJECT) {
        hu_json_free(alloc, resp);
        return HU_ERR_TOOL_EXECUTION;
    }
    hu_json_value_t *content = hu_json_object_get(result, "content");
    if (!content || content->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, resp);
        return HU_ERR_TOOL_EXECUTION;
    }

    /* Collect text from content array */
    hu_json_buf_t out_buf;
    if (hu_json_buf_init(&out_buf, alloc) != HU_OK) {
        hu_json_free(alloc, resp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < content->data.array.len; i++) {
        hu_json_value_t *item = content->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;
        hu_json_value_t *text_val = hu_json_object_get(item, "text");
        if (!text_val || text_val->type != HU_JSON_STRING)
            continue;
        if (out_buf.len > 0) {
            err = hu_json_buf_append_raw(&out_buf, "\n", 1);
            if (err != HU_OK)
                goto call_tool_fail;
        }
        err =
            hu_json_buf_append_raw(&out_buf, text_val->data.string.ptr, text_val->data.string.len);
        if (err != HU_OK)
            goto call_tool_fail;
    }

    size_t len = out_buf.len;
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf) {
        hu_json_buf_free(&out_buf);
        hu_json_free(alloc, resp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(buf, out_buf.ptr, len);
    buf[len] = '\0';
    hu_json_buf_free(&out_buf);
    hu_json_free(alloc, resp);

    *out_result = buf;
    *out_result_len = len;
    return HU_OK;

call_tool_fail:
    hu_json_buf_free(&out_buf);
    hu_json_free(alloc, resp);
    return err;
#endif
}

void hu_mcp_server_destroy(hu_mcp_server_t *srv) {
    if (!srv)
        return;

#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    if (srv->stdin_fd >= 0) {
        close(srv->stdin_fd);
        srv->stdin_fd = -1;
    }
    if (srv->stdout_fd >= 0) {
        close(srv->stdout_fd);
        srv->stdout_fd = -1;
    }
    if (srv->child_pid >= 0) {
        kill(srv->child_pid, SIGKILL);
        waitpid(srv->child_pid, NULL, 0);
        srv->child_pid = -1;
    }
#endif
    hu_allocator_t *alloc = srv->alloc;
    alloc->free(alloc->ctx, srv, sizeof(*srv));
}

/* ── MCP tool wrapper (vtable) ────────────────────────────────────────────── */

typedef struct hu_mcp_tool_wrapper {
    hu_allocator_t *alloc;
    hu_mcp_server_t *server;
    bool owns_server;
    char *original_name;
    char *prefixed_name;
    char *desc;
    char *params_json;
    size_t server_index;
} hu_mcp_tool_wrapper_t;

static hu_error_t mcp_tool_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                   hu_tool_result_t *out) {
    hu_mcp_tool_wrapper_t *w = (hu_mcp_tool_wrapper_t *)ctx;
    char *args_json = NULL;
    size_t args_len = 0;
    bool args_allocated = false;
    if (args && args->type == HU_JSON_OBJECT) {
        hu_error_t err = hu_json_stringify(alloc, args, &args_json, &args_len);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("Failed to serialize args", 24);
            return HU_OK;
        }
        args_allocated = true;
    }
    if (!args_json)
        args_json = "{}";

    char *result = NULL;
    size_t result_len = 0;
    hu_error_t err = hu_mcp_server_call_tool(w->server, alloc, w->original_name, args_json, &result,
                                             &result_len);
    if (args_allocated && args_json)
        alloc->free(alloc->ctx, args_json, args_len + 1);

    if (err != HU_OK) {
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "MCP tool '%s' failed", w->original_name);
        char *msg = (char *)alloc->alloc(alloc->ctx, (size_t)n + 1);
        if (msg) {
            memcpy(msg, buf, (size_t)n + 1);
            *out = hu_tool_result_fail_owned(msg, (size_t)n);
        } else {
            *out = hu_tool_result_fail("MCP tool call failed", 20);
        }
        return HU_OK;
    }
    *out = hu_tool_result_ok_owned(result, result_len);
    return HU_OK;
}

static const char *mcp_tool_name(void *ctx) {
    return ((hu_mcp_tool_wrapper_t *)ctx)->prefixed_name;
}
static const char *mcp_tool_description(void *ctx) {
    return ((hu_mcp_tool_wrapper_t *)ctx)->desc;
}
static const char *mcp_tool_parameters_json(void *ctx) {
    return ((hu_mcp_tool_wrapper_t *)ctx)->params_json;
}

static void mcp_tool_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_mcp_tool_wrapper_t *w = (hu_mcp_tool_wrapper_t *)ctx;
    if (w->original_name)
        alloc->free(alloc->ctx, w->original_name, strlen(w->original_name) + 1);
    if (w->prefixed_name)
        alloc->free(alloc->ctx, w->prefixed_name, strlen(w->prefixed_name) + 1);
    if (w->desc)
        alloc->free(alloc->ctx, w->desc, strlen(w->desc) + 1);
    if (w->params_json)
        alloc->free(alloc->ctx, w->params_json, strlen(w->params_json) + 1);
    if (w->owns_server)
        hu_mcp_server_destroy(w->server);
    alloc->free(alloc->ctx, w, sizeof(*w));
}

static const hu_tool_vtable_t mcp_tool_vtable = {
    .execute = mcp_tool_execute,
    .name = mcp_tool_name,
    .description = mcp_tool_description,
    .parameters_json = mcp_tool_parameters_json,
    .deinit = mcp_tool_deinit,
};

/* ── hu_mcp_init_tools / hu_mcp_free_tools ─────────────────────────────────── */

hu_error_t hu_mcp_init_tools(hu_allocator_t *alloc, const hu_mcp_server_config_t *server_configs,
                             size_t config_count, hu_tool_t **out_tools, size_t *out_count) {
    if (!alloc || !out_tools || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_tools = NULL;
    *out_count = 0;

    if (config_count == 0)
        return HU_OK;

    size_t total = 0;
    hu_tool_t *all = NULL;
    hu_mcp_server_t **servers = NULL;
    size_t *counts = NULL;

    servers =
        (hu_mcp_server_t **)alloc->alloc(alloc->ctx, config_count * sizeof(hu_mcp_server_t *));
    counts = (size_t *)alloc->alloc(alloc->ctx, config_count * sizeof(size_t));
    if (!servers || !counts) {
        if (servers)
            alloc->free(alloc->ctx, servers, config_count * sizeof(hu_mcp_server_t *));
        if (counts)
            alloc->free(alloc->ctx, counts, config_count * sizeof(size_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(servers, 0, config_count * sizeof(hu_mcp_server_t *));
    memset(counts, 0, config_count * sizeof(size_t));

    for (size_t i = 0; i < config_count; i++) {
        hu_mcp_server_t *srv = hu_mcp_server_create(alloc, &server_configs[i]);
        if (!srv)
            continue;
        hu_error_t err = hu_mcp_server_connect(srv);
        if (err != HU_OK) {
            hu_mcp_server_destroy(srv);
            continue;
        }

        char **names = NULL, **descs = NULL, **tool_params = NULL;
        size_t n = 0;
        err = hu_mcp_server_list_tools(srv, alloc, &names, &descs, &tool_params, &n);
        if (err != HU_OK || n == 0) {
            hu_mcp_server_destroy(srv);
            continue;
        }

        servers[i] = srv;
        counts[i] = n;
        total += n;

        for (size_t j = 0; j < n; j++) {
            if (!names[j])
                continue;
            size_t pref_len = 32 + strlen(names[j]);
            char *prefixed = (char *)alloc->alloc(alloc->ctx, pref_len);
            if (!prefixed) {
                for (size_t k = j; k < n; k++) {
                    alloc->free(alloc->ctx, names[k], strlen(names[k]) + 1);
                    alloc->free(alloc->ctx, descs[k], strlen(descs[k]) + 1);
                    if (tool_params && tool_params[k])
                        alloc->free(alloc->ctx, tool_params[k], strlen(tool_params[k]) + 1);
                }
                alloc->free(alloc->ctx, names, n * sizeof(char *));
                alloc->free(alloc->ctx, descs, n * sizeof(char *));
                if (tool_params)
                    alloc->free(alloc->ctx, tool_params, n * sizeof(char *));
                if (j == 0)
                    hu_mcp_server_destroy(srv);
                servers[i] = NULL;
                goto fail;
            }
            snprintf(prefixed, pref_len, "mcp_%zu_%s", i, names[j]);

            hu_mcp_tool_wrapper_t *w =
                (hu_mcp_tool_wrapper_t *)alloc->alloc(alloc->ctx, sizeof(hu_mcp_tool_wrapper_t));
            if (!w) {
                alloc->free(alloc->ctx, prefixed, pref_len);
                for (size_t k = j; k < n; k++) {
                    alloc->free(alloc->ctx, names[k], strlen(names[k]) + 1);
                    alloc->free(alloc->ctx, descs[k], strlen(descs[k]) + 1);
                    if (tool_params && tool_params[k])
                        alloc->free(alloc->ctx, tool_params[k], strlen(tool_params[k]) + 1);
                }
                alloc->free(alloc->ctx, names, n * sizeof(char *));
                alloc->free(alloc->ctx, descs, n * sizeof(char *));
                if (tool_params)
                    alloc->free(alloc->ctx, tool_params, n * sizeof(char *));
                if (j == 0)
                    hu_mcp_server_destroy(srv);
                servers[i] = NULL;
                goto fail;
            }
            w->alloc = alloc;
            w->server = srv;
            w->owns_server = (j == 0);
            w->original_name = names[j];
            w->prefixed_name = prefixed;
            w->desc = descs[j];
            if (tool_params && tool_params[j]) {
                w->params_json = tool_params[j];
            } else {
                w->params_json = (char *)alloc->alloc(alloc->ctx, 3);
                if (w->params_json)
                    memcpy(w->params_json, "{}", 3);
            }
            w->server_index = i;

            hu_tool_t *tools_buf = (hu_tool_t *)alloc->realloc(
                alloc->ctx, all, (total - 1) * sizeof(hu_tool_t), total * sizeof(hu_tool_t));
            if (!tools_buf) {
                mcp_tool_deinit(
                    w, alloc); /* frees w, names[j], descs[j], prefixed; destroys server if j==0 */
                for (size_t k = j + 1; k < n; k++) {
                    alloc->free(alloc->ctx, names[k], strlen(names[k]) + 1);
                    alloc->free(alloc->ctx, descs[k], strlen(descs[k]) + 1);
                    if (tool_params && tool_params[k])
                        alloc->free(alloc->ctx, tool_params[k], strlen(tool_params[k]) + 1);
                }
                alloc->free(alloc->ctx, names, n * sizeof(char *));
                alloc->free(alloc->ctx, descs, n * sizeof(char *));
                if (tool_params)
                    alloc->free(alloc->ctx, tool_params, n * sizeof(char *));
                servers[i] = NULL;
                goto fail;
            }
            all = tools_buf;
            all[total - 1].ctx = w;
            all[total - 1].vtable = &mcp_tool_vtable;
        }
        alloc->free(alloc->ctx, names, n * sizeof(char *));
        alloc->free(alloc->ctx, descs, n * sizeof(char *));
        if (tool_params)
            alloc->free(alloc->ctx, tool_params, n * sizeof(char *));
    }

    alloc->free(alloc->ctx, servers, config_count * sizeof(hu_mcp_server_t *));
    alloc->free(alloc->ctx, counts, config_count * sizeof(size_t));
    *out_tools = all;
    *out_count = total;
    return HU_OK;

fail:
    if (all) {
        for (size_t i = 0; i < total; i++) {
            if (all[i].vtable && all[i].vtable->deinit)
                all[i].vtable->deinit(all[i].ctx, alloc);
        }
        alloc->free(alloc->ctx, all, total * sizeof(hu_tool_t));
    }
    for (size_t i = 0; i < config_count; i++) {
        if (servers && servers[i])
            hu_mcp_server_destroy(servers[i]);
    }
    if (servers)
        alloc->free(alloc->ctx, servers, config_count * sizeof(hu_mcp_server_t *));
    if (counts)
        alloc->free(alloc->ctx, counts, config_count * sizeof(size_t));
    return HU_ERR_OUT_OF_MEMORY;
}

void hu_mcp_free_tools(hu_allocator_t *alloc, hu_tool_t *tools, size_t count) {
    if (!alloc || !tools)
        return;
    for (size_t i = 0; i < count; i++) {
        if (tools[i].vtable && tools[i].vtable->deinit)
            tools[i].vtable->deinit(tools[i].ctx, alloc);
    }
    alloc->free(alloc->ctx, tools, count * sizeof(hu_tool_t));
}
