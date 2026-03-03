#include "seaclaw/core/error.h"
#include "seaclaw/security/sandbox.h"
#include "seaclaw/security/sandbox_internal.h"

static sc_error_t noop_wrap(void *ctx, const char *const *argv, size_t argc, const char **buf,
                            size_t buf_count, size_t *out_count) {
    (void)ctx;
    if (!buf || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    if (buf_count < argc)
        return SC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < argc; i++)
        buf[i] = argv[i];
    *out_count = argc;
    return SC_OK;
}

static bool noop_available(void *ctx) {
    (void)ctx;
    return true;
}

static const char *noop_name(void *ctx) {
    (void)ctx;
    return "none";
}

static const char *noop_desc(void *ctx) {
    (void)ctx;
    return "No sandboxing (application-layer security only)";
}

static const sc_sandbox_vtable_t noop_vtable = {
    .wrap_command = noop_wrap,
    .apply = NULL,
    .is_available = noop_available,
    .name = noop_name,
    .description = noop_desc,
};

/* For detect.c */
sc_sandbox_t sc_noop_sandbox_get(sc_noop_sandbox_ctx_t *ctx) {
    sc_sandbox_t sb = {
        .ctx = ctx,
        .vtable = &noop_vtable,
    };
    return sb;
}
