#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <stdio.h>

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
#if SC_IS_TEST
    return false;
#elif defined(__linux__) || defined(__APPLE__)
    if (access("/usr/local/bin/wasmtime", X_OK) == 0) return true;
    if (access("/usr/bin/wasmtime", X_OK) == 0) return true;
    if (access("/opt/homebrew/bin/wasmtime", X_OK) == 0) return true;
    /* Also check wasmer as fallback */
    if (access("/usr/local/bin/wasmer", X_OK) == 0) return true;
    if (access("/usr/bin/wasmer", X_OK) == 0) return true;
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

static sc_error_t wasi_wrap(void *ctx, const char *const *argv, size_t argc,
    const char **buf, size_t buf_count, size_t *out_count) {
    sc_wasi_sandbox_ctx_t *wc = (sc_wasi_sandbox_ctx_t *)ctx;
    /*
     * wasmtime run --dir=WORKSPACE --dir=/tmp <argv...>
     * 4 prefix args: runtime, "run", --dir=workspace, --dir=/tmp
     */
    const size_t prefix_len = 4;
    if (!buf || !out_count) return SC_ERR_INVALID_ARGUMENT;
    if (buf_count < prefix_len + argc) return SC_ERR_INVALID_ARGUMENT;

    buf[0] = wc->runtime_path;
    buf[1] = "run";
    /* Use workspace_dir for the --dir= arg; pre-formatted in init */
    static char dir_arg[1040];
    static char tmp_arg[] = "--dir=/tmp";
    int n = snprintf(dir_arg, sizeof(dir_arg), "--dir=%s", wc->workspace_dir);
    if (n <= 0 || (size_t)n >= sizeof(dir_arg))
        return SC_ERR_INVALID_ARGUMENT;

    buf[2] = dir_arg;
    buf[3] = tmp_arg;
    for (size_t i = 0; i < argc; i++)
        buf[prefix_len + i] = argv[i];
    *out_count = prefix_len + argc;
    return SC_OK;
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

static const sc_sandbox_vtable_t wasi_vtable = {
    .wrap_command = wasi_wrap,
    .apply = NULL,
    .is_available = wasi_available,
    .name = wasi_name,
    .description = wasi_desc,
};

sc_sandbox_t sc_wasi_sandbox_get(sc_wasi_sandbox_ctx_t *ctx) {
    sc_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &wasi_vtable,
    };
    return sb;
}

void sc_wasi_sandbox_init(sc_wasi_sandbox_ctx_t *ctx, const char *workspace_dir) {
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
