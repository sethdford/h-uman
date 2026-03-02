#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <stdio.h>

/*
 * Apple Seatbelt sandbox using sandbox-exec with SBPL (Sandbox Profile Language).
 * Provides kernel-level FS/network/IPC isolation on macOS with near-zero overhead.
 *
 * The SBPL profile:
 *  - Denies all by default
 *  - Allows read/write to the workspace directory
 *  - Allows read to /usr, /bin, /Library, /System (for tool execution)
 *  - Allows process execution
 *  - Denies network access
 *  - Denies mach-lookup (IPC) except essential services
 */

#ifdef __APPLE__
#include <unistd.h>
#endif

static const char *seatbelt_profile_fmt =
    "(version 1)"
    "(deny default)"
    "(allow process*)"
    "(allow sysctl-read)"
    "(allow file-read* (subpath \"/usr\"))"
    "(allow file-read* (subpath \"/bin\"))"
    "(allow file-read* (subpath \"/sbin\"))"
    "(allow file-read* (subpath \"/Library\"))"
    "(allow file-read* (subpath \"/System\"))"
    "(allow file-read* (subpath \"/private/var\"))"
    "(allow file-write* (subpath \"/private/var/folders\"))"
    "(allow file-read* (subpath \"/dev\"))"
    "(allow file-read* (subpath \"/tmp\"))"
    "(allow file-write* (subpath \"/tmp\"))"
    "(allow file-read* (subpath \"/private/tmp\"))"
    "(allow file-write* (subpath \"/private/tmp\"))"
    "(allow file-read* (subpath \"/var\"))"
    "(allow file-write* (subpath \"/var/folders\"))"
    "(allow file-read* (literal \"/etc\"))"
    "(allow file-read* (subpath \"/etc\"))"
    "(allow file-read* (subpath \"%s\"))"
    "(allow file-write* (subpath \"%s\"))"
    "(deny network*)";

static sc_error_t seatbelt_wrap(void *ctx, const char *const *argv, size_t argc,
    const char **buf, size_t buf_count, size_t *out_count) {
#ifndef __APPLE__
    (void)ctx; (void)argv; (void)argc; (void)buf; (void)buf_count; (void)out_count;
    return SC_ERR_NOT_SUPPORTED;
#else
    sc_seatbelt_ctx_t *sb = (sc_seatbelt_ctx_t *)ctx;
    const size_t prefix_len = 3;
    if (!buf || !out_count) return SC_ERR_INVALID_ARGUMENT;
    if (buf_count < prefix_len + argc) return SC_ERR_INVALID_ARGUMENT;
    if (sb->profile_len == 0) return SC_ERR_INTERNAL;

    buf[0] = "sandbox-exec";
    buf[1] = "-p";
    buf[2] = sb->profile;
    for (size_t i = 0; i < argc; i++)
        buf[prefix_len + i] = argv[i];
    *out_count = prefix_len + argc;
    return SC_OK;
#endif
}

static bool seatbelt_available(void *ctx) {
    (void)ctx;
#ifdef __APPLE__
#if SC_IS_TEST
    return true;
#else
    return access("/usr/bin/sandbox-exec", X_OK) == 0;
#endif
#else
    return false;
#endif
}

static const char *seatbelt_name(void *ctx) {
    (void)ctx;
    return "seatbelt";
}

static const char *seatbelt_desc(void *ctx) {
    (void)ctx;
#ifdef __APPLE__
    return "macOS Seatbelt sandbox (kernel-level SBPL profiles, near-zero overhead)";
#else
    return "macOS Seatbelt sandbox (not available on this platform)";
#endif
}

static const sc_sandbox_vtable_t seatbelt_vtable = {
    .wrap_command = seatbelt_wrap,
    .apply = NULL,
    .is_available = seatbelt_available,
    .name = seatbelt_name,
    .description = seatbelt_desc,
};

sc_sandbox_t sc_seatbelt_sandbox_get(sc_seatbelt_ctx_t *ctx) {
    sc_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &seatbelt_vtable,
    };
    return sb;
}

void sc_seatbelt_sandbox_init(sc_seatbelt_ctx_t *ctx, const char *workspace_dir) {
    memset(ctx, 0, sizeof(*ctx));
    const char *ws = workspace_dir ? workspace_dir : "/tmp";
    size_t wlen = strlen(ws);
    if (wlen >= sizeof(ctx->workspace_dir))
        wlen = sizeof(ctx->workspace_dir) - 1;
    memcpy(ctx->workspace_dir, ws, wlen);
    ctx->workspace_dir[wlen] = '\0';

    int n = snprintf(ctx->profile, sizeof(ctx->profile),
        seatbelt_profile_fmt, ctx->workspace_dir, ctx->workspace_dir);
    if (n > 0 && (size_t)n < sizeof(ctx->profile))
        ctx->profile_len = (size_t)n;
    else
        ctx->profile_len = 0;
}
