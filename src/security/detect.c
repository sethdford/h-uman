#include "human/core/error.h"
#include "human/security/sandbox.h"
#include "human/security/sandbox_internal.h"
#include <stdlib.h>
#include <string.h>

struct hu_sandbox_storage {
    hu_noop_sandbox_ctx_t noop;
    hu_docker_ctx_t docker;
    hu_wasi_sandbox_ctx_t wasi;
#ifdef __APPLE__
    hu_seatbelt_ctx_t seatbelt;
#endif
#if defined(__linux__)
    hu_landlock_ctx_t landlock;
    hu_firejail_ctx_t firejail;
    hu_bubblewrap_ctx_t bubblewrap;
    hu_seccomp_ctx_t seccomp;
    hu_landlock_seccomp_ctx_t landlock_seccomp;
    hu_firecracker_ctx_t firecracker;
#endif
#ifdef _WIN32
    hu_appcontainer_ctx_t appcontainer;
#endif
};

hu_sandbox_storage_t *hu_sandbox_storage_create(const hu_sandbox_alloc_t *alloc) {
    if (!alloc || !alloc->alloc)
        return NULL;
    void *p = alloc->alloc(alloc->ctx, sizeof(struct hu_sandbox_storage));
    return (hu_sandbox_storage_t *)p;
}

void hu_sandbox_storage_destroy(hu_sandbox_storage_t *s, const hu_sandbox_alloc_t *alloc) {
    if (!s || !alloc || !alloc->free)
        return;
    alloc->free(alloc->ctx, s, sizeof(struct hu_sandbox_storage));
}

hu_sandbox_t hu_sandbox_create(hu_sandbox_backend_t backend, const char *workspace_dir,
                               hu_sandbox_storage_t *storage, const hu_sandbox_alloc_t *alloc) {
    if (!storage) {
        hu_sandbox_t empty = {.ctx = NULL, .vtable = NULL};
        return empty;
    }

    struct hu_sandbox_storage *st = (struct hu_sandbox_storage *)storage;
    hu_sandbox_t result = {.ctx = NULL, .vtable = NULL};
    const char *ws = workspace_dir ? workspace_dir : "/tmp";

    switch (backend) {
    case HU_SANDBOX_NONE: {
        result = hu_noop_sandbox_get(&st->noop);
        break;
    }
#if defined(__linux__)
    case HU_SANDBOX_LANDLOCK: {
        hu_landlock_sandbox_init(&st->landlock, ws);
        result = hu_landlock_sandbox_get(&st->landlock);
        if (!result.vtable->is_available(result.ctx))
            result = hu_noop_sandbox_get(&st->noop);
        break;
    }
    case HU_SANDBOX_FIREJAIL: {
        hu_firejail_sandbox_init(&st->firejail, ws);
        result = hu_firejail_sandbox_get(&st->firejail);
        if (!result.vtable->is_available(result.ctx))
            result = hu_noop_sandbox_get(&st->noop);
        break;
    }
    case HU_SANDBOX_BUBBLEWRAP: {
        hu_bubblewrap_sandbox_init(&st->bubblewrap, ws);
        result = hu_bubblewrap_sandbox_get(&st->bubblewrap);
        if (!result.vtable->is_available(result.ctx))
            result = hu_noop_sandbox_get(&st->noop);
        break;
    }
#endif
    case HU_SANDBOX_DOCKER: {
        if (!alloc || !alloc->alloc || !alloc->free)
            break;
        hu_docker_sandbox_init(&st->docker, ws, "alpine:latest", alloc->ctx, alloc->alloc,
                               alloc->free);
        result = hu_docker_sandbox_get(&st->docker);
        break;
    }
#ifdef __APPLE__
    case HU_SANDBOX_SEATBELT: {
        hu_seatbelt_sandbox_init(&st->seatbelt, ws);
        result = hu_seatbelt_sandbox_get(&st->seatbelt);
        if (!result.vtable->is_available(result.ctx))
            result = hu_noop_sandbox_get(&st->noop);
        break;
    }
#endif
#if defined(__linux__)
    case HU_SANDBOX_SECCOMP: {
        hu_seccomp_sandbox_init(&st->seccomp, ws, false);
        result = hu_seccomp_sandbox_get(&st->seccomp);
        if (!result.vtable->is_available(result.ctx))
            result = hu_noop_sandbox_get(&st->noop);
        break;
    }
#endif
    case HU_SANDBOX_WASI: {
        hu_wasi_sandbox_init(&st->wasi, ws);
        result = hu_wasi_sandbox_get(&st->wasi);
        if (!result.vtable->is_available(result.ctx))
            result = hu_noop_sandbox_get(&st->noop);
        break;
    }
#if defined(__linux__)
    case HU_SANDBOX_LANDLOCK_SECCOMP: {
        hu_landlock_seccomp_sandbox_init(&st->landlock_seccomp, ws, false);
        result = hu_landlock_seccomp_sandbox_get(&st->landlock_seccomp);
        if (!result.vtable->is_available(result.ctx))
            result = hu_noop_sandbox_get(&st->noop);
        break;
    }
    case HU_SANDBOX_FIRECRACKER: {
        hu_firecracker_sandbox_init(&st->firecracker, ws);
        result = hu_firecracker_sandbox_get(&st->firecracker);
        if (!result.vtable->is_available(result.ctx))
            result = hu_noop_sandbox_get(&st->noop);
        break;
    }
#endif
#ifdef _WIN32
    case HU_SANDBOX_APPCONTAINER: {
        hu_appcontainer_sandbox_init(&st->appcontainer, ws);
        result = hu_appcontainer_sandbox_get(&st->appcontainer);
        if (!result.vtable->is_available(result.ctx))
            result = hu_noop_sandbox_get(&st->noop);
        break;
    }
#endif
    case HU_SANDBOX_AUTO: {
        /*
         * Tiered auto-detection:
         *
         * macOS:   Seatbelt (kernel) -> Docker -> WASI -> noop
         * Linux:   Landlock+seccomp (combined) -> Bubblewrap -> Firejail
         *          -> Firecracker (KVM) -> Docker -> WASI -> noop
         * Windows: AppContainer -> WASI -> noop
         */
#ifdef __APPLE__
        hu_seatbelt_sandbox_init(&st->seatbelt, ws);
        result = hu_seatbelt_sandbox_get(&st->seatbelt);
        if (result.vtable->is_available(result.ctx))
            break;
#endif

#ifdef __linux__
        /* Tier 1: Kernel-level combined (Landlock FS ACLs + seccomp syscall filter) */
        hu_landlock_seccomp_sandbox_init(&st->landlock_seccomp, ws, false);
        result = hu_landlock_seccomp_sandbox_get(&st->landlock_seccomp);
        if (result.vtable->is_available(result.ctx))
            break;

        /* Tier 2: User-space namespace isolation */
        hu_bubblewrap_sandbox_init(&st->bubblewrap, ws);
        result = hu_bubblewrap_sandbox_get(&st->bubblewrap);
        if (result.vtable->is_available(result.ctx))
            break;

        hu_firejail_sandbox_init(&st->firejail, ws);
        result = hu_firejail_sandbox_get(&st->firejail);
        if (result.vtable->is_available(result.ctx))
            break;

        /* Tier 3: MicroVM (requires KVM) */
        hu_firecracker_sandbox_init(&st->firecracker, ws);
        result = hu_firecracker_sandbox_get(&st->firecracker);
        if (result.vtable->is_available(result.ctx))
            break;
#endif

#ifdef _WIN32
        hu_appcontainer_sandbox_init(&st->appcontainer, ws);
        result = hu_appcontainer_sandbox_get(&st->appcontainer);
        if (result.vtable->is_available(result.ctx))
            break;
#endif

        /* Cross-platform fallbacks */
        if (alloc && alloc->alloc && alloc->free) {
            hu_docker_sandbox_init(&st->docker, ws, "alpine:latest", alloc->ctx, alloc->alloc,
                                   alloc->free);
            result = hu_docker_sandbox_get(&st->docker);
            if (result.vtable->is_available(result.ctx))
                break;
        }

        hu_wasi_sandbox_init(&st->wasi, ws);
        result = hu_wasi_sandbox_get(&st->wasi);
        if (result.vtable->is_available(result.ctx))
            break;

        result = hu_noop_sandbox_get(&st->noop);
        break;
    }
    default:
        result = hu_noop_sandbox_get(&st->noop);
        break;
    }
    return result;
}

hu_available_backends_t hu_sandbox_detect_available(const char *workspace_dir,
                                                    const hu_sandbox_alloc_t *alloc) {
    hu_available_backends_t out = {0};
    hu_sandbox_storage_t *st = alloc && alloc->alloc ? hu_sandbox_storage_create(alloc) : NULL;
    if (!st)
        return out;

    const char *ws = workspace_dir ? workspace_dir : "/tmp";
    hu_sandbox_t sb;

#ifdef __linux__
    hu_landlock_sandbox_init(&((struct hu_sandbox_storage *)st)->landlock, ws);
    sb = hu_landlock_sandbox_get(&((struct hu_sandbox_storage *)st)->landlock);
    out.landlock = sb.vtable->is_available(sb.ctx);

    hu_firejail_sandbox_init(&((struct hu_sandbox_storage *)st)->firejail, ws);
    sb = hu_firejail_sandbox_get(&((struct hu_sandbox_storage *)st)->firejail);
    out.firejail = sb.vtable->is_available(sb.ctx);

    hu_bubblewrap_sandbox_init(&((struct hu_sandbox_storage *)st)->bubblewrap, ws);
    sb = hu_bubblewrap_sandbox_get(&((struct hu_sandbox_storage *)st)->bubblewrap);
    out.bubblewrap = sb.vtable->is_available(sb.ctx);

    hu_seccomp_sandbox_init(&((struct hu_sandbox_storage *)st)->seccomp, ws, false);
    sb = hu_seccomp_sandbox_get(&((struct hu_sandbox_storage *)st)->seccomp);
    out.seccomp = sb.vtable->is_available(sb.ctx);

    hu_firecracker_sandbox_init(&((struct hu_sandbox_storage *)st)->firecracker, ws);
    sb = hu_firecracker_sandbox_get(&((struct hu_sandbox_storage *)st)->firecracker);
    out.firecracker = sb.vtable->is_available(sb.ctx);

    hu_landlock_seccomp_sandbox_init(&((struct hu_sandbox_storage *)st)->landlock_seccomp, ws,
                                     false);
    sb = hu_landlock_seccomp_sandbox_get(&((struct hu_sandbox_storage *)st)->landlock_seccomp);
    out.landlock_seccomp = sb.vtable->is_available(sb.ctx);
#endif

#ifdef _WIN32
    hu_appcontainer_sandbox_init(&((struct hu_sandbox_storage *)st)->appcontainer, ws);
    sb = hu_appcontainer_sandbox_get(&((struct hu_sandbox_storage *)st)->appcontainer);
    out.appcontainer = sb.vtable->is_available(sb.ctx);
#endif

#ifdef __APPLE__
    hu_seatbelt_sandbox_init(&((struct hu_sandbox_storage *)st)->seatbelt, ws);
    sb = hu_seatbelt_sandbox_get(&((struct hu_sandbox_storage *)st)->seatbelt);
    out.seatbelt = sb.vtable->is_available(sb.ctx);
#endif

    if (alloc && alloc->alloc && alloc->free) {
        hu_docker_sandbox_init(&((struct hu_sandbox_storage *)st)->docker, ws, "alpine:latest",
                               alloc->ctx, alloc->alloc, alloc->free);
        sb = hu_docker_sandbox_get(&((struct hu_sandbox_storage *)st)->docker);
        out.docker = sb.vtable->is_available(sb.ctx);
    }

    hu_wasi_sandbox_init(&((struct hu_sandbox_storage *)st)->wasi, ws);
    sb = hu_wasi_sandbox_get(&((struct hu_sandbox_storage *)st)->wasi);
    out.wasi = sb.vtable->is_available(sb.ctx);

    hu_sandbox_storage_destroy(st, alloc);
    return out;
}
