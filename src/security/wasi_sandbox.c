#include "human/core/error.h"
#include "human/security/sandbox.h"
#include "human/security/sandbox_internal.h"
#include <stdio.h>
#include <string.h>

/*
 * WASI sandbox: wraps commands with a WebAssembly runtime (wasmtime or wasmer).
 *
 * Provides cross-platform capability-based isolation. The WASI security model:
 *  - No filesystem access unless explicitly granted (--dir flags)
 *  - No network access unless explicitly granted
 *  - No environment variable leakage
 *  - Deterministic execution
 *
 * Requires the target binary to be compiled to .wasm (WASI target).
 * The wrap_command produces: wasmtime run --dir=WORKSPACE <argv...>
 */

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

static bool wasi_runtime_exists(void) {
#if HU_IS_TEST
    return false;
#elif defined(__linux__) || defined(__APPLE__)
    if (access("/usr/local/bin/wasmtime", X_OK) == 0)
        return true;
    if (access("/usr/bin/wasmtime", X_OK) == 0)
        return true;
    if (access("/opt/homebrew/bin/wasmtime", X_OK) == 0)
        return true;
    /* Also check wasmer as fallback */
    if (access("/usr/local/bin/wasmer", X_OK) == 0)
        return true;
    if (access("/usr/bin/wasmer", X_OK) == 0)
        return true;
    return false;
#else
    return false;
#endif
}

static const char *find_wasi_runtime(void) {
#if defined(__linux__) || defined(__APPLE__)
    if (access("/usr/local/bin/wasmtime", X_OK) == 0)
        return "/usr/local/bin/wasmtime";
    if (access("/usr/bin/wasmtime", X_OK) == 0)
        return "/usr/bin/wasmtime";
    if (access("/opt/homebrew/bin/wasmtime", X_OK) == 0)
        return "/opt/homebrew/bin/wasmtime";
    if (access("/usr/local/bin/wasmer", X_OK) == 0)
        return "/usr/local/bin/wasmer";
    if (access("/usr/bin/wasmer", X_OK) == 0)
        return "/usr/bin/wasmer";
#endif
    return "wasmtime";
}

static hu_error_t wasi_wrap(void *ctx, const char *const *argv, size_t argc, const char **buf,
                            size_t buf_count, size_t *out_count) {
    hu_wasi_sandbox_ctx_t *wc = (hu_wasi_sandbox_ctx_t *)ctx;
    const size_t prefix_len = 4;
    if (!buf || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    if (buf_count < prefix_len + argc)
        return HU_ERR_INVALID_ARGUMENT;
    if (!wc->workspace_dir[0])
        return HU_ERR_INVALID_ARGUMENT;

    buf[0] = wc->runtime_path;
    buf[1] = "run";
    /* Use workspace_dir for the --dir= arg; pre-formatted in init */
    int n = snprintf(wc->dir_arg, sizeof(wc->dir_arg), "--dir=%s", wc->workspace_dir);
    if (n <= 0 || (size_t)n >= sizeof(wc->dir_arg))
        return HU_ERR_INVALID_ARGUMENT;

    buf[2] = wc->dir_arg;
    buf[3] = "--dir=/tmp";
    for (size_t i = 0; i < argc; i++)
        buf[prefix_len + i] = argv[i];
    *out_count = prefix_len + argc;
    return HU_OK;
}

static bool wasi_available(void *ctx) {
    (void)ctx;
    return wasi_runtime_exists();
}

static const char *wasi_name(void *ctx) {
    (void)ctx;
    return "wasi";
}

static const char *wasi_desc(void *ctx) {
    (void)ctx;
    return "WASI sandbox (cross-platform capability-based isolation via wasmtime/wasmer)";
}

static const hu_sandbox_vtable_t wasi_vtable = {
    .wrap_command = wasi_wrap,
    .apply = NULL,
    .is_available = wasi_available,
    .name = wasi_name,
    .description = wasi_desc,
};

hu_sandbox_t hu_wasi_sandbox_get(hu_wasi_sandbox_ctx_t *ctx) {
    hu_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &wasi_vtable,
    };
    return sb;
}

void hu_wasi_sandbox_init(hu_wasi_sandbox_ctx_t *ctx, const char *workspace_dir) {
    memset(ctx, 0, sizeof(*ctx));
    if (workspace_dir) {
        size_t len = strlen(workspace_dir);
        if (len >= sizeof(ctx->workspace_dir))
            len = sizeof(ctx->workspace_dir) - 1;
        memcpy(ctx->workspace_dir, workspace_dir, len);
        ctx->workspace_dir[len] = '\0';
    }
    const char *rt = find_wasi_runtime();
    size_t rlen = strlen(rt);
    if (rlen >= sizeof(ctx->runtime_path))
        rlen = sizeof(ctx->runtime_path) - 1;
    memcpy(ctx->runtime_path, rt, rlen);
    ctx->runtime_path[rlen] = '\0';
}
