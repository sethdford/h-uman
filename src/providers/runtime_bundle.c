#include "seaclaw/providers/runtime_bundle.h"
#include "seaclaw/providers/reliable.h"
#include <stdint.h>
#include <string.h>

sc_error_t sc_runtime_bundle_init(sc_allocator_t *alloc, sc_provider_t primary, uint32_t retries,
                                  uint64_t backoff_ms, sc_runtime_bundle_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (retries > 0 || backoff_ms > 0) {
        sc_provider_t reliable;
        sc_error_t err = sc_reliable_create(alloc, primary, retries, backoff_ms, &reliable);
        if (err != SC_OK)
            return err;
        out->provider = reliable;
        out->inner_ctx = primary.ctx;
        out->inner_vtable = primary.vtable;
    } else {
        out->provider = primary;
    }
    return SC_OK;
}

sc_provider_t sc_runtime_bundle_provider(sc_runtime_bundle_t *bundle) {
    return bundle ? bundle->provider : (sc_provider_t){0};
}

void sc_runtime_bundle_deinit(sc_runtime_bundle_t *bundle, sc_allocator_t *alloc) {
    if (!bundle)
        return;
    if (bundle->provider.vtable && bundle->provider.vtable->deinit)
        bundle->provider.vtable->deinit(bundle->provider.ctx, alloc);
    if (bundle->inner_ctx && bundle->inner_vtable && bundle->inner_vtable->deinit) {
        /* Inner was wrapped by reliable; reliable's deinit already called inner's deinit */
        /* So we don't double-deinit */
    }
    memset(bundle, 0, sizeof(*bundle));
}
