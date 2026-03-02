#include "seaclaw/mcp.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/tool.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#endif

#define MCP_LINE_BUF_INIT 256
#define MCP_REQUEST_BUF_INIT 512

struct sc_mcp_server {
    sc_allocator_t *alloc;
    sc_mcp_server_config_t config;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    pid_t child_pid;
#endif
    int stdin_fd;
    int stdout_fd;
    uint32_t next_id;
    bool connected;
};

/* ── send_request helper (only used when !SC_IS_TEST) ─────────────────────── */

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0

static sc_error_t read_line(sc_mcp_server_t *srv, sc_allocator_t *alloc,
    char **out_line, size_t *out_len)
{
    size_t cap = MCP_LINE_BUF_INIT;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
    size_t len = 0;

    while (1) {
        char byte;
        ssize_t n = read(srv->stdout_fd, &byte, 1);
        if (n <= 0) {
            alloc->free(alloc->ctx, buf, cap);
            return n == 0 ? SC_ERR_IO : SC_ERR_IO;
        }
        if (byte == '\n' || byte == '\r') {
            if (byte == '\r') continue;
            break;
        }
        if (len + 1 >= cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nb) { alloc->free(alloc->ctx, buf, cap); return SC_ERR_OUT_OF_MEMORY; }
            buf = nb;
            cap = new_cap;
        }
        buf[len++] = byte;
    }
    buf[len] = '\0';
    *out_line = buf;
    *out_len = len;
    return SC_OK;
}

static sc_error_t send_request(sc_mcp_server_t *srv, sc_allocator_t *alloc,
    const char *method, const char *params_json, bool is_notification,
    sc_json_value_t **out_result)
{
    sc_json_buf_t req_buf;
    if (sc_json_buf_init(&req_buf, alloc) != SC_OK) return SC_ERR_OUT_OF_MEMORY;

    sc_error_t err;
    if (is_notification) {
        err = sc_json_buf_append_raw(&req_buf, "{\"jsonrpc\":\"2.0\",\"method\":\"", 28);
    } else {
        char id_buf[16];
        int id_len = snprintf(id_buf, sizeof(id_buf), "%u", (unsigned)srv->next_id);
        err = sc_json_buf_append_raw(&req_buf, "{\"jsonrpc\":\"2.0\",\"id\":", 23);
        if (err == SC_OK) err = sc_json_buf_append_raw(&req_buf, id_buf, (size_t)id_len);
        if (err == SC_OK) err = sc_json_buf_append_raw(&req_buf, ",\"method\":\"", 10);
    }
    if (err != SC_OK) goto fail;
    err = sc_json_buf_append_raw(&req_buf, method, strlen(method));
    if (err != SC_OK) goto fail;
    err = sc_json_buf_append_raw(&req_buf, "\",\"params\":", 11);
    if (err != SC_OK) goto fail;
    err = sc_json_buf_append_raw(&req_buf, params_json ? params_json : "{}",
        params_json ? strlen(params_json) : 2);
    if (err != SC_OK) goto fail;
    err = sc_json_buf_append_raw(&req_buf, "}\n", 2);
    if (err != SC_OK) goto fail;

    size_t req_len = req_buf.len;
    ssize_t written = write(srv->stdin_fd, req_buf.ptr, req_len);
    sc_json_buf_free(&req_buf);
    if (written < 0 || (size_t)written != req_len) return SC_ERR_IO;

    if (is_notification) {
        *out_result = NULL;
        return SC_OK;
    }

    srv->next_id++;

    char *line = NULL;
    size_t line_len = 0;
    err = read_line(srv, alloc, &line, &line_len);
    if (err != SC_OK) return err;

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(alloc, line, line_len, &parsed);
    alloc->free(alloc->ctx, line, line_len + 1);
    if (err != SC_OK) return err;

    sc_json_value_t *err_val = parsed ? sc_json_object_get(parsed, "error") : NULL;
    if (err_val) {
        sc_json_free(alloc, parsed);
        return SC_ERR_TOOL_EXECUTION;
    }

    sc_json_value_t *result = parsed ? sc_json_object_get(parsed, "result") : NULL;
    if (!result) {
        sc_json_free(alloc, parsed);
        return SC_ERR_TOOL_EXECUTION;
    }

    *out_result = parsed;
    return SC_OK;
fail:
    sc_json_buf_free(&req_buf);
    return err;
}

#endif /* !SC_IS_TEST */

/* ── Public API ───────────────────────────────────────────────────────────── */

sc_mcp_server_t *sc_mcp_server_create(sc_allocator_t *alloc,
    const sc_mcp_server_config_t *config)
{
    if (!alloc || !config || !config->command) return NULL;
    sc_mcp_server_t *srv = (sc_mcp_server_t *)alloc->alloc(alloc->ctx, sizeof(*srv));
    if (!srv) return NULL;
    memset(srv, 0, sizeof(*srv));
    srv->alloc = alloc;
    srv->config = *config;
#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    srv->child_pid = -1;
#endif
    srv->stdin_fd = -1;
    srv->stdout_fd = -1;
    srv->next_id = 1;
    srv->connected = false;
    return srv;
}

sc_error_t sc_mcp_server_connect(sc_mcp_server_t *srv)
{
    if (!srv) return SC_ERR_INVALID_ARGUMENT;

#if defined(SC_IS_TEST) && SC_IS_TEST != 0
    srv->connected = true;
    return SC_OK;
#else
    int stdin_pipe[2], stdout_pipe[2];
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) return SC_ERR_IO;

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return SC_ERR_IO;
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        size_t argc = 1 + srv->config.args_count;
        char **argv = (char **)srv->alloc->alloc(srv->alloc->ctx,
            (argc + 1) * sizeof(char *));
        if (!argv) _exit(127);
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
    sc_json_value_t *init_resp = NULL;
    sc_error_t err = send_request(srv, srv->alloc, "initialize", init_params,
        false, &init_resp);
    if (err != SC_OK) {
        sc_mcp_server_destroy(srv);
        return err;
    }

    if (!init_resp) {
        sc_mcp_server_destroy(srv);
        return SC_ERR_IO;
    }
    sc_json_value_t *result = sc_json_object_get(init_resp, "result");
    if (!result || result->type != SC_JSON_OBJECT) {
        sc_json_free(srv->alloc, init_resp);
        sc_mcp_server_destroy(srv);
        return SC_ERR_IO;
    }
    if (!sc_json_object_get(result, "protocolVersion")) {
        sc_json_free(srv->alloc, init_resp);
        sc_mcp_server_destroy(srv);
        return SC_ERR_IO;
    }
    sc_json_free(srv->alloc, init_resp);

    /* Send initialized notification */
    err = send_request(srv, srv->alloc, "notifications/initialized", "{}", true, NULL);
    if (err != SC_OK) {
        sc_mcp_server_destroy(srv);
        return err;
    }

    srv->connected = true;
    return SC_OK;
#endif
}

sc_error_t sc_mcp_server_list_tools(sc_mcp_server_t *srv, sc_allocator_t *alloc,
    char ***out_names, char ***out_descriptions, size_t *out_count)
{
    if (!srv || !alloc || !out_names || !out_descriptions || !out_count)
        return SC_ERR_INVALID_ARGUMENT;

#if defined(SC_IS_TEST) && SC_IS_TEST != 0
    *out_count = 1;
    char **names = (char **)alloc->alloc(alloc->ctx, sizeof(char *));
    char **descs = (char **)alloc->alloc(alloc->ctx, sizeof(char *));
    if (!names || !descs) {
        if (names) alloc->free(alloc->ctx, names, sizeof(char *));
        if (descs) alloc->free(alloc->ctx, descs, sizeof(char *));
        return SC_ERR_OUT_OF_MEMORY;
    }
    const char *mock_name = "mock_tool";
    const char *mock_desc = "Mock MCP tool";
    names[0] = (char *)alloc->alloc(alloc->ctx, strlen(mock_name) + 1);
    descs[0] = (char *)alloc->alloc(alloc->ctx, strlen(mock_desc) + 1);
    if (!names[0] || !descs[0]) {
        if (names[0]) alloc->free(alloc->ctx, names[0], strlen(mock_name) + 1);
        if (descs[0]) alloc->free(alloc->ctx, descs[0], strlen(mock_desc) + 1);
        alloc->free(alloc->ctx, names, sizeof(char *));
        alloc->free(alloc->ctx, descs, sizeof(char *));
        return SC_ERR_OUT_OF_MEMORY;
    }
    strcpy(names[0], mock_name);
    strcpy(descs[0], mock_desc);
    *out_names = names;
    *out_descriptions = descs;
    return SC_OK;
#else
    if (!srv->connected) return SC_ERR_INVALID_ARGUMENT;

    sc_json_value_t *resp = NULL;
    sc_error_t err = send_request(srv, alloc, "tools/list", "{}", false, &resp);
    if (err != SC_OK || !resp) return err;

    sc_json_value_t *result = sc_json_object_get(resp, "result");
    if (!result || result->type != SC_JSON_OBJECT) {
        sc_json_free(alloc, resp);
        return SC_ERR_TOOL_EXECUTION;
    }
    sc_json_value_t *tools_arr = sc_json_object_get(result, "tools");
    if (!tools_arr || tools_arr->type != SC_JSON_ARRAY) {
        sc_json_free(alloc, resp);
        return SC_ERR_TOOL_EXECUTION;
    }

    size_t n = tools_arr->data.array.len;
    char **names = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
    char **descs = (char **)alloc->alloc(alloc->ctx, n * sizeof(char *));
    if (!names || !descs) {
        if (names) alloc->free(alloc->ctx, names, n * sizeof(char *));
        if (descs) alloc->free(alloc->ctx, descs, n * sizeof(char *));
        sc_json_free(alloc, resp);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(names, 0, n * sizeof(char *));
    memset(descs, 0, n * sizeof(char *));

    for (size_t i = 0; i < n; i++) {
        sc_json_value_t *item = tools_arr->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT) continue;

        sc_json_value_t *name_val = sc_json_object_get(item, "name");
        const char *name_str = (name_val && name_val->type == SC_JSON_STRING)
            ? name_val->data.string.ptr : "";
        size_t name_len = (name_val && name_val->type == SC_JSON_STRING)
            ? name_val->data.string.len : 0;

        sc_json_value_t *desc_val = sc_json_object_get(item, "description");
        const char *desc_str = (desc_val && desc_val->type == SC_JSON_STRING)
            ? desc_val->data.string.ptr : "";
        size_t desc_len = (desc_val && desc_val->type == SC_JSON_STRING)
            ? desc_val->data.string.len : 0;

        names[i] = (char *)alloc->alloc(alloc->ctx, name_len + 1);
        descs[i] = (char *)alloc->alloc(alloc->ctx, desc_len + 1);
        if (!names[i] || !descs[i]) {
            for (size_t j = 0; j <= i; j++) {
                if (names[j]) alloc->free(alloc->ctx, names[j],
                    strlen(names[j]) + 1);
                if (descs[j]) alloc->free(alloc->ctx, descs[j],
                    strlen(descs[j]) + 1);
            }
            alloc->free(alloc->ctx, names, n * sizeof(char *));
            alloc->free(alloc->ctx, descs, n * sizeof(char *));
            sc_json_free(alloc, resp);
            return SC_ERR_OUT_OF_MEMORY;
        }
        memcpy(names[i], name_str, name_len + 1);
        memcpy(descs[i], desc_str, desc_len + 1);
    }

    sc_json_free(alloc, resp);
    *out_names = names;
    *out_descriptions = descs;
    *out_count = n;
    return SC_OK;
#endif
}

sc_error_t sc_mcp_server_call_tool(sc_mcp_server_t *srv, sc_allocator_t *alloc,
    const char *tool_name, const char *args_json,
    char **out_result, size_t *out_result_len)
{
    if (!srv || !alloc || !tool_name || !out_result || !out_result_len)
        return SC_ERR_INVALID_ARGUMENT;

#if defined(SC_IS_TEST) && SC_IS_TEST != 0
    (void)args_json;
    const char *mock = "mock result";
    size_t len = strlen(mock);
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
    memcpy(buf, mock, len + 1);
    *out_result = buf;
    *out_result_len = len;
    return SC_OK;
#else
    if (!srv->connected) return SC_ERR_INVALID_ARGUMENT;

    sc_json_buf_t params_buf;
    if (sc_json_buf_init(&params_buf, alloc) != SC_OK) return SC_ERR_OUT_OF_MEMORY;
    sc_error_t err = sc_json_buf_append_raw(&params_buf, "{\"name\":", 8);
    if (err == SC_OK) err = sc_json_append_string(&params_buf, tool_name, strlen(tool_name));
    if (err == SC_OK) err = sc_json_buf_append_raw(&params_buf, ",\"arguments\":", 14);
    if (err == SC_OK) err = sc_json_buf_append_raw(&params_buf,
        args_json ? args_json : "{}", args_json ? strlen(args_json) : 2);
    if (err == SC_OK) err = sc_json_buf_append_raw(&params_buf, "}", 1);
    if (err != SC_OK) {
        sc_json_buf_free(&params_buf);
        return err;
    }

    sc_json_value_t *resp = NULL;
    err = send_request(srv, alloc, "tools/call", params_buf.ptr, false, &resp);
    sc_json_buf_free(&params_buf);
    if (err != SC_OK || !resp) return err;

    sc_json_value_t *result = sc_json_object_get(resp, "result");
    if (!result || result->type != SC_JSON_OBJECT) {
        sc_json_free(alloc, resp);
        return SC_ERR_TOOL_EXECUTION;
    }
    sc_json_value_t *content = sc_json_object_get(result, "content");
    if (!content || content->type != SC_JSON_ARRAY) {
        sc_json_free(alloc, resp);
        return SC_ERR_TOOL_EXECUTION;
    }

    /* Collect text from content array */
    sc_json_buf_t out_buf;
    if (sc_json_buf_init(&out_buf, alloc) != SC_OK) {
        sc_json_free(alloc, resp);
        return SC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < content->data.array.len; i++) {
        sc_json_value_t *item = content->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT) continue;
        sc_json_value_t *text_val = sc_json_object_get(item, "text");
        if (!text_val || text_val->type != SC_JSON_STRING) continue;
        if (out_buf.len > 0) {
            err = sc_json_buf_append_raw(&out_buf, "\n", 1);
            if (err != SC_OK) goto call_tool_fail;
        }
        err = sc_json_buf_append_raw(&out_buf, text_val->data.string.ptr,
            text_val->data.string.len);
        if (err != SC_OK) goto call_tool_fail;
    }

    size_t len = out_buf.len;
    char *buf = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!buf) {
        sc_json_buf_free(&out_buf);
        sc_json_free(alloc, resp);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(buf, out_buf.ptr, len);
    buf[len] = '\0';
    sc_json_buf_free(&out_buf);
    sc_json_free(alloc, resp);

    *out_result = buf;
    *out_result_len = len;
    return SC_OK;

call_tool_fail:
    sc_json_buf_free(&out_buf);
    sc_json_free(alloc, resp);
    return err;
#endif
}

void sc_mcp_server_destroy(sc_mcp_server_t *srv)
{
    if (!srv) return;

#if !defined(SC_IS_TEST) || SC_IS_TEST == 0
    if (srv->stdin_fd >= 0) { close(srv->stdin_fd); srv->stdin_fd = -1; }
    if (srv->stdout_fd >= 0) { close(srv->stdout_fd); srv->stdout_fd = -1; }
    if (srv->child_pid >= 0) {
        kill(srv->child_pid, SIGKILL);
        waitpid(srv->child_pid, NULL, 0);
        srv->child_pid = -1;
    }
#endif
    sc_allocator_t *alloc = srv->alloc;
    alloc->free(alloc->ctx, srv, sizeof(*srv));
}

/* ── MCP tool wrapper (vtable) ────────────────────────────────────────────── */

typedef struct sc_mcp_tool_wrapper {
    sc_allocator_t *alloc;
    sc_mcp_server_t *server;
    bool owns_server;
    char *original_name;
    char *prefixed_name;
    char *desc;
    char *params_json;
    size_t server_index;
} sc_mcp_tool_wrapper_t;

static sc_error_t mcp_tool_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    sc_mcp_tool_wrapper_t *w = (sc_mcp_tool_wrapper_t *)ctx;
    char *args_json = NULL;
    size_t args_len = 0;
    bool args_allocated = false;
    if (args && args->type == SC_JSON_OBJECT) {
        sc_error_t err = sc_json_stringify(alloc, args, &args_json, &args_len);
        if (err != SC_OK) {
            *out = sc_tool_result_fail("Failed to serialize args", 24);
            return SC_OK;
        }
        args_allocated = true;
    }
    if (!args_json) args_json = "{}";

    char *result = NULL;
    size_t result_len = 0;
    sc_error_t err = sc_mcp_server_call_tool(w->server, alloc,
        w->original_name, args_json, &result, &result_len);
    if (args_allocated && args_json)
        alloc->free(alloc->ctx, args_json, args_len + 1);

    if (err != SC_OK) {
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "MCP tool '%s' failed", w->original_name);
        char *msg = (char *)alloc->alloc(alloc->ctx, (size_t)n + 1);
        if (msg) {
            memcpy(msg, buf, (size_t)n + 1);
            *out = sc_tool_result_fail_owned(msg, (size_t)n);
        } else {
            *out = sc_tool_result_fail("MCP tool call failed", 20);
        }
        return SC_OK;
    }
    *out = sc_tool_result_ok_owned(result, result_len);
    return SC_OK;
}

static const char *mcp_tool_name(void *ctx) {
    return ((sc_mcp_tool_wrapper_t *)ctx)->prefixed_name;
}
static const char *mcp_tool_description(void *ctx) {
    return ((sc_mcp_tool_wrapper_t *)ctx)->desc;
}
static const char *mcp_tool_parameters_json(void *ctx) {
    return ((sc_mcp_tool_wrapper_t *)ctx)->params_json;
}

static void mcp_tool_deinit(void *ctx, sc_allocator_t *alloc)
{
    sc_mcp_tool_wrapper_t *w = (sc_mcp_tool_wrapper_t *)ctx;
    if (w->original_name) alloc->free(alloc->ctx, w->original_name,
        strlen(w->original_name) + 1);
    if (w->prefixed_name) alloc->free(alloc->ctx, w->prefixed_name,
        strlen(w->prefixed_name) + 1);
    if (w->desc) alloc->free(alloc->ctx, w->desc, strlen(w->desc) + 1);
    if (w->params_json) alloc->free(alloc->ctx, w->params_json,
        strlen(w->params_json) + 1);
    if (w->owns_server) sc_mcp_server_destroy(w->server);
    alloc->free(alloc->ctx, w, sizeof(*w));
}

static const sc_tool_vtable_t mcp_tool_vtable = {
    .execute = mcp_tool_execute,
    .name = mcp_tool_name,
    .description = mcp_tool_description,
    .parameters_json = mcp_tool_parameters_json,
    .deinit = mcp_tool_deinit,
};

/* ── sc_mcp_init_tools / sc_mcp_free_tools ─────────────────────────────────── */

sc_error_t sc_mcp_init_tools(sc_allocator_t *alloc,
    const sc_mcp_server_config_t *server_configs, size_t config_count,
    sc_tool_t **out_tools, size_t *out_count)
{
    if (!alloc || !out_tools || !out_count) return SC_ERR_INVALID_ARGUMENT;
    *out_tools = NULL;
    *out_count = 0;

    if (config_count == 0) return SC_OK;

    size_t total = 0;
    sc_tool_t *all = NULL;
    sc_mcp_server_t **servers = NULL;
    size_t *counts = NULL;

    servers = (sc_mcp_server_t **)alloc->alloc(alloc->ctx,
        config_count * sizeof(sc_mcp_server_t *));
    counts = (size_t *)alloc->alloc(alloc->ctx, config_count * sizeof(size_t));
    if (!servers || !counts) {
        if (servers) alloc->free(alloc->ctx, servers, config_count * sizeof(sc_mcp_server_t *));
        if (counts) alloc->free(alloc->ctx, counts, config_count * sizeof(size_t));
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(servers, 0, config_count * sizeof(sc_mcp_server_t *));
    memset(counts, 0, config_count * sizeof(size_t));

    for (size_t i = 0; i < config_count; i++) {
        sc_mcp_server_t *srv = sc_mcp_server_create(alloc, &server_configs[i]);
        if (!srv) continue;
        sc_error_t err = sc_mcp_server_connect(srv);
        if (err != SC_OK) {
            sc_mcp_server_destroy(srv);
            continue;
        }

        char **names = NULL, **descs = NULL;
        size_t n = 0;
        err = sc_mcp_server_list_tools(srv, alloc, &names, &descs, &n);
        if (err != SC_OK || n == 0) {
            sc_mcp_server_destroy(srv);
            continue;
        }

        servers[i] = srv;
        counts[i] = n;
        total += n;

        for (size_t j = 0; j < n; j++) {
            size_t pref_len = 32 + strlen(names[j]);
            char *prefixed = (char *)alloc->alloc(alloc->ctx, pref_len);
            if (!prefixed) {
                for (size_t k = j; k < n; k++) {
                    alloc->free(alloc->ctx, names[k], strlen(names[k]) + 1);
                    alloc->free(alloc->ctx, descs[k], strlen(descs[k]) + 1);
                }
                alloc->free(alloc->ctx, names, n * sizeof(char *));
                alloc->free(alloc->ctx, descs, n * sizeof(char *));
                if (j == 0) sc_mcp_server_destroy(srv);
                servers[i] = NULL;
                goto fail;
            }
            snprintf(prefixed, pref_len, "mcp_%zu_%s", i, names[j]);

            sc_mcp_tool_wrapper_t *w = (sc_mcp_tool_wrapper_t *)alloc->alloc(
                alloc->ctx, sizeof(sc_mcp_tool_wrapper_t));
            if (!w) {
                alloc->free(alloc->ctx, prefixed, pref_len);
                for (size_t k = j; k < n; k++) {
                    alloc->free(alloc->ctx, names[k], strlen(names[k]) + 1);
                    alloc->free(alloc->ctx, descs[k], strlen(descs[k]) + 1);
                }
                alloc->free(alloc->ctx, names, n * sizeof(char *));
                alloc->free(alloc->ctx, descs, n * sizeof(char *));
                if (j == 0) sc_mcp_server_destroy(srv);
                servers[i] = NULL;
                goto fail;
            }
            w->alloc = alloc;
            w->server = srv;
            w->owns_server = (j == 0);
            w->original_name = names[j];
            w->prefixed_name = prefixed;
            w->desc = descs[j];
            w->params_json = (char *)alloc->alloc(alloc->ctx, 3);
            if (w->params_json) strcpy(w->params_json, "{}");
            w->server_index = i;

            sc_tool_t *tools_buf = (sc_tool_t *)alloc->realloc(alloc->ctx,
                all, (total - 1) * sizeof(sc_tool_t), total * sizeof(sc_tool_t));
            if (!tools_buf) {
                mcp_tool_deinit(w, alloc);  /* frees w, names[j], descs[j], prefixed; destroys server if j==0 */
                for (size_t k = j + 1; k < n; k++) {
                    alloc->free(alloc->ctx, names[k], strlen(names[k]) + 1);
                    alloc->free(alloc->ctx, descs[k], strlen(descs[k]) + 1);
                }
                alloc->free(alloc->ctx, names, n * sizeof(char *));
                alloc->free(alloc->ctx, descs, n * sizeof(char *));
                servers[i] = NULL;
                goto fail;
            }
            all = tools_buf;
            all[total - 1].ctx = w;
            all[total - 1].vtable = &mcp_tool_vtable;
        }
        alloc->free(alloc->ctx, names, n * sizeof(char *));
        alloc->free(alloc->ctx, descs, n * sizeof(char *));
    }

    alloc->free(alloc->ctx, servers, config_count * sizeof(sc_mcp_server_t *));
    alloc->free(alloc->ctx, counts, config_count * sizeof(size_t));
    *out_tools = all;
    *out_count = total;
    return SC_OK;

fail:
    if (all) {
        for (size_t i = 0; i < total; i++) {
            if (all[i].vtable && all[i].vtable->deinit)
                all[i].vtable->deinit(all[i].ctx, alloc);
        }
        alloc->free(alloc->ctx, all, total * sizeof(sc_tool_t));
    }
    for (size_t i = 0; i < config_count; i++) {
        if (servers && servers[i]) sc_mcp_server_destroy(servers[i]);
    }
    if (servers) alloc->free(alloc->ctx, servers, config_count * sizeof(sc_mcp_server_t *));
    if (counts) alloc->free(alloc->ctx, counts, config_count * sizeof(size_t));
    return SC_ERR_OUT_OF_MEMORY;
}

void sc_mcp_free_tools(sc_allocator_t *alloc, sc_tool_t *tools, size_t count)
{
    if (!alloc || !tools) return;
    for (size_t i = 0; i < count; i++) {
        if (tools[i].vtable && tools[i].vtable->deinit)
            tools[i].vtable->deinit(tools[i].ctx, alloc);
    }
    alloc->free(alloc->ctx, tools, count * sizeof(sc_tool_t));
}
