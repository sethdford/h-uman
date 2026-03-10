#include "human/core/error.h"
#include "human/security/sandbox.h"
#include "human/security/sandbox_internal.h"
#include <stdio.h>
#include <string.h>

#if defined(__linux__) && !HU_IS_TEST
#include <unistd.h>
static bool firejail_binary_exists(void) {
    if (access("/usr/bin/firejail", X_OK) == 0)
        return true;
    if (access("/usr/local/bin/firejail", X_OK) == 0)
        return true;
    return false;
}
#endif

static hu_error_t firejail_wrap(void *ctx, const char *const *argv, size_t argc, const char **buf,
                                size_t buf_count, size_t *out_count) {
#ifndef __linux__
    (void)ctx;
    (void)argv;
    (void)argc;
    (void)buf;
    (void)buf_count;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_firejail_ctx_t *fj = (hu_firejail_ctx_t *)ctx;
    const size_t base_prefix = 5;
    size_t extra = fj->extra_args_len;
    size_t total_prefix = base_prefix + extra;
    if (!buf || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    if (buf_count < total_prefix + argc)
        return HU_ERR_INVALID_ARGUMENT;

    buf[0] = "firejail";
    buf[1] = fj->private_arg;
    buf[2] = "--net=none";
    buf[3] = "--quiet";
    buf[4] = "--noprofile";
    for (size_t i = 0; i < extra; i++)
        buf[base_prefix + i] = fj->extra_args[i];
    for (size_t i = 0; i < argc; i++)
        buf[total_prefix + i] = argv[i];
    *out_count = total_prefix + argc;
    return HU_OK;
#endif
}

static bool firejail_available(void *ctx) {
    (void)ctx;
#ifdef __linux__
#if HU_IS_TEST
    return false; /* Skip binary check in tests; avoids CI dependency */
#else
    return firejail_binary_exists();
#endif
#else
    return false;
#endif
}

static const char *firejail_name(void *ctx) {
    (void)ctx;
    return "firejail";
}

static const char *firejail_desc(void *ctx) {
    (void)ctx;
    return "Linux user-space sandbox (requires firejail to be installed)";
}

static const hu_sandbox_vtable_t firejail_vtable = {
    .wrap_command = firejail_wrap,
    .apply = NULL,
    .is_available = firejail_available,
    .name = firejail_name,
    .description = firejail_desc,
};

hu_sandbox_t hu_firejail_sandbox_get(hu_firejail_ctx_t *ctx) {
    hu_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &firejail_vtable,
    };
    return sb;
}

void hu_firejail_sandbox_init(hu_firejail_ctx_t *ctx, const char *workspace_dir) {
    memset(ctx, 0, sizeof(*ctx));
    if (workspace_dir) {
        int n = snprintf(ctx->private_arg, 256, "--private=%s", workspace_dir);
        if (n > 0 && n < 256)
            ctx->private_len = (size_t)n;
        else {
            memcpy(ctx->private_arg, "--private", 10);
            ctx->private_len = 9;
        }
    }
}

void hu_firejail_sandbox_set_extra_args(hu_firejail_ctx_t *ctx, const char *const *args,
                                        size_t args_len) {
    if (!ctx)
        return;
    ctx->extra_args = args;
    ctx->extra_args_len = args_len;
}
