#ifndef SC_RELIABLE_H
#define SC_RELIABLE_H

#include "seaclaw/provider.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>
#include <stdint.h>

/* Provider entry for multi-provider fallback chain */
typedef struct sc_reliable_provider_entry {
    const char *name;
    size_t name_len;
    sc_provider_t provider;
} sc_reliable_provider_entry_t;

/* Model + fallback models for per-model failover */
typedef struct sc_reliable_fallback_model {
    const char *model;
    size_t model_len;
} sc_reliable_fallback_model_t;

typedef struct sc_reliable_model_fallback_entry {
    const char *model;
    size_t model_len;
    const sc_reliable_fallback_model_t *fallbacks;
    size_t fallbacks_count;
} sc_reliable_model_fallback_entry_t;

/* Create a reliable provider that wraps an inner provider with retry and exponential backoff.
 * max_retries: number of retries (0 = no retries, 1 = 2 total attempts)
 * backoff_ms: initial backoff in ms (min 50), doubles each retry up to 10000ms.
 * extras: optional fallback providers (NULL = none). Caller owns the array and providers.
 * model_fallbacks: optional per-model fallback chains (NULL = none). Caller owns the array.
 * In SC_IS_TEST: skips sleep, retries immediately.
 * Multi-provider: tries inner, then each extra in order for each model in the chain.
 * Model fallback: for model X, if configured, tries [X, fallback1, fallback2, ...] */
sc_error_t sc_reliable_create(sc_allocator_t *alloc,
    sc_provider_t inner,
    uint32_t max_retries,
    uint64_t backoff_ms,
    sc_provider_t *out);

sc_error_t sc_reliable_create_ex(sc_allocator_t *alloc,
    sc_provider_t inner,
    uint32_t max_retries,
    uint64_t backoff_ms,
    const sc_reliable_provider_entry_t *extras,
    size_t extras_count,
    const sc_reliable_model_fallback_entry_t *model_fallbacks,
    size_t model_fallbacks_count,
    sc_provider_t *out);

#endif /* SC_RELIABLE_H */
