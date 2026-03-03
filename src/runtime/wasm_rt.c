#include "seaclaw/core/error.h"
#include "seaclaw/runtime.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct sc_wasm_runtime {
    uint64_t memory_limit_mb;
} sc_wasm_runtime_t;

static sc_wasm_runtime_t *get_ctx(void *ctx) {
    return (sc_wasm_runtime_t *)ctx;
}

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "wasm";
}

static bool impl_has_shell_access(void *ctx) {
    (void)ctx;
    return false;
}

static bool impl_has_filesystem_access(void *ctx) {
    (void)ctx;
    return false;
}

static const char *impl_storage_path(void *ctx) {
    (void)ctx;
    return ".seaclaw/wasm";
}

static bool impl_supports_long_running(void *ctx) {
    (void)ctx;
    return false;
}

static uint64_t impl_memory_budget(void *ctx) {
    sc_wasm_runtime_t *w = get_ctx(ctx);
    if (w->memory_limit_mb > 0)
        return w->memory_limit_mb * 1024 * 1024;
    return 64 * 1024 * 1024; /* default 64 MB */
}

static sc_error_t wasm_wrap_command(void *ctx, const char **argv_in, size_t argc_in,
                                    const char **argv_out, size_t max_out, size_t *argc_out) {
    (void)ctx;
    (void)argv_in;
    (void)argc_in;
    (void)argv_out;
    (void)max_out;
    (void)argc_out;
    return SC_ERR_NOT_SUPPORTED;
}

static const sc_runtime_vtable_t wasm_vtable = {
    .name = impl_name,
    .has_shell_access = impl_has_shell_access,
    .has_filesystem_access = impl_has_filesystem_access,
    .storage_path = impl_storage_path,
    .supports_long_running = impl_supports_long_running,
    .memory_budget = impl_memory_budget,
    .wrap_command = wasm_wrap_command,
};

sc_runtime_t sc_runtime_wasm(uint64_t memory_limit_mb) {
    static sc_wasm_runtime_t s_wasm = {0};
    s_wasm.memory_limit_mb = memory_limit_mb;
    return (sc_runtime_t){
        .ctx = &s_wasm,
        .vtable = &wasm_vtable,
    };
}
