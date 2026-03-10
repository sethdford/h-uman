#include "human/core/error.h"
#include "human/security/sandbox.h"
#include "human/security/sandbox_internal.h"
#include <string.h>

#ifdef __linux__
#include <unistd.h>
static bool bwrap_binary_exists(void) {
    if (access("/usr/bin/bwrap", X_OK) == 0)
        return true;
    if (access("/usr/local/bin/bwrap", X_OK) == 0)
        return true;
    return false;
}
#endif

static hu_error_t bubblewrap_wrap(void *ctx, const char *const *argv, size_t argc, const char **buf,
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
    hu_bubblewrap_ctx_t *bw = (hu_bubblewrap_ctx_t *)ctx;
    /* bwrap --ro-bind /usr /usr --dev /dev --proc /proc --bind /tmp /tmp
       --bind WORKSPACE /workspace --unshare-all --die-with-parent <argv...> */
    const char *prefix[] = {
        "bwrap",           "--ro-bind",  "/usr",          "/usr",
        "--dev",           "/dev",       "--proc",        "/proc",
        "--bind",          "/tmp",       "/tmp",          "--bind",
        bw->workspace_dir, "/workspace", "--unshare-all", "--die-with-parent",
    };
    const size_t prefix_len = sizeof(prefix) / sizeof(prefix[0]);

    if (!buf || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    if (buf_count < prefix_len + argc)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < prefix_len; i++)
        buf[i] = prefix[i];
    for (size_t i = 0; i < argc; i++)
        buf[prefix_len + i] = argv[i];
    *out_count = prefix_len + argc;
    return HU_OK;
#endif
}

static bool bubblewrap_available(void *ctx) {
    (void)ctx;
#if defined(__linux__) && defined(HU_GATEWAY_POSIX)
#if HU_IS_TEST
    return true; /* Skip binary check in tests */
#else
    return bwrap_binary_exists();
#endif
#else
    return false;
#endif
}

static const char *bubblewrap_name(void *ctx) {
    (void)ctx;
    return "bubblewrap";
}

static const char *bubblewrap_desc(void *ctx) {
    (void)ctx;
    return "User namespace sandbox (requires bwrap)";
}

static const hu_sandbox_vtable_t bubblewrap_vtable = {
    .wrap_command = bubblewrap_wrap,
    .apply = NULL,
    .is_available = bubblewrap_available,
    .name = bubblewrap_name,
    .description = bubblewrap_desc,
};

hu_sandbox_t hu_bubblewrap_sandbox_get(hu_bubblewrap_ctx_t *ctx) {
    hu_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &bubblewrap_vtable,
    };
    return sb;
}

void hu_bubblewrap_sandbox_init(hu_bubblewrap_ctx_t *ctx, const char *workspace_dir) {
    memset(ctx, 0, sizeof(*ctx));
    if (workspace_dir) {
        size_t len = strlen(workspace_dir);
        if (len >= sizeof(ctx->workspace_dir))
            len = sizeof(ctx->workspace_dir) - 1;
        memcpy(ctx->workspace_dir, workspace_dir, len);
        ctx->workspace_dir[len] = '\0';
    }
}
