#include "human/config.h"
#include "human/core/log.h"
#include "human/runtime.h"
#include <stdatomic.h>
#include <string.h>

/* One-shot operator warnings for stub-tier runtimes. Per
 * ~/.claude/rules/silent-config-gated-subsystems.md, runtimes that are
 * incomplete (vtable methods return HU_ERR_NOT_SUPPORTED) or that require
 * out-of-band setup (gce — no usable behavior without an instance configured)
 * should announce themselves once per process so the operator can see in the
 * service log that the configured runtime is not the production-ready path.
 *
 * Each kind owns its own atomic_bool guard so the warnings are independent;
 * hu_log_warn_once() (see include/human/core/log.h) does the
 * compare-and-exchange so two threads racing on the same kind still emit at
 * most one line. */
static atomic_bool warned_runtime_gce = false;
#ifdef HU_HAS_RUNTIME_EXOTIC
/* Only declared when the exotic runtime branch below is compiled in;
 * otherwise -Werror=unused-variable trips. */
static atomic_bool warned_runtime_wasm = false;
static atomic_bool warned_runtime_cloudflare = false;
#endif

#define HU_STUB_RUNTIME_MSG_FMT                                                        \
    "runtime.kind='%s' selected — this runtime is a stub/incomplete tier and most "    \
    "vtable methods return HU_ERR_NOT_SUPPORTED. Supported runtimes: native, docker. " \
    "Update runtime.kind in config.json to use the production path."

hu_error_t hu_runtime_from_config(const struct hu_config *cfg, hu_runtime_t *out) {
    if (!cfg || !out)
        return HU_ERR_INVALID_ARGUMENT;

    const char *kind = cfg->runtime.kind;
    if (!kind || kind[0] == '\0' || strcmp(kind, "native") == 0) {
        *out = hu_runtime_native();
        return HU_OK;
    }

    if (strcmp(kind, "docker") == 0) {
        uint64_t mem_mb = 0;
        if (cfg->security.resource_limits.max_memory_mb > 0)
            mem_mb = (uint64_t)cfg->security.resource_limits.max_memory_mb;
        const char *image = cfg->runtime.docker_image;
        const char *workspace = cfg->workspace_dir ? cfg->workspace_dir : ".";
        *out = hu_runtime_docker(true, mem_mb, image, workspace);
        return HU_OK;
    }

    if (strcmp(kind, "gce") == 0) {
        hu_log_warn_once(&warned_runtime_gce, "runtime", NULL, HU_STUB_RUNTIME_MSG_FMT, "gce");
        uint64_t mem_mb = 0;
        if (cfg->security.resource_limits.max_memory_mb > 0)
            mem_mb = (uint64_t)cfg->security.resource_limits.max_memory_mb;
        const char *project = cfg->runtime.gce_project;
        const char *zone = cfg->runtime.gce_zone;
        const char *instance = cfg->runtime.gce_instance;
        *out = hu_runtime_gce(project, zone, instance, mem_mb);
        return HU_OK;
    }

#ifdef HU_HAS_RUNTIME_EXOTIC
    if (strcmp(kind, "wasm") == 0) {
        hu_log_warn_once(&warned_runtime_wasm, "runtime", NULL, HU_STUB_RUNTIME_MSG_FMT, "wasm");
        *out = hu_runtime_wasm(0);
        return HU_OK;
    }

    if (strcmp(kind, "cloudflare") == 0) {
        hu_log_warn_once(&warned_runtime_cloudflare, "runtime", NULL, HU_STUB_RUNTIME_MSG_FMT,
                         "cloudflare");
        *out = hu_runtime_cloudflare();
        return HU_OK;
    }
#endif

    return HU_ERR_NOT_SUPPORTED;
}
