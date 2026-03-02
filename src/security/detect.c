#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <stdlib.h>

struct sc_sandbox_storage {
    sc_noop_sandbox_ctx_t noop;
    sc_landlock_ctx_t landlock;
    sc_firejail_ctx_t firejail;
    sc_bubblewrap_ctx_t bubblewrap;
    sc_docker_ctx_t docker;
    sc_seatbelt_ctx_t seatbelt;
    sc_seccomp_ctx_t seccomp;
    sc_landlock_seccomp_ctx_t landlock_seccomp;
    sc_wasi_sandbox_ctx_t wasi;
    sc_firecracker_ctx_t firecracker;
    sc_appcontainer_ctx_t appcontainer;
};

sc_sandbox_storage_t *sc_sandbox_storage_create(const sc_sandbox_alloc_t *alloc) {
    if (!alloc || !alloc->alloc) return NULL;
    void *p = alloc->alloc(alloc->ctx, sizeof(struct sc_sandbox_storage));
    return (sc_sandbox_storage_t *)p;
}

void sc_sandbox_storage_destroy(sc_sandbox_storage_t *s,
    const sc_sandbox_alloc_t *alloc) {
    if (!s || !alloc || !alloc->free) return;
    alloc->free(alloc->ctx, s, sizeof(struct sc_sandbox_storage));
}

sc_sandbox_t sc_sandbox_create(sc_sandbox_backend_t backend,
    const char *workspace_dir,
    sc_sandbox_storage_t *storage,
    const sc_sandbox_alloc_t *alloc) {
    if (!storage) {
        sc_sandbox_t empty = { .ctx = NULL, .vtable = NULL };
        return empty;
    }

    struct sc_sandbox_storage *st = (struct sc_sandbox_storage *)storage;
    sc_sandbox_t result = { .ctx = NULL, .vtable = NULL };
    const char *ws = workspace_dir ? workspace_dir : "/tmp";

    switch (backend) {
        case SC_SANDBOX_NONE: {
            result = sc_noop_sandbox_get(&st->noop);
            break;
        }
        case SC_SANDBOX_LANDLOCK: {
            sc_landlock_sandbox_init(&st->landlock, ws);
            result = sc_landlock_sandbox_get(&st->landlock);
            if (!result.vtable->is_available(result.ctx))
                result = sc_noop_sandbox_get(&st->noop);
            break;
        }
        case SC_SANDBOX_FIREJAIL: {
            sc_firejail_sandbox_init(&st->firejail, ws);
            result = sc_firejail_sandbox_get(&st->firejail);
            if (!result.vtable->is_available(result.ctx))
                result = sc_noop_sandbox_get(&st->noop);
            break;
        }
        case SC_SANDBOX_BUBBLEWRAP: {
            sc_bubblewrap_sandbox_init(&st->bubblewrap, ws);
            result = sc_bubblewrap_sandbox_get(&st->bubblewrap);
            if (!result.vtable->is_available(result.ctx))
                result = sc_noop_sandbox_get(&st->noop);
            break;
        }
        case SC_SANDBOX_DOCKER: {
            if (!alloc || !alloc->alloc || !alloc->free) break;
            sc_docker_sandbox_init(&st->docker, ws, "alpine:latest",
                alloc->ctx, alloc->alloc, alloc->free);
            result = sc_docker_sandbox_get(&st->docker);
            break;
        }
        case SC_SANDBOX_SEATBELT: {
            sc_seatbelt_sandbox_init(&st->seatbelt, ws);
            result = sc_seatbelt_sandbox_get(&st->seatbelt);
            if (!result.vtable->is_available(result.ctx))
                result = sc_noop_sandbox_get(&st->noop);
            break;
        }
        case SC_SANDBOX_SECCOMP: {
            sc_seccomp_sandbox_init(&st->seccomp, ws, false);
            result = sc_seccomp_sandbox_get(&st->seccomp);
            if (!result.vtable->is_available(result.ctx))
                result = sc_noop_sandbox_get(&st->noop);
            break;
        }
        case SC_SANDBOX_WASI: {
            sc_wasi_sandbox_init(&st->wasi, ws);
            result = sc_wasi_sandbox_get(&st->wasi);
            if (!result.vtable->is_available(result.ctx))
                result = sc_noop_sandbox_get(&st->noop);
            break;
        }
        case SC_SANDBOX_LANDLOCK_SECCOMP: {
            sc_landlock_seccomp_sandbox_init(&st->landlock_seccomp, ws, false);
            result = sc_landlock_seccomp_sandbox_get(&st->landlock_seccomp);
            if (!result.vtable->is_available(result.ctx))
                result = sc_noop_sandbox_get(&st->noop);
            break;
        }
        case SC_SANDBOX_FIRECRACKER: {
            sc_firecracker_sandbox_init(&st->firecracker, ws);
            result = sc_firecracker_sandbox_get(&st->firecracker);
            if (!result.vtable->is_available(result.ctx))
                result = sc_noop_sandbox_get(&st->noop);
            break;
        }
        case SC_SANDBOX_APPCONTAINER: {
            sc_appcontainer_sandbox_init(&st->appcontainer, ws);
            result = sc_appcontainer_sandbox_get(&st->appcontainer);
            if (!result.vtable->is_available(result.ctx))
                result = sc_noop_sandbox_get(&st->noop);
            break;
        }
        case SC_SANDBOX_AUTO: {
            /*
             * Tiered auto-detection:
             *
             * macOS:   Seatbelt (kernel) -> Docker -> WASI -> noop
             * Linux:   Landlock+seccomp (combined) -> Bubblewrap -> Firejail
             *          -> Firecracker (KVM) -> Docker -> WASI -> noop
             * Windows: AppContainer -> WASI -> noop
             */
#ifdef __APPLE__
            sc_seatbelt_sandbox_init(&st->seatbelt, ws);
            result = sc_seatbelt_sandbox_get(&st->seatbelt);
            if (result.vtable->is_available(result.ctx)) break;
#endif

#ifdef __linux__
            /* Tier 1: Kernel-level combined (Landlock FS ACLs + seccomp syscall filter) */
            sc_landlock_seccomp_sandbox_init(&st->landlock_seccomp, ws, false);
            result = sc_landlock_seccomp_sandbox_get(&st->landlock_seccomp);
            if (result.vtable->is_available(result.ctx)) break;

            /* Tier 2: User-space namespace isolation */
            sc_bubblewrap_sandbox_init(&st->bubblewrap, ws);
            result = sc_bubblewrap_sandbox_get(&st->bubblewrap);
            if (result.vtable->is_available(result.ctx)) break;

            sc_firejail_sandbox_init(&st->firejail, ws);
            result = sc_firejail_sandbox_get(&st->firejail);
            if (result.vtable->is_available(result.ctx)) break;

            /* Tier 3: MicroVM (requires KVM) */
            sc_firecracker_sandbox_init(&st->firecracker, ws);
            result = sc_firecracker_sandbox_get(&st->firecracker);
            if (result.vtable->is_available(result.ctx)) break;
#endif

#ifdef _WIN32
            sc_appcontainer_sandbox_init(&st->appcontainer, ws);
            result = sc_appcontainer_sandbox_get(&st->appcontainer);
            if (result.vtable->is_available(result.ctx)) break;
#endif

            /* Cross-platform fallbacks */
            if (alloc && alloc->alloc && alloc->free) {
                sc_docker_sandbox_init(&st->docker, ws, "alpine:latest",
                    alloc->ctx, alloc->alloc, alloc->free);
                result = sc_docker_sandbox_get(&st->docker);
                if (result.vtable->is_available(result.ctx)) break;
            }

            sc_wasi_sandbox_init(&st->wasi, ws);
            result = sc_wasi_sandbox_get(&st->wasi);
            if (result.vtable->is_available(result.ctx)) break;

            result = sc_noop_sandbox_get(&st->noop);
            break;
        }
    }
    return result;
}

sc_available_backends_t sc_sandbox_detect_available(const char *workspace_dir,
    const sc_sandbox_alloc_t *alloc) {
    sc_available_backends_t out = {0};
    sc_sandbox_storage_t *st = alloc && alloc->alloc
        ? sc_sandbox_storage_create(alloc) : NULL;
    if (!st) return out;

    const char *ws = workspace_dir ? workspace_dir : "/tmp";
    sc_sandbox_t sb;

#ifdef __linux__
    sc_landlock_sandbox_init(&((struct sc_sandbox_storage *)st)->landlock, ws);
    sb = sc_landlock_sandbox_get(&((struct sc_sandbox_storage *)st)->landlock);
    out.landlock = sb.vtable->is_available(sb.ctx);

    sc_firejail_sandbox_init(&((struct sc_sandbox_storage *)st)->firejail, ws);
    sb = sc_firejail_sandbox_get(&((struct sc_sandbox_storage *)st)->firejail);
    out.firejail = sb.vtable->is_available(sb.ctx);

    sc_bubblewrap_sandbox_init(&((struct sc_sandbox_storage *)st)->bubblewrap, ws);
    sb = sc_bubblewrap_sandbox_get(&((struct sc_sandbox_storage *)st)->bubblewrap);
    out.bubblewrap = sb.vtable->is_available(sb.ctx);

    sc_seccomp_sandbox_init(&((struct sc_sandbox_storage *)st)->seccomp, ws, false);
    sb = sc_seccomp_sandbox_get(&((struct sc_sandbox_storage *)st)->seccomp);
    out.seccomp = sb.vtable->is_available(sb.ctx);

    sc_firecracker_sandbox_init(&((struct sc_sandbox_storage *)st)->firecracker, ws);
    sb = sc_firecracker_sandbox_get(&((struct sc_sandbox_storage *)st)->firecracker);
    out.firecracker = sb.vtable->is_available(sb.ctx);

    sc_landlock_seccomp_sandbox_init(&((struct sc_sandbox_storage *)st)->landlock_seccomp, ws, false);
    sb = sc_landlock_seccomp_sandbox_get(&((struct sc_sandbox_storage *)st)->landlock_seccomp);
    out.landlock_seccomp = sb.vtable->is_available(sb.ctx);
#endif

#ifdef _WIN32
    sc_appcontainer_sandbox_init(&((struct sc_sandbox_storage *)st)->appcontainer, ws);
    sb = sc_appcontainer_sandbox_get(&((struct sc_sandbox_storage *)st)->appcontainer);
    out.appcontainer = sb.vtable->is_available(sb.ctx);
#endif

#ifdef __APPLE__
    sc_seatbelt_sandbox_init(&((struct sc_sandbox_storage *)st)->seatbelt, ws);
    sb = sc_seatbelt_sandbox_get(&((struct sc_sandbox_storage *)st)->seatbelt);
    out.seatbelt = sb.vtable->is_available(sb.ctx);
#endif

    if (alloc && alloc->alloc && alloc->free) {
        sc_docker_sandbox_init(&((struct sc_sandbox_storage *)st)->docker,
            ws, "alpine:latest", alloc->ctx, alloc->alloc, alloc->free);
        sb = sc_docker_sandbox_get(&((struct sc_sandbox_storage *)st)->docker);
        out.docker = sb.vtable->is_available(sb.ctx);
    }

    sc_wasi_sandbox_init(&((struct sc_sandbox_storage *)st)->wasi, ws);
    sb = sc_wasi_sandbox_get(&((struct sc_sandbox_storage *)st)->wasi);
    out.wasi = sb.vtable->is_available(sb.ctx);

    sc_sandbox_storage_destroy(st, alloc);
    return out;
}
