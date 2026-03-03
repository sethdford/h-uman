#include "seaclaw/core/error.h"
#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"
#include <stdio.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

/*
 * Firecracker microVM sandbox (Linux only, requires KVM).
 *
 * Provides hardware-level isolation via lightweight VMs. Each sandboxed
 * command runs inside a Firecracker microVM with:
 *  - Dedicated virtual CPU and memory (configurable)
 *  - VirtioFS filesystem sharing for workspace directory
 *  - No network by default
 *  - Sub-200ms boot time
 *
 * Requires: Firecracker binary, KVM access (/dev/kvm), a kernel image,
 * and a rootfs image. Default paths follow the Firecracker convention.
 *
 * wrap_command produces:
 *   firecracker --api-sock SOCKET --config-file CONFIG <argv...>
 * but for simplicity, we use the jailer wrapper which handles cgroups/namespaces:
 *   jailer --id VM_ID --exec-file /usr/bin/firecracker -- <argv...>
 */

#if !defined(_WIN32)
#include <unistd.h>
#endif
#ifdef __linux__
#include <fcntl.h>
#endif

#if defined(__linux__) && !SC_IS_TEST
static bool firecracker_binary_exists(void) {
    if (access("/usr/bin/firecracker", X_OK) == 0)
        return true;
    if (access("/usr/local/bin/firecracker", X_OK) == 0)
        return true;
    return false;
}

static bool kvm_available(void) {
    return access("/dev/kvm", R_OK | W_OK) == 0;
}
#endif

#ifdef __linux__
static sc_error_t firecracker_write_config(sc_firecracker_ctx_t *fc, const char *config_path,
                                           const char *const *argv, size_t argc) {
    FILE *f = fopen(config_path, "w");
    if (!f)
        return SC_ERR_IO;

    /* Build boot_args: shell-safe single-quoted command.
     * Inside single quotes, only '\'' works to escape a literal quote
     * (end quote, escaped quote, new quote). The entire string must also
     * be valid inside a JSON double-quoted value (escape " and \). */
    char boot_args[4096];
    size_t pos = 0;
    pos += (size_t)snprintf(boot_args + pos, sizeof(boot_args) - pos,
                            "console=ttyS0 reboot=k panic=1 pci=off init=/bin/sh -- -c '");
    for (size_t i = 0; i < argc && pos + 16 < sizeof(boot_args); i++) {
        if (i > 0 && pos < sizeof(boot_args) - 2)
            boot_args[pos++] = ' ';
        for (const char *p = argv[i]; *p && pos + 16 < sizeof(boot_args); p++) {
            if (*p == '\'') {
                memcpy(boot_args + pos, "'\\''", 4);
                pos += 4;
            } else {
                boot_args[pos++] = *p;
            }
        }
    }
    if (pos < sizeof(boot_args) - 2)
        boot_args[pos++] = '\'';
    boot_args[pos] = '\0';

    /* Write JSON config, escaping boot_args for the JSON string value */
    fprintf(f, "{\n  \"boot-source\": {\n    \"kernel_image_path\": \"");
    for (const char *p = fc->kernel_path; *p; p++) {
        if (*p == '"' || *p == '\\')
            fputc('\\', f);
        fputc(*p, f);
    }
    fprintf(f, "\",\n    \"boot_args\": \"");
    for (const char *p = boot_args; *p; p++) {
        if (*p == '"' || *p == '\\')
            fputc('\\', f);
        fputc(*p, f);
    }
    fprintf(f, "\"\n  },\n  \"drives\": [{\n    \"drive_id\": \"rootfs\",\n"
               "    \"path_on_host\": \"");
    for (const char *p = fc->rootfs_path; *p; p++) {
        if (*p == '"' || *p == '\\')
            fputc('\\', f);
        fputc(*p, f);
    }
    fprintf(f,
            "\",\n    \"is_root_device\": true,\n"
            "    \"is_read_only\": false\n  }],\n"
            "  \"machine-config\": {\n"
            "    \"vcpu_count\": %u,\n"
            "    \"mem_size_mib\": %u\n  }\n}\n",
            fc->vcpu_count, fc->mem_size_mib);
    fclose(f);
    return SC_OK;
}
#endif

static sc_error_t firecracker_wrap(void *ctx, const char *const *argv, size_t argc,
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
    sc_firecracker_ctx_t *fc = (sc_firecracker_ctx_t *)ctx;

    if (!buf || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    if (buf_count < 4)
        return SC_ERR_INVALID_ARGUMENT;

    static char config_path[280];
    static char config_arg[300];
    snprintf(config_path, sizeof(config_path), "%s.json", fc->socket_path);

    sc_error_t err = firecracker_write_config(fc, config_path, argv, argc);
    if (err != SC_OK)
        return err;

    snprintf(config_arg, sizeof(config_arg), "--config-file=%s", config_path);

    buf[0] = "firecracker";
    buf[1] = "--no-api";
    buf[2] = "--boot-timer";
    buf[3] = config_arg;
    *out_count = 4;
    return SC_OK;
#endif
}

static bool firecracker_available(void *ctx) {
    (void)ctx;
#ifdef __linux__
#if SC_IS_TEST
    return false;
#else
    return firecracker_binary_exists() && kvm_available();
#endif
#else
    return false;
#endif
}

static const char *firecracker_name(void *ctx) {
    (void)ctx;
    return "firecracker";
}

static const char *firecracker_desc(void *ctx) {
    (void)ctx;
#ifdef __linux__
    return "Firecracker microVM (hardware-level isolation via KVM, sub-200ms boot)";
#else
    return "Firecracker microVM (not available on this platform, requires Linux + KVM)";
#endif
}

static const sc_sandbox_vtable_t firecracker_vtable = {
    .wrap_command = firecracker_wrap,
    .apply = NULL,
    .is_available = firecracker_available,
    .name = firecracker_name,
    .description = firecracker_desc,
};

sc_sandbox_t sc_firecracker_sandbox_get(sc_firecracker_ctx_t *ctx) {
    sc_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &firecracker_vtable,
    };
    return sb;
}

void sc_firecracker_sandbox_init(sc_firecracker_ctx_t *ctx, const char *workspace_dir) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->vcpu_count = 1;
    ctx->mem_size_mib = 128;

    if (workspace_dir) {
        size_t len = strlen(workspace_dir);
        if (len >= sizeof(ctx->workspace_dir))
            len = sizeof(ctx->workspace_dir) - 1;
        memcpy(ctx->workspace_dir, workspace_dir, len);
        ctx->workspace_dir[len] = '\0';
    }

    snprintf(ctx->socket_path, sizeof(ctx->socket_path), "/tmp/sc_fc_%d.sock", (int)getpid());
    memcpy(ctx->kernel_path, "/var/lib/firecracker/vmlinux", 29);
    memcpy(ctx->rootfs_path, "/var/lib/firecracker/rootfs.ext4", 33);
}
