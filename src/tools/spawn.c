#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/security.h"
#include "human/security/sandbox.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_SPAWN_NAME "spawn"
#define HU_SPAWN_DESC "Spawn child process"
#define HU_SPAWN_PARAMS                                                                          \
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"args\":{\"type\":" \
    "\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"command\"]}"
#define HU_SPAWN_MAX_ARGS   64
#define HU_SPAWN_MAX_OUTPUT 65536

typedef struct hu_spawn_ctx {
    const char *workspace_dir;
    size_t workspace_dir_len;
    hu_security_policy_t *policy;
} hu_spawn_ctx_t;

static hu_error_t spawn_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                hu_tool_result_t *out) {
    hu_spawn_ctx_t *c = (hu_spawn_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *cmd = hu_json_get_string(args, "command");
    if (!cmd || strlen(cmd) == 0) {
        *out = hu_tool_result_fail("missing command", 14);
        return HU_OK;
    }
    {
        static const char *const SHELL_CMDS[] = {
            "/bin/sh",     "/bin/bash",     "/bin/zsh",     "/bin/csh", "/bin/ksh",
            "/usr/bin/sh", "/usr/bin/bash", "/usr/bin/zsh", "sh",       "bash",
            "zsh",         "csh",           "ksh",          NULL};
        for (const char *const *b = SHELL_CMDS; *b; b++) {
            if (strcmp(cmd, *b) == 0) {
                *out = hu_tool_result_fail(
                    "shell interpreters not allowed in spawn; use shell tool", 55);
                return HU_OK;
            }
        }
    }
#if HU_IS_TEST
    char *msg = hu_strndup(alloc, "(spawn disabled in test)", 24);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, 24);
    return HU_OK;
#else
#ifndef _WIN32
    if (c->policy && !hu_security_shell_allowed(c->policy)) {
        *out = hu_tool_result_fail("spawn not allowed by policy", 27);
        return HU_OK;
    }
    if (c->policy) {
        bool approved = c->policy->pre_approved;
        c->policy->pre_approved = false;
        hu_command_risk_level_t risk;
        hu_error_t perr = hu_policy_validate_command(c->policy, cmd, approved, &risk);
        if (perr == HU_ERR_SECURITY_APPROVAL_REQUIRED) {
            *out = hu_tool_result_fail("approval required", 17);
            out->needs_approval = true;
            return HU_OK;
        }
        if (perr != HU_OK) {
            *out = hu_tool_result_fail("command blocked by policy", 25);
            return HU_OK;
        }
    }

    /* Build argv: [command, args..., NULL] */
    const char *argv_buf[HU_SPAWN_MAX_ARGS + 2];
    char num_buf[32];
    size_t argc = 0;
    argv_buf[argc++] = cmd;

    hu_json_value_t *args_arr = hu_json_object_get(args, "args");
    if (args_arr && args_arr->type == HU_JSON_ARRAY && args_arr->data.array.len > 0) {
        size_t n = args_arr->data.array.len;
        if (n > HU_SPAWN_MAX_ARGS)
            n = HU_SPAWN_MAX_ARGS;
        for (size_t i = 0; i < n && argc < HU_SPAWN_MAX_ARGS + 1; i++) {
            hu_json_value_t *item = args_arr->data.array.items[i];
            if (!item)
                continue;
            if (item->type == HU_JSON_STRING && item->data.string.ptr) {
                argv_buf[argc++] = item->data.string.ptr;
            } else if (item->type == HU_JSON_NUMBER) {
                int r = snprintf(num_buf, sizeof(num_buf), "%.0f", item->data.number);
                if (r > 0 && (size_t)r < sizeof(num_buf)) {
                    char *dup = hu_strndup(alloc, num_buf, (size_t)r);
                    if (!dup) {
                        *out = hu_tool_result_fail("out of memory", 12);
                        return HU_ERR_OUT_OF_MEMORY;
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

    /* Wrap argv with sandbox if available */
    const char *sandbox_argv[HU_SPAWN_MAX_ARGS + 16];
    const char *const *run_argv = argv_buf;

    if (c->policy && c->policy->sandbox && hu_sandbox_is_available(c->policy->sandbox)) {
        size_t wrapped_count = 0;
        if (hu_sandbox_wrap_command(c->policy->sandbox, argv_buf, argc, sandbox_argv,
                                    HU_SPAWN_MAX_ARGS + 16, &wrapped_count) == HU_OK &&
            wrapped_count > 0) {
            sandbox_argv[wrapped_count] = NULL;
            run_argv = sandbox_argv;
        }
    }

    hu_run_result_t run = {0};
    hu_error_t err =
        hu_process_run_with_policy(alloc, run_argv, cwd, HU_SPAWN_MAX_OUTPUT, c->policy, &run);

    /* Free any number-arg copies we allocated */
    if (args_arr && args_arr->type == HU_JSON_ARRAY) {
        for (size_t i = 1; i < argc; i++) {
            hu_json_value_t *item =
                (i <= args_arr->data.array.len) ? args_arr->data.array.items[i - 1] : NULL;
            if (item && item->type == HU_JSON_NUMBER && argv_buf[i]) {
                hu_str_free(alloc, (char *)argv_buf[i]);
            }
        }
    }
    if (cwd)
        alloc->free(alloc->ctx, (void *)cwd, c->workspace_dir_len + 1);

    if (err != HU_OK) {
        hu_run_result_free(alloc, &run);
        *out = hu_tool_result_fail("spawn failed", 12);
        return HU_OK;
    }

    size_t out_len = run.stdout_len + (run.stderr_len ? run.stderr_len + 2 : 0) + 64;
    if (run.stderr_len && !run.success)
        out_len += 32;
    char *msg = (char *)alloc->alloc(alloc->ctx, out_len);
    if (!msg) {
        hu_run_result_free(alloc, &run);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
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
        if (run.stderr_len > 0)
            msg[off++] = '\n';
    }
    if (run.stderr_len > 0) {
        size_t n = run.stderr_len < out_len - off - 1 ? run.stderr_len : out_len - off - 1;
        memcpy(msg + off, run.stderr_buf, n);
        off += n;
    }
    msg[off] = '\0';

    hu_run_result_free(alloc, &run);

    *out = hu_tool_result_ok_owned(msg, off);
    return HU_OK;
#else
    (void)c;
    (void)alloc;
    *out = hu_tool_result_fail("spawn not supported on platform", 31);
    return HU_OK;
#endif
#endif
}

static const char *spawn_name(void *ctx) {
    (void)ctx;
    return HU_SPAWN_NAME;
}
static const char *spawn_description(void *ctx) {
    (void)ctx;
    return HU_SPAWN_DESC;
}
static const char *spawn_parameters_json(void *ctx) {
    (void)ctx;
    return HU_SPAWN_PARAMS;
}
static void spawn_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_spawn_ctx_t *c = (hu_spawn_ctx_t *)ctx;
    if (!c || !alloc)
        return;
    if (c->workspace_dir)
        alloc->free(alloc->ctx, (void *)c->workspace_dir, c->workspace_dir_len + 1);
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_tool_vtable_t spawn_vtable = {
    .execute = spawn_execute,
    .name = spawn_name,
    .description = spawn_description,
    .parameters_json = spawn_parameters_json,
    .deinit = spawn_deinit,
};

hu_error_t hu_spawn_create(hu_allocator_t *alloc, const char *workspace_dir,
                           size_t workspace_dir_len, hu_security_policy_t *policy, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_spawn_ctx_t *c = (hu_spawn_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    if (workspace_dir && workspace_dir_len > 0) {
        c->workspace_dir = hu_strndup(alloc, workspace_dir, workspace_dir_len);
        if (!c->workspace_dir) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->workspace_dir_len = workspace_dir_len;
    }
    c->policy = policy;
    out->ctx = c;
    out->vtable = &spawn_vtable;
    return HU_OK;
}
