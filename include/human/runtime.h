#ifndef HU_RUNTIME_H
#define HU_RUNTIME_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Runtime vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct hu_runtime_vtable;

typedef struct hu_runtime {
    void *ctx;
    const struct hu_runtime_vtable *vtable;
} hu_runtime_t;

typedef struct hu_runtime_vtable {
    const char *(*name)(void *ctx);
    bool (*has_shell_access)(void *ctx);
    bool (*has_filesystem_access)(void *ctx);
    const char *(*storage_path)(void *ctx);
    bool (*supports_long_running)(void *ctx);
    uint64_t (*memory_budget)(void *ctx);
    /* Optional: wrap a command for execution in this runtime's context.
     * Returns HU_ERR_NOT_SUPPORTED if the runtime doesn't wrap commands.
     * argv_in/argc_in: original command; argv_out/max_out: wrapped command buffer.
     * Returns count of args written to argv_out. */
    hu_error_t (*wrap_command)(void *ctx, const char **argv_in, size_t argc_in,
                               const char **argv_out, size_t max_out, size_t *argc_out);
} hu_runtime_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Factory
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_runtime_kind {
    HU_RUNTIME_NATIVE,
    HU_RUNTIME_DOCKER,
    HU_RUNTIME_WASM,
    HU_RUNTIME_CLOUDFLARE,
    HU_RUNTIME_GCE,
} hu_runtime_kind_t;

hu_runtime_t hu_runtime_native(void);
hu_runtime_t hu_runtime_docker(bool mount_workspace, uint64_t memory_limit_mb, const char *image,
                               const char *workspace);
hu_runtime_t hu_runtime_gce(const char *project, const char *zone, const char *instance,
                            uint64_t memory_limit_mb);
hu_runtime_t hu_runtime_wasm(uint64_t memory_limit_mb);
hu_runtime_t hu_runtime_cloudflare(void);

/* Select runtime from config; fills *out and returns HU_OK, or error for unknown kind. */
struct hu_config;
hu_error_t hu_runtime_from_config(const struct hu_config *cfg, hu_runtime_t *out);

#endif /* HU_RUNTIME_H */
