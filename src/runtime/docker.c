#include "seaclaw/runtime.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef struct sc_docker_runtime {
    bool mount_workspace;
    uint64_t memory_limit_mb;
} sc_docker_runtime_t;

static sc_docker_runtime_t *get_ctx(void *ctx) {
    return (sc_docker_runtime_t *)ctx;
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
    return get_ctx(ctx)->mount_workspace ? "/workspace/.seaclaw" : "/tmp/.seaclaw";
}

static bool impl_supports_long_running(void *ctx) {
    (void)ctx;
    return false;
}

static uint64_t impl_memory_budget(void *ctx) {
    sc_docker_runtime_t *d = get_ctx(ctx);
    if (d->memory_limit_mb > 0)
        return d->memory_limit_mb * 1024 * 1024;
    return 0;
}

static const sc_runtime_vtable_t docker_vtable = {
    .name = impl_name,
    .has_shell_access = impl_has_shell_access,
    .has_filesystem_access = impl_has_filesystem_access,
    .storage_path = impl_storage_path,
    .supports_long_running = impl_supports_long_running,
    .memory_budget = impl_memory_budget,
};

sc_runtime_t sc_runtime_docker(bool mount_workspace, uint64_t memory_limit_mb) {
    static sc_docker_runtime_t s_docker = { false, 0 };
    s_docker.mount_workspace = mount_workspace;
    s_docker.memory_limit_mb = memory_limit_mb;
    return (sc_runtime_t){
        .ctx = &s_docker,
        .vtable = &docker_vtable,
    };
}
