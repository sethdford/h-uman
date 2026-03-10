/*
 * Firebase CLI tool — execute Firebase CLI commands.
 */
#include "human/tools/firebase.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/security.h"
#include "human/tool.h"
#include "human/tools/cli_wrapper_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_FIREBASE_NAME "firebase"
#define HU_FIREBASE_DESC "Execute Firebase CLI commands"
#define HU_FIREBASE_PARAMS                                                                   \
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[" \
    "\"command\"]}"
#define HU_FIREBASE_MAX_ARGS   64
#define HU_FIREBASE_MAX_OUTPUT 1048576

typedef struct hu_firebase_ctx {
    hu_security_policy_t *policy;
} hu_firebase_ctx_t;

static hu_error_t firebase_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                   hu_tool_result_t *out) {
    hu_firebase_ctx_t *c = (hu_firebase_ctx_t *)ctx;
    if (!c || !args || !out)
        return HU_ERR_INVALID_ARGUMENT;
    const char *cmd = hu_json_get_string(args, "command");
    if (!cmd || strlen(cmd) == 0) {
        *out = hu_tool_result_fail("Missing 'command'", 16);
        return HU_OK;
    }
    if (!hu_cli_sanitize_command(cmd)) {
        *out = hu_tool_result_fail("Unsafe command characters detected", 33);
        return HU_OK;
    }

#if HU_IS_TEST
    char *msg = hu_strndup(alloc, "(firebase stub in test)", 23);
    if (!msg) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(msg, 23);
    return HU_OK;
#else
    const char *argv_buf[HU_FIREBASE_MAX_ARGS + 2];
    size_t argc = 0;
    argv_buf[argc++] = "firebase";

    size_t cmd_len = strlen(cmd) + 1;
    char *cmd_copy = (char *)alloc->alloc(alloc->ctx, cmd_len);
    if (!cmd_copy) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(cmd_copy, cmd, cmd_len);
    size_t n = hu_cli_split_args(cmd_copy, argv_buf + 1, HU_FIREBASE_MAX_ARGS);
    for (size_t i = 1; i <= n; i++) {
        char *dup = hu_strdup(alloc, argv_buf[i]);
        if (!dup) {
            for (size_t j = 1; j < i; j++)
                alloc->free(alloc->ctx, (void *)argv_buf[j], strlen(argv_buf[j]) + 1);
            alloc->free(alloc->ctx, cmd_copy, cmd_len);
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        argv_buf[argc++] = dup;
    }
    alloc->free(alloc->ctx, cmd_copy, cmd_len);
    argv_buf[argc] = NULL;

    hu_run_result_t run = {0};
    hu_error_t err = hu_process_run_with_policy(alloc, (const char *const *)argv_buf, NULL,
                                                HU_FIREBASE_MAX_OUTPUT, c->policy, &run);

    for (size_t i = 1; i < argc; i++)
        alloc->free(alloc->ctx, (void *)argv_buf[i], strlen(argv_buf[i]) + 1);

    if (err != HU_OK) {
        hu_run_result_free(alloc, &run);
        *out = hu_tool_result_fail("firebase execution failed", 26);
        return HU_OK;
    }

    size_t out_len = run.stdout_len + (run.stderr_len ? run.stderr_len + 2 : 0) + 64;
    char *msg = (char *)alloc->alloc(alloc->ctx, out_len);
    if (!msg) {
        hu_run_result_free(alloc, &run);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
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
    hu_run_result_free(alloc, &run);
    *out = hu_tool_result_ok_owned(msg, off);
    return HU_OK;
#endif
}

static const char *firebase_name(void *ctx) {
    (void)ctx;
    return HU_FIREBASE_NAME;
}
static const char *firebase_description(void *ctx) {
    (void)ctx;
    return HU_FIREBASE_DESC;
}
static const char *firebase_parameters_json(void *ctx) {
    (void)ctx;
    return HU_FIREBASE_PARAMS;
}
static void firebase_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(hu_firebase_ctx_t));
}

static const hu_tool_vtable_t firebase_vtable = {
    .execute = firebase_execute,
    .name = firebase_name,
    .description = firebase_description,
    .parameters_json = firebase_parameters_json,
    .deinit = firebase_deinit,
};

hu_error_t hu_firebase_create(hu_allocator_t *alloc, hu_security_policy_t *policy, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_firebase_ctx_t *c = (hu_firebase_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->policy = policy;
    out->ctx = c;
    out->vtable = &firebase_vtable;
    return HU_OK;
}
