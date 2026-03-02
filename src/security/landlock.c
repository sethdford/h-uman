#include "seaclaw/security/sandbox.h"
#include <stdint.h>
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/core/error.h"
#include <string.h>

#ifdef __linux__
#include <linux/landlock.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_landlock_create_ruleset
/* Fallback for older headers; x86_64 = 444, aarch64 = 444 */
#if defined(__x86_64__) || defined(__aarch64__)
#define SYS_landlock_create_ruleset 444
#else
#define SYS_landlock_create_ruleset (-1)
#endif
#endif

static bool landlock_supported(void) {
#if SC_IS_TEST
    return false; /* Skip kernel check in tests; macOS has no Landlock */
#else
    /* Query Landlock ABI version; ENOSYS means kernel lacks Landlock */
    long r = syscall(SYS_landlock_create_ruleset, NULL, 0,
        (uint32_t)LANDLOCK_CREATE_RULESET_VERSION);
    return (r >= 1 && r <= 5);
#endif
}
#endif

static sc_error_t landlock_wrap(void *ctx, const char *const *argv, size_t argc,
    const char **buf, size_t buf_count, size_t *out_count) {
#ifndef __linux__
    (void)ctx;
    (void)argv;
    (void)argc;
    (void)buf;
    (void)buf_count;
    (void)out_count;
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

static bool landlock_available(void *ctx) {
    (void)ctx;
#ifdef __linux__
    return landlock_supported();
#else
    return false;
#endif
}

static const char *landlock_name(void *ctx) {
    (void)ctx;
    return "landlock";
}

static const char *landlock_desc(void *ctx) {
    (void)ctx;
#ifdef __linux__
    return "Linux kernel LSM sandboxing (filesystem access control)";
#else
    return "Linux kernel LSM sandboxing (not available on this platform)";
#endif
}

static const sc_sandbox_vtable_t landlock_vtable = {
    .wrap_command = landlock_wrap,
    .is_available = landlock_available,
    .name = landlock_name,
    .description = landlock_desc,
};

sc_sandbox_t sc_landlock_sandbox_get(sc_landlock_ctx_t *ctx) {
    sc_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &landlock_vtable,
    };
    return sb;
}

void sc_landlock_sandbox_init(sc_landlock_ctx_t *ctx, const char *workspace_dir) {
    memset(ctx, 0, sizeof(*ctx));
    if (workspace_dir) {
        size_t len = strlen(workspace_dir);
        if (len >= sizeof(ctx->workspace_dir))
            len = sizeof(ctx->workspace_dir) - 1;
        memcpy(ctx->workspace_dir, workspace_dir, len);
        ctx->workspace_dir[len] = '\0';
    }
}
