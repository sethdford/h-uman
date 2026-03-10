#include "human/providers/runtime_bundle.h"
#include "human/providers/reliable.h"
#include <stdint.h>
#include <string.h>

hu_error_t hu_runtime_bundle_init(hu_allocator_t *alloc, hu_provider_t primary, uint32_t retries,
                                  uint64_t backoff_ms, hu_runtime_bundle_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (retries > 0 || backoff_ms > 0) {
        hu_provider_t reliable;
        hu_error_t err = hu_reliable_create(alloc, primary, retries, backoff_ms, &reliable);
        if (err != HU_OK)
            return err;
        out->provider = reliable;
        out->inner_ctx = primary.ctx;
        out->inner_vtable = primary.vtable;
    } else {
        out->provider = primary;
    }
    return HU_OK;
}

hu_provider_t hu_runtime_bundle_provider(hu_runtime_bundle_t *bundle) {
    return bundle ? bundle->provider : (hu_provider_t){0};
}

void hu_runtime_bundle_deinit(hu_runtime_bundle_t *bundle, hu_allocator_t *alloc) {
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
