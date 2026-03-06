/*
 * Firebase CLI tool — execute Firebase CLI commands.
 */
#include "seaclaw/tools/firebase.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/string.h"
#include "seaclaw/security.h"
#include "seaclaw/tool.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_FIREBASE_NAME "firebase"
#define SC_FIREBASE_DESC "Execute Firebase CLI commands"
#define SC_FIREBASE_PARAMS                                                                   \
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[" \
    "\"command\"]}"
#define SC_FIREBASE_MAX_ARGS   64
#define SC_FIREBASE_MAX_OUTPUT 1048576

typedef struct sc_firebase_ctx {
    sc_security_policy_t *policy;
} sc_firebase_ctx_t;

static bool firebase_sanitize_command(const char *cmd) {
    if (!cmd)
        return true;
    if (strstr(cmd, "$(") || strchr(cmd, '`') || strchr(cmd, '|') || strchr(cmd, ';'))
        return false;
    return true;
}

#if !SC_IS_TEST
static size_t firebase_split_args(char *cmd, const char **argv_out, size_t max_out) {
    size_t argc = 0;
    char *p = cmd;
    while (*p && argc < max_out) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        char *start = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (*p) {
            *p = '\0';
            p++;
        }
        argv_out[argc++] = start;
    }
    return argc;
}
#endif /* !SC_IS_TEST */

static sc_error_t firebase_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                   sc_tool_result_t *out) {
    sc_firebase_ctx_t *c = (sc_firebase_ctx_t *)ctx;
    if (!c || !args || !out)
        return SC_ERR_INVALID_ARGUMENT;
    const char *cmd = sc_json_get_string(args, "command");
    if (!cmd || strlen(cmd) == 0) {
        *out = sc_tool_result_fail("Missing 'command'", 16);
        return SC_OK;
    }
    if (!firebase_sanitize_command(cmd)) {
        *out = sc_tool_result_fail("Unsafe command characters detected", 33);
        return SC_OK;
    }

#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(firebase stub in test)", 23);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, 23);
    return SC_OK;
#else
    const char *argv_buf[SC_FIREBASE_MAX_ARGS + 2];
    size_t argc = 0;
    argv_buf[argc++] = "firebase";

    size_t cmd_len = strlen(cmd) + 1;
    char *cmd_copy = (char *)alloc->alloc(alloc->ctx, cmd_len);
    if (!cmd_copy) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memcpy(cmd_copy, cmd, cmd_len);
    size_t n = firebase_split_args(cmd_copy, argv_buf + 1, SC_FIREBASE_MAX_ARGS);
    for (size_t i = 1; i <= n; i++) {
        char *dup = sc_strdup(alloc, argv_buf[i]);
        if (!dup) {
            for (size_t j = 1; j < i; j++)
                alloc->free(alloc->ctx, (void *)argv_buf[j], strlen(argv_buf[j]) + 1);
            alloc->free(alloc->ctx, cmd_copy, cmd_len);
            *out = sc_tool_result_fail("out of memory", 12);
            return SC_ERR_OUT_OF_MEMORY;
        }
        argv_buf[argc++] = dup;
    }
    alloc->free(alloc->ctx, cmd_copy, cmd_len);
    argv_buf[argc] = NULL;

    sc_run_result_t run = {0};
    sc_error_t err = sc_process_run_with_policy(alloc, (const char *const *)argv_buf, NULL,
                                                SC_FIREBASE_MAX_OUTPUT, c->policy, &run);

    for (size_t i = 1; i < argc; i++)
        alloc->free(alloc->ctx, (void *)argv_buf[i], strlen(argv_buf[i]) + 1);

    if (err != SC_OK) {
        sc_run_result_free(alloc, &run);
        *out = sc_tool_result_fail("firebase execution failed", 26);
        return SC_OK;
    }

    size_t out_len = run.stdout_len + (run.stderr_len ? run.stderr_len + 2 : 0) + 64;
    char *msg = (char *)alloc->alloc(alloc->ctx, out_len);
    if (!msg) {
        sc_run_result_free(alloc, &run);
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t off = 0;
    if (!run.success && run.exit_code >= 0)
        off += (size_t)snprintf(msg + off, out_len - off, "exit code %d\n", run.exit_code);
    if (run.stdout_len > 0) {
        size_t ncpy = run.stdout_len < out_len - off - 1 ? run.stdout_len : out_len - off - 1;
        memcpy(msg + off, run.stdout_buf, ncpy);
        off += ncpy;
    }
    if (run.stderr_len > 0 && off < out_len - 1) {
        msg[off++] = '\n';
        size_t ncpy = run.stderr_len < out_len - off - 1 ? run.stderr_len : out_len - off - 1;
        memcpy(msg + off, run.stderr_buf, ncpy);
        off += ncpy;
    }
    msg[off] = '\0';
    sc_run_result_free(alloc, &run);
    *out = sc_tool_result_ok_owned(msg, off);
    return SC_OK;
#endif
}

static const char *firebase_name(void *ctx) {
    (void)ctx;
    return SC_FIREBASE_NAME;
}
static const char *firebase_description(void *ctx) {
    (void)ctx;
    return SC_FIREBASE_DESC;
}
static const char *firebase_parameters_json(void *ctx) {
    (void)ctx;
    return SC_FIREBASE_PARAMS;
}
static void firebase_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    if (ctx)
        free(ctx);
}

static const sc_tool_vtable_t firebase_vtable = {
    .execute = firebase_execute,
    .name = firebase_name,
    .description = firebase_description,
    .parameters_json = firebase_parameters_json,
    .deinit = firebase_deinit,
};

sc_error_t sc_firebase_create(sc_allocator_t *alloc, sc_security_policy_t *policy, sc_tool_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    sc_firebase_ctx_t *c = (sc_firebase_ctx_t *)calloc(1, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    c->policy = policy;
    out->ctx = c;
    out->vtable = &firebase_vtable;
    return SC_OK;
}
