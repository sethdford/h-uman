#include "seaclaw/runtime.h"
#include <stdint.h>
#include <string.h>

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "native";
}

static bool impl_has_shell_access(void *ctx) {
    (void)ctx;
    return true;
}

static bool impl_has_filesystem_access(void *ctx) {
    (void)ctx;
    return true;
}

static const char *impl_storage_path(void *ctx) {
    (void)ctx;
    return "~/.seaclaw/";
}

static bool impl_supports_long_running(void *ctx) {
    (void)ctx;
    return true;
}

static uint64_t impl_memory_budget(void *ctx) {
    (void)ctx;
    return 5 * 1024 * 1024;  /* 5 MB */
}

static const sc_runtime_vtable_t native_vtable = {
    .name = impl_name,
    .has_shell_access = impl_has_shell_access,
    .has_filesystem_access = impl_has_filesystem_access,
    .storage_path = impl_storage_path,
    .supports_long_running = impl_supports_long_running,
    .memory_budget = impl_memory_budget,
};

static char s_native_dummy;
static sc_runtime_t s_native = {
    .ctx = &s_native_dummy,
    .vtable = &native_vtable,
};

sc_runtime_t sc_runtime_native(void) {
    return s_native;
}
