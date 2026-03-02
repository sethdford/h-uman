#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <stdio.h>

#ifdef __linux__
#include <unistd.h>
static bool firejail_binary_exists(void) {
    if (access("/usr/bin/firejail", X_OK) == 0) return true;
    if (access("/usr/local/bin/firejail", X_OK) == 0) return true;
    return false;
}
#endif

static sc_error_t firejail_wrap(void *ctx, const char *const *argv, size_t argc,
    const char **buf, size_t buf_count, size_t *out_count) {
#ifndef __linux__
    (void)ctx; (void)argv; (void)argc; (void)buf; (void)buf_count; (void)out_count;
    return SC_ERR_NOT_SUPPORTED;
#else
    sc_firejail_ctx_t *fj = (sc_firejail_ctx_t *)ctx;
    const size_t base_prefix = 5;
    size_t extra = fj->extra_args_len;
    size_t total_prefix = base_prefix + extra;
    if (!buf || !out_count) return SC_ERR_INVALID_ARGUMENT;
    if (buf_count < total_prefix + argc) return SC_ERR_INVALID_ARGUMENT;

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
    return SC_OK;
#endif
}

static bool firejail_available(void *ctx) {
    (void)ctx;
#ifdef __linux__
#if SC_IS_TEST
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

static const sc_sandbox_vtable_t firejail_vtable = {
    .wrap_command = firejail_wrap,
    .apply = NULL,
    .is_available = firejail_available,
    .name = firejail_name,
    .description = firejail_desc,
};

sc_sandbox_t sc_firejail_sandbox_get(sc_firejail_ctx_t *ctx) {
    sc_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &firejail_vtable,
    };
    return sb;
}

void sc_firejail_sandbox_init(sc_firejail_ctx_t *ctx, const char *workspace_dir) {
    memset(ctx, 0, sizeof(*ctx));
    if (workspace_dir) {
        int n = snprintf(ctx->private_arg, 256,
            "--private=%s", workspace_dir);
        if (n > 0 && n < 256)
            ctx->private_len = (size_t)n;
        else {
            memcpy(ctx->private_arg, "--private", 10);
            ctx->private_len = 9;
        }
    }
}

void sc_firejail_sandbox_set_extra_args(sc_firejail_ctx_t *ctx,
    const char *const *args, size_t args_len) {
    if (!ctx) return;
    ctx->extra_args = args;
    ctx->extra_args_len = args_len;
}
