#include "seaclaw/core/error.h"
#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
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

static sc_error_t bubblewrap_wrap(void *ctx, const char *const *argv, size_t argc, const char **buf,
                                  size_t buf_count, size_t *out_count) {
#ifndef __linux__
    (void)ctx;
    (void)argv;
    (void)argc;
    (void)buf;
    (void)buf_count;
    (void)out_count;
    return SC_ERR_NOT_SUPPORTED;
#else
    sc_bubblewrap_ctx_t *bw = (sc_bubblewrap_ctx_t *)ctx;
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
        return SC_ERR_INVALID_ARGUMENT;
    if (buf_count < prefix_len + argc)
        return SC_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < prefix_len; i++)
        buf[i] = prefix[i];
    for (size_t i = 0; i < argc; i++)
        buf[prefix_len + i] = argv[i];
    *out_count = prefix_len + argc;
    return SC_OK;
#endif
}

static bool bubblewrap_available(void *ctx) {
    (void)ctx;
#ifdef __linux__
#if SC_IS_TEST
    return false; /* Skip binary check in tests; avoids CI dependency */
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

static const sc_sandbox_vtable_t bubblewrap_vtable = {
    .wrap_command = bubblewrap_wrap,
    .apply = NULL,
    .is_available = bubblewrap_available,
    .name = bubblewrap_name,
    .description = bubblewrap_desc,
};

sc_sandbox_t sc_bubblewrap_sandbox_get(sc_bubblewrap_ctx_t *ctx) {
    sc_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &bubblewrap_vtable,
    };
    return sb;
}

void sc_bubblewrap_sandbox_init(sc_bubblewrap_ctx_t *ctx, const char *workspace_dir) {
    memset(ctx, 0, sizeof(*ctx));
    if (workspace_dir) {
        size_t len = strlen(workspace_dir);
        if (len >= sizeof(ctx->workspace_dir))
            len = sizeof(ctx->workspace_dir) - 1;
        memcpy(ctx->workspace_dir, workspace_dir, len);
        ctx->workspace_dir[len] = '\0';
    }
}
