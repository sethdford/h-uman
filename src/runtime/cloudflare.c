#include "human/core/error.h"
#include "human/runtime.h"
#include <stdint.h>

static char cf_ctx_dummy;

static hu_error_t cf_wrap_command(void *ctx, const char **argv_in, size_t argc_in,
                                  const char **argv_out, size_t max_out, size_t *argc_out) {
    (void)ctx;
    (void)argv_in;
    (void)argc_in;
    (void)argv_out;
    (void)max_out;
    (void)argc_out;
    return HU_ERR_NOT_SUPPORTED;
}

static const char *cf_name(void *ctx) {
    (void)ctx;
    return "cloudflare";
}
static bool cf_has_shell(void *ctx) {
    (void)ctx;
    return false;
}
static bool cf_has_fs(void *ctx) {
    (void)ctx;
    return false;
}
static const char *cf_storage_path(void *ctx) {
    (void)ctx;
    return "";
}
static bool cf_long_running(void *ctx) {
    (void)ctx;
    return false;
}
static uint64_t cf_memory_budget(void *ctx) {
    (void)ctx;
    return 128ULL * 1024 * 1024;
}

static const hu_runtime_vtable_t cloudflare_vtable = {
    .name = cf_name,
    .has_shell_access = cf_has_shell,
    .has_filesystem_access = cf_has_fs,
    .storage_path = cf_storage_path,
    .supports_long_running = cf_long_running,
    .memory_budget = cf_memory_budget,
    .wrap_command = cf_wrap_command,
};

hu_runtime_t hu_runtime_cloudflare(void) {
    return (hu_runtime_t){.ctx = &cf_ctx_dummy, .vtable = &cloudflare_vtable};
}
