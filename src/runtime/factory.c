#include "seaclaw/config.h"
#include "seaclaw/runtime.h"
#include <string.h>

sc_error_t sc_runtime_from_config(const struct sc_config *cfg, sc_runtime_t *out) {
    if (!cfg || !out)
        return SC_ERR_INVALID_ARGUMENT;

    const char *kind = cfg->runtime.kind;
    if (!kind || kind[0] == '\0' || strcmp(kind, "native") == 0) {
        *out = sc_runtime_native();
        return SC_OK;
    }

    if (strcmp(kind, "docker") == 0) {
        uint64_t mem_mb = 0;
        if (cfg->security.resource_limits.max_memory_mb > 0)
            mem_mb = (uint64_t)cfg->security.resource_limits.max_memory_mb;
        const char *image = cfg->runtime.docker_image;
        const char *workspace = cfg->workspace_dir ? cfg->workspace_dir : ".";
        *out = sc_runtime_docker(true, mem_mb, image, workspace);
        return SC_OK;
    }

    if (strcmp(kind, "wasm") == 0) {
        *out = sc_runtime_wasm(0);
        return SC_OK;
    }

    if (strcmp(kind, "cloudflare") == 0) {
        *out = sc_runtime_cloudflare();
        return SC_OK;
    }

    return SC_ERR_NOT_SUPPORTED;
}
