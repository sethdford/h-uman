#ifndef SC_RUNTIME_BUNDLE_H
#define SC_RUNTIME_BUNDLE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/provider.h"
#include <stdint.h>

/* Minimal runtime provider bundle: holds a single provider, optionally wrapped with reliable.
 * For full config-driven fallbacks, use the config loader. */
typedef struct sc_runtime_bundle {
    sc_provider_t provider; /* primary or reliable-wrapped */
    void *inner_ctx;        /* for deinit: original provider ctx if wrapped */
    const sc_provider_vtable_t *inner_vtable;
} sc_runtime_bundle_t;

/* Initialize with a provider. Optionally wrap with reliable. */
sc_error_t sc_runtime_bundle_init(sc_allocator_t *alloc, sc_provider_t primary, uint32_t retries,
                                  uint64_t backoff_ms, sc_runtime_bundle_t *out);

/* Get the provider (possibly reliable-wrapped). */
sc_provider_t sc_runtime_bundle_provider(sc_runtime_bundle_t *bundle);

/* Free resources. */
void sc_runtime_bundle_deinit(sc_runtime_bundle_t *bundle, sc_allocator_t *alloc);

#endif /* SC_RUNTIME_BUNDLE_H */
