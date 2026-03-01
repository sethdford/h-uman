#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/security.h"
#include "seaclaw/config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#endif

#define SC_SPAWN_NAME "spawn"
#define SC_SPAWN_DESC "Spawn child process"
#define SC_SPAWN_PARAMS "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"args\":{\"type\":\"array\"}},\"required\":[\"command\"]}"
#define SC_SPAWN_MAX_ARGS 64
#define SC_SPAWN_MAX_OUTPUT 65536

typedef struct sc_spawn_ctx {
    const char *workspace_dir;
    size_t workspace_dir_len;
    sc_security_policy_t *policy;
} sc_spawn_ctx_t;

static sc_error_t spawn_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    sc_spawn_ctx_t *c = (sc_spawn_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *cmd = sc_json_get_string(args, "command");
    if (!cmd || strlen(cmd) == 0) {
        *out = sc_tool_result_fail("missing command", 14);
        return SC_OK;
    }
#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(spawn disabled in test)", 24);
    if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    *out = sc_tool_result_ok_owned(msg, 24);
    return SC_OK;
#else
#ifndef _WIN32
    if (c->policy && !sc_security_shell_allowed(c->policy)) {
        *out = sc_tool_result_fail("spawn not allowed by policy", 27);
        return SC_OK;
    }
    if (c->policy) {
        sc_command_risk_level_t risk;
        sc_error_t perr = sc_policy_validate_command(c->policy, cmd, false, &risk);
        if (perr != SC_OK) {
            *out = sc_tool_result_fail("command blocked by policy", 25);
            return SC_OK;
        }
    }

    /* Build argv: [command, args..., NULL] */
    const char *argv_buf[SC_SPAWN_MAX_ARGS + 2];
    char num_buf[32];
    size_t argc = 0;
    argv_buf[argc++] = cmd;

    sc_json_value_t *args_arr = sc_json_object_get(args, "args");
    if (args_arr && args_arr->type == SC_JSON_ARRAY && args_arr->data.array.len > 0) {
        size_t n = args_arr->data.array.len;
        if (n > SC_SPAWN_MAX_ARGS) n = SC_SPAWN_MAX_ARGS;
        for (size_t i = 0; i < n && argc < SC_SPAWN_MAX_ARGS + 1; i++) {
            sc_json_value_t *item = args_arr->data.array.items[i];
            if (!item) continue;
            if (item->type == SC_JSON_STRING && item->data.string.ptr) {
                argv_buf[argc++] = item->data.string.ptr;
            } else if (item->type == SC_JSON_NUMBER) {
                int r = snprintf(num_buf, sizeof(num_buf), "%.0f", item->data.number);
                if (r > 0 && (size_t)r < sizeof(num_buf)) {
                    char *dup = sc_strndup(alloc, num_buf, (size_t)r);
                    if (!dup) {
                        *out = sc_tool_result_fail("out of memory", 12);
                        return SC_ERR_OUT_OF_MEMORY;
                    }
                    argv_buf[argc++] = dup;
                }
            }
        }
    }
    argv_buf[argc] = NULL;

    const char *cwd = NULL;
    if (c->workspace_dir && c->workspace_dir_len > 0) {
        char *wd = (char *)alloc->alloc(alloc->ctx, c->workspace_dir_len + 1);
        if (wd) {
            memcpy(wd, c->workspace_dir, c->workspace_dir_len);
            wd[c->workspace_dir_len] = '\0';
            cwd = wd;
        }
    }

    sc_run_result_t run = {0};
    sc_error_t err = sc_process_run(alloc, argv_buf, cwd, SC_SPAWN_MAX_OUTPUT, &run);

    /* Free any number-arg copies we allocated */
    if (args_arr && args_arr->type == SC_JSON_ARRAY) {
        for (size_t i = 1; i < argc; i++) {
            sc_json_value_t *item = (i <= args_arr->data.array.len) ?
                args_arr->data.array.items[i - 1] : NULL;
            if (item && item->type == SC_JSON_NUMBER && argv_buf[i]) {
                sc_str_free(alloc, (char *)argv_buf[i]);
            }
        }
    }
    if (cwd) alloc->free(alloc->ctx, (void *)cwd, c->workspace_dir_len + 1);

    if (err != SC_OK) {
        sc_run_result_free(alloc, &run);
        *out = sc_tool_result_fail("spawn failed", 12);
        return SC_OK;
    }

    size_t out_len = run.stdout_len + (run.stderr_len ? run.stderr_len + 2 : 0) + 64;
    if (run.stderr_len && !run.success) out_len += 32;
    char *msg = (char *)alloc->alloc(alloc->ctx, out_len);
    if (!msg) {
        sc_run_result_free(alloc, &run);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t off = 0;
    if (!run.success && run.exit_code >= 0) {
        off += (size_t)snprintf(msg + off, out_len - off, "exit code %d\n", run.exit_code);
    } else if (!run.success && run.exit_code < 0) {
        off += (size_t)snprintf(msg + off, out_len - off, "process terminated by signal\n");
    }
    if (run.stdout_len > 0) {
        size_t n = run.stdout_len < out_len - off - 1 ? run.stdout_len : out_len - off - 1;
        memcpy(msg + off, run.stdout_buf, n);
        off += n;
        if (run.stderr_len > 0) msg[off++] = '\n';
    }
    if (run.stderr_len > 0) {
        size_t n = run.stderr_len < out_len - off - 1 ? run.stderr_len : out_len - off - 1;
        memcpy(msg + off, run.stderr_buf, n);
        off += n;
    }
    msg[off] = '\0';

    sc_run_result_free(alloc, &run);

    *out = sc_tool_result_ok_owned(msg, off);
    return SC_OK;
#else
    (void)c;
    (void)alloc;
    *out = sc_tool_result_fail("spawn not supported on platform", 31);
    return SC_OK;
#endif
#endif
}

static const char *spawn_name(void *ctx) { (void)ctx; return SC_SPAWN_NAME; }
static const char *spawn_description(void *ctx) { (void)ctx; return SC_SPAWN_DESC; }
static const char *spawn_parameters_json(void *ctx) { (void)ctx; return SC_SPAWN_PARAMS; }
static void spawn_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t spawn_vtable = {
    .execute = spawn_execute, .name = spawn_name,
    .description = spawn_description, .parameters_json = spawn_parameters_json,
    .deinit = spawn_deinit,
};

sc_error_t sc_spawn_create(sc_allocator_t *alloc,
    const char *workspace_dir, size_t workspace_dir_len,
    sc_security_policy_t *policy,
    sc_tool_t *out)
{
    sc_spawn_ctx_t *c = (sc_spawn_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    if (workspace_dir && workspace_dir_len > 0) {
        c->workspace_dir = sc_strndup(alloc, workspace_dir, workspace_dir_len);
        if (!c->workspace_dir) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        c->workspace_dir_len = workspace_dir_len;
    }
    c->policy = policy;
    out->ctx = c;
    out->vtable = &spawn_vtable;
    return SC_OK;
}
