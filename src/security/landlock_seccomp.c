#include "human/core/error.h"
#include "human/security/sandbox.h"
#include "human/security/sandbox_internal.h"
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

static hu_error_t landlock_seccomp_apply(void *ctx) {
#ifndef __linux__
    (void)ctx;
    return HU_ERR_NOT_SUPPORTED;
#else
    hu_landlock_seccomp_ctx_t *ls = (hu_landlock_seccomp_ctx_t *)ctx;

    /* Apply Landlock filesystem ACLs first */
    hu_sandbox_t ll_sb = hu_landlock_sandbox_get(&ls->landlock);
    if (ll_sb.vtable->apply) {
        hu_error_t err = ll_sb.vtable->apply(ll_sb.ctx);
        if (err != HU_OK && err != HU_ERR_NOT_SUPPORTED)
            return err;
    }

    /* Then apply seccomp syscall filter */
    hu_sandbox_t hu_sb = hu_seccomp_sandbox_get(&ls->seccomp);
    if (hu_sb.vtable->apply) {
        hu_error_t err = hu_sb.vtable->apply(hu_sb.ctx);
        if (err != HU_OK && err != HU_ERR_NOT_SUPPORTED)
            return err;
    }

    return HU_OK;
#endif
}

static hu_error_t landlock_seccomp_wrap(void *ctx, const char *const *argv, size_t argc,
                                        const char **buf, size_t buf_count, size_t *out_count) {
#ifndef __linux__
    (void)ctx;
    (void)argv;
    (void)argc;
    (void)buf;
    (void)buf_count;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
#else
    (void)ctx;
    if (!buf || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    if (buf_count < argc)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < argc; i++)
        buf[i] = argv[i];
    *out_count = argc;
    return HU_OK;
#endif
}

static bool landlock_seccomp_available(void *ctx) {
    (void)ctx;
#ifdef __linux__
    hu_landlock_seccomp_ctx_t *ls = (hu_landlock_seccomp_ctx_t *)ctx;
    hu_sandbox_t ll = hu_landlock_sandbox_get(&ls->landlock);
    hu_sandbox_t sc = hu_seccomp_sandbox_get(&ls->seccomp);
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

static const hu_sandbox_vtable_t landlock_seccomp_vtable = {
    .wrap_command = landlock_seccomp_wrap,
    .apply = landlock_seccomp_apply,
    .is_available = landlock_seccomp_available,
    .name = landlock_seccomp_name,
    .description = landlock_seccomp_desc,
};

hu_sandbox_t hu_landlock_seccomp_sandbox_get(hu_landlock_seccomp_ctx_t *ctx) {
    hu_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &landlock_seccomp_vtable,
    };
    return sb;
}

void hu_landlock_seccomp_sandbox_init(hu_landlock_seccomp_ctx_t *ctx, const char *workspace_dir,
                                      bool allow_network) {
    memset(ctx, 0, sizeof(*ctx));
    hu_landlock_sandbox_init(&ctx->landlock, workspace_dir);
    hu_seccomp_sandbox_init(&ctx->seccomp, workspace_dir, allow_network);
}
