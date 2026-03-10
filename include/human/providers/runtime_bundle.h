#ifndef HU_RUNTIME_BUNDLE_H
#define HU_RUNTIME_BUNDLE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdint.h>

/* Minimal runtime provider bundle: holds a single provider, optionally wrapped with reliable.
 * For full config-driven fallbacks, use the config loader. */
typedef struct hu_runtime_bundle {
    hu_provider_t provider; /* primary or reliable-wrapped */
    void *inner_ctx;        /* for deinit: original provider ctx if wrapped */
    const hu_provider_vtable_t *inner_vtable;
} hu_runtime_bundle_t;

/* Initialize with a provider. Optionally wrap with reliable. */
hu_error_t hu_runtime_bundle_init(hu_allocator_t *alloc, hu_provider_t primary, uint32_t retries,
                                  uint64_t backoff_ms, hu_runtime_bundle_t *out);

/* Get the provider (possibly reliable-wrapped). */
hu_provider_t hu_runtime_bundle_provider(hu_runtime_bundle_t *bundle);

/* Free resources. */
void hu_runtime_bundle_deinit(hu_runtime_bundle_t *bundle, hu_allocator_t *alloc);

#endif /* HU_RUNTIME_BUNDLE_H */
