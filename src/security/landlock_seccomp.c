#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/core/error.h"
#include <string.h>

/*
 * Combined Landlock + seccomp-BPF sandbox (Linux only).
 *
 * Applies both sandboxes in sequence for Chrome-grade process isolation:
 *  1. Landlock: filesystem ACLs (restrict which paths are accessible)
 *  2. seccomp-BPF: syscall filtering (restrict which syscalls are allowed)
 *
 * The combination provides defense-in-depth: even if a process escapes the
 * filesystem restrictions, it cannot use dangerous syscalls like ptrace or mount.
 */

static sc_error_t landlock_seccomp_apply(void *ctx) {
#ifndef __linux__
    (void)ctx;
    return SC_ERR_NOT_SUPPORTED;
#else
    sc_landlock_seccomp_ctx_t *ls = (sc_landlock_seccomp_ctx_t *)ctx;

    /* Apply Landlock filesystem ACLs first */
    sc_sandbox_t ll_sb = sc_landlock_sandbox_get(&ls->landlock);
    if (ll_sb.vtable->apply) {
        sc_error_t err = ll_sb.vtable->apply(ll_sb.ctx);
        if (err != SC_OK && err != SC_ERR_NOT_SUPPORTED)
            return err;
    }

    /* Then apply seccomp syscall filter */
    sc_sandbox_t sc_sb = sc_seccomp_sandbox_get(&ls->seccomp);
    if (sc_sb.vtable->apply) {
        sc_error_t err = sc_sb.vtable->apply(sc_sb.ctx);
        if (err != SC_OK && err != SC_ERR_NOT_SUPPORTED)
            return err;
    }

    return SC_OK;
#endif
}

static sc_error_t landlock_seccomp_wrap(void *ctx,
    const char *const *argv, size_t argc,
    const char **buf, size_t buf_count, size_t *out_count) {
#ifndef __linux__
    (void)ctx; (void)argv; (void)argc; (void)buf; (void)buf_count; (void)out_count;
    return SC_ERR_NOT_SUPPORTED;
#else
    (void)ctx;
    if (!buf || !out_count) return SC_ERR_INVALID_ARGUMENT;
    if (buf_count < argc) return SC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < argc; i++)
        buf[i] = argv[i];
    *out_count = argc;
    return SC_OK;
#endif
}

static bool landlock_seccomp_available(void *ctx) {
    (void)ctx;
#ifdef __linux__
    sc_landlock_seccomp_ctx_t *ls = (sc_landlock_seccomp_ctx_t *)ctx;
    sc_sandbox_t ll = sc_landlock_sandbox_get(&ls->landlock);
    sc_sandbox_t sc = sc_seccomp_sandbox_get(&ls->seccomp);
    return ll.vtable->is_available(ll.ctx) || sc.vtable->is_available(sc.ctx);
#else
    return false;
#endif
}

static const char *landlock_seccomp_name(void *ctx) {
    (void)ctx;
    return "landlock+seccomp";
}

static const char *landlock_seccomp_desc(void *ctx) {
    (void)ctx;
#ifdef __linux__
    return "Combined Landlock + seccomp-BPF (Chrome-grade filesystem + syscall isolation)";
#else
    return "Combined Landlock + seccomp-BPF (not available on this platform)";
#endif
}

static const sc_sandbox_vtable_t landlock_seccomp_vtable = {
    .wrap_command = landlock_seccomp_wrap,
    .apply = landlock_seccomp_apply,
    .is_available = landlock_seccomp_available,
    .name = landlock_seccomp_name,
    .description = landlock_seccomp_desc,
};

sc_sandbox_t sc_landlock_seccomp_sandbox_get(sc_landlock_seccomp_ctx_t *ctx) {
    sc_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &landlock_seccomp_vtable,
    };
    return sb;
}

void sc_landlock_seccomp_sandbox_init(sc_landlock_seccomp_ctx_t *ctx,
    const char *workspace_dir, bool allow_network) {
    memset(ctx, 0, sizeof(*ctx));
    sc_landlock_sandbox_init(&ctx->landlock, workspace_dir);
    sc_seccomp_sandbox_init(&ctx->seccomp, workspace_dir, allow_network);
}
