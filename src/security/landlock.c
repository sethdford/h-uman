#include "seaclaw/security/sandbox.h"
#include <stdint.h>
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <fcntl.h>

#ifdef __linux__
#include <linux/landlock.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <errno.h>

#ifndef SYS_landlock_create_ruleset
#if defined(__x86_64__) || defined(__aarch64__)
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule 445
#define SYS_landlock_restrict_self 446
#else
#define SYS_landlock_create_ruleset (-1)
#define SYS_landlock_add_rule (-1)
#define SYS_landlock_restrict_self (-1)
#endif
#endif

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif

static long landlock_abi_version(void) {
#if SC_IS_TEST
    return 0;
#else
    long r = syscall(SYS_landlock_create_ruleset, NULL, 0,
        (uint32_t)LANDLOCK_CREATE_RULESET_VERSION);
    return (r >= 1 && r <= 5) ? r : 0;
#endif
}

static bool landlock_supported(void) {
    return landlock_abi_version() >= 1;
}
#endif /* __linux__ (line 8) */

/*
 * Apply Landlock filesystem ACLs. Called after fork(), before child runs.
 *
 * Policy: read/write access to workspace_dir and /tmp; read-only access to
 * /usr, /bin, /lib, /etc, /dev, /proc for tool execution. Everything else
 * is denied.
 */
static sc_error_t landlock_apply(void *ctx) {
#ifndef __linux__
    (void)ctx;
    return SC_ERR_NOT_SUPPORTED;
#else
#if SC_IS_TEST
    (void)ctx;
    return SC_OK;
#else
    sc_landlock_ctx_t *ll = (sc_landlock_ctx_t *)ctx;
    long abi = landlock_abi_version();
    if (abi < 1) return SC_ERR_NOT_SUPPORTED;

    uint64_t fs_access =
        LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR |
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR |
        LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO |
        LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM |
        LANDLOCK_ACCESS_FS_EXECUTE;
    if (abi >= 2) fs_access |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) fs_access |= LANDLOCK_ACCESS_FS_TRUNCATE;

    struct landlock_ruleset_attr ruleset_attr = {
        .handled_access_fs = fs_access,
    };
    int ruleset_fd = (int)syscall(SYS_landlock_create_ruleset,
        &ruleset_attr, sizeof(ruleset_attr), 0);
    if (ruleset_fd < 0) return SC_ERR_SYSTEM;

    /* Helper: add a path rule with given access mask */
    struct {
        const char *path;
        uint64_t access;
    } rules[] = {
        /* Read-write: workspace */
        { ll->workspace_dir, fs_access & ~LANDLOCK_ACCESS_FS_EXECUTE },
        /* Read-write: /tmp */
        { "/tmp", fs_access & ~LANDLOCK_ACCESS_FS_EXECUTE },
        /* Read + execute: system directories */
        { "/usr", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE },
        { "/bin", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE },
        { "/lib", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE },
        { "/lib64", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE },
        { "/sbin", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE },
        /* Read-only: config and device directories */
        { "/etc", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR },
        { "/dev", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR },
        { "/proc", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR },
    };

    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        int path_fd = open(rules[i].path, O_PATH | O_CLOEXEC);
        if (path_fd < 0) continue;
        struct landlock_path_beneath_attr path_beneath = {
            .allowed_access = rules[i].access & fs_access,
            .parent_fd = path_fd,
        };
        long ret = syscall(SYS_landlock_add_rule, ruleset_fd,
            LANDLOCK_RULE_PATH_BENEATH, &path_beneath, 0);
        close(path_fd);
        if (ret < 0 && i < 2) {
            close(ruleset_fd);
            return SC_ERR_SYSTEM;
        }
    }

    /* Drop ambient privileges so Landlock can restrict us */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        close(ruleset_fd);
        return SC_ERR_SYSTEM;
    }

    if (syscall(SYS_landlock_restrict_self, ruleset_fd, 0) < 0) {
        close(ruleset_fd);
        return SC_ERR_SYSTEM;
    }

    close(ruleset_fd);
    return SC_OK;
#endif /* SC_IS_TEST */
#endif /* __linux__ */
}

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
    return "Linux Landlock LSM (kernel-level filesystem ACLs, near-zero overhead)";
#else
    return "Linux Landlock LSM (not available on this platform)";
#endif
}

static const sc_sandbox_vtable_t landlock_vtable = {
    .wrap_command = landlock_wrap,
    .apply = landlock_apply,
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
