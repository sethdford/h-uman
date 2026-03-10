#include "human/core/error.h"
#include "human/runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct hu_docker_ctx {
    bool mount_workspace;
    uint64_t memory_limit_mb;
    char image[256];
    char workspace[1024];
} hu_docker_ctx_t;

static hu_docker_ctx_t *get_ctx(void *ctx) {
    return (hu_docker_ctx_t *)ctx;
}

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "docker";
}

static bool impl_has_shell_access(void *ctx) {
    (void)ctx;
    return true;
}

static bool impl_has_filesystem_access(void *ctx) {
    return get_ctx(ctx)->mount_workspace;
}

static const char *impl_storage_path(void *ctx) {
    return get_ctx(ctx)->mount_workspace ? "/workspace/.human" : "/tmp/.human";
}

static bool impl_supports_long_running(void *ctx) {
    (void)ctx;
    return false;
}

static uint64_t impl_memory_budget(void *ctx) {
    hu_docker_ctx_t *d = get_ctx(ctx);
    if (d->memory_limit_mb > 0)
        return d->memory_limit_mb * 1024 * 1024;
    return 0;
}

static hu_error_t docker_wrap_command(void *ctx, const char **argv_in, size_t argc_in,
                                      const char **argv_out, size_t max_out, size_t *argc_out) {
    hu_docker_ctx_t *d = (hu_docker_ctx_t *)ctx;
    if (!d->image[0])
        return HU_ERR_NOT_SUPPORTED;
    if (!argv_out || !argc_out || max_out < 5)
        return HU_ERR_INVALID_ARGUMENT;

    size_t idx = 0;
    argv_out[idx++] = "docker";
    argv_out[idx++] = "run";
    argv_out[idx++] = "--rm";

    static char mem_arg[32];
    if (d->memory_limit_mb > 0) {
        snprintf(mem_arg, sizeof(mem_arg), "%llum", (unsigned long long)d->memory_limit_mb);
        argv_out[idx++] = "-m";
        argv_out[idx++] = mem_arg;
    }

    static char mount_arg[2048];
    if (d->mount_workspace && d->workspace[0]) {
        if (strchr(d->workspace, ':'))
            return HU_ERR_INVALID_ARGUMENT;
        snprintf(mount_arg, sizeof(mount_arg), "%s:/workspace", d->workspace);
        argv_out[idx++] = "-v";
        argv_out[idx++] = mount_arg;
        argv_out[idx++] = "-w";
        argv_out[idx++] = "/workspace";
    }

    argv_out[idx++] = d->image;

    for (size_t i = 0; i < argc_in && idx < max_out - 1; i++)
        argv_out[idx++] = argv_in[i];

    argv_out[idx] = NULL;
    *argc_out = idx;
    return HU_OK;
}

static const hu_runtime_vtable_t docker_vtable = {
    .name = impl_name,
    .has_shell_access = impl_has_shell_access,
    .has_filesystem_access = impl_has_filesystem_access,
    .storage_path = impl_storage_path,
    .supports_long_running = impl_supports_long_running,
    .memory_budget = impl_memory_budget,
    .wrap_command = docker_wrap_command,
};

hu_runtime_t hu_runtime_docker(bool mount_workspace, uint64_t memory_limit_mb, const char *image,
                               const char *workspace) {
    static hu_docker_ctx_t s_docker = {false, 0, {0}, {0}};
    s_docker.mount_workspace = mount_workspace;
    s_docker.memory_limit_mb = memory_limit_mb;
    if (image) {
        size_t len = strlen(image);
        if (len >= sizeof(s_docker.image))
            len = sizeof(s_docker.image) - 1;
        memcpy(s_docker.image, image, len);
        s_docker.image[len] = '\0';
    } else {
        s_docker.image[0] = '\0';
    }
    if (workspace) {
        size_t len = strlen(workspace);
        if (len >= sizeof(s_docker.workspace))
            len = sizeof(s_docker.workspace) - 1;
        memcpy(s_docker.workspace, workspace, len);
        s_docker.workspace[len] = '\0';
    } else {
        s_docker.workspace[0] = '\0';
    }
    return (hu_runtime_t){
        .ctx = &s_docker,
        .vtable = &docker_vtable,
    };
}
