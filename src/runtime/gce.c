#include "human/core/error.h"
#include "human/runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct hu_gce_ctx {
    char project[256];
    char zone[128];
    char instance[256];
    uint64_t memory_limit_mb;
} hu_gce_ctx_t;

static hu_gce_ctx_t *get_ctx(void *ctx) {
    return (hu_gce_ctx_t *)ctx;
}

static const char *gce_name(void *ctx) {
    (void)ctx;
    return "gce";
}

static bool gce_has_shell_access(void *ctx) {
    (void)ctx;
    return true;
}

static bool gce_has_filesystem_access(void *ctx) {
    (void)ctx;
    return true;
}

static const char *gce_storage_path(void *ctx) {
    (void)ctx;
    return "/workspace/.human";
}

static bool gce_supports_long_running(void *ctx) {
    (void)ctx;
    return true;
}

static uint64_t gce_memory_budget(void *ctx) {
    hu_gce_ctx_t *g = get_ctx(ctx);
    if (g->memory_limit_mb > 0)
        return g->memory_limit_mb * 1024 * 1024;
    return 0;
}

static hu_error_t gce_wrap_command(void *ctx, const char **argv_in, size_t argc_in,
                                   const char **argv_out, size_t max_out, size_t *argc_out) {
    hu_gce_ctx_t *g = (hu_gce_ctx_t *)ctx;
    if (!g->instance[0])
        return HU_ERR_NOT_SUPPORTED;
    if (!argv_out || !argc_out || max_out < 8)
        return HU_ERR_INVALID_ARGUMENT;

    char zone_arg[160];
    char project_arg[288];
    char cmd_buf[4096];

    size_t idx = 0;
    argv_out[idx++] = "gcloud";
    argv_out[idx++] = "compute";
    argv_out[idx++] = "ssh";
    argv_out[idx++] = g->instance;

    if (g->zone[0]) {
        snprintf(zone_arg, sizeof(zone_arg), "--zone=%s", g->zone);
        argv_out[idx++] = zone_arg;
    }
    if (g->project[0]) {
        snprintf(project_arg, sizeof(project_arg), "--project=%s", g->project);
        argv_out[idx++] = project_arg;
    }

    /* Build combined command string for --command= */
    size_t cmd_off = 0;
    for (size_t i = 0; i < argc_in && cmd_off < sizeof(cmd_buf) - 2; i++) {
        if (i > 0 && cmd_off < sizeof(cmd_buf) - 1)
            cmd_buf[cmd_off++] = ' ';
        size_t len = strlen(argv_in[i]);
        if (cmd_off + len >= sizeof(cmd_buf) - 1)
            len = sizeof(cmd_buf) - 1 - cmd_off;
        memcpy(cmd_buf + cmd_off, argv_in[i], len);
        cmd_off += len;
    }
    cmd_buf[cmd_off] = '\0';

    char command_arg[4128];
    snprintf(command_arg, sizeof(command_arg), "--command=%s", cmd_buf);
    argv_out[idx++] = command_arg;

    argv_out[idx] = NULL;
    *argc_out = idx;
    return HU_OK;
}

static const hu_runtime_vtable_t gce_vtable = {
    .name = gce_name,
    .has_shell_access = gce_has_shell_access,
    .has_filesystem_access = gce_has_filesystem_access,
    .storage_path = gce_storage_path,
    .supports_long_running = gce_supports_long_running,
    .memory_budget = gce_memory_budget,
    .wrap_command = gce_wrap_command,
};

hu_runtime_t hu_runtime_gce(const char *project, const char *zone, const char *instance,
                            uint64_t memory_limit_mb) {
    static hu_gce_ctx_t s_gce = {{0}, {0}, {0}, 0};
    if (project) {
        size_t len = strlen(project);
        if (len >= sizeof(s_gce.project))
            len = sizeof(s_gce.project) - 1;
        memcpy(s_gce.project, project, len);
        s_gce.project[len] = '\0';
    } else {
        s_gce.project[0] = '\0';
    }
    if (zone) {
        size_t len = strlen(zone);
        if (len >= sizeof(s_gce.zone))
            len = sizeof(s_gce.zone) - 1;
        memcpy(s_gce.zone, zone, len);
        s_gce.zone[len] = '\0';
    } else {
        s_gce.zone[0] = '\0';
    }
    if (instance) {
        size_t len = strlen(instance);
        if (len >= sizeof(s_gce.instance))
            len = sizeof(s_gce.instance) - 1;
        memcpy(s_gce.instance, instance, len);
        s_gce.instance[len] = '\0';
    } else {
        s_gce.instance[0] = '\0';
    }
    s_gce.memory_limit_mb = memory_limit_mb;
    return (hu_runtime_t){
        .ctx = &s_gce,
        .vtable = &gce_vtable,
    };
}
