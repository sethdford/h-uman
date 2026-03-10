#ifndef HU_RELIABLE_H
#define HU_RELIABLE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>
#include <stdint.h>

/* Provider entry for multi-provider fallback chain */
typedef struct hu_reliable_provider_entry {
    const char *name;
    size_t name_len;
    hu_provider_t provider;
} hu_reliable_provider_entry_t;

/* Model + fallback models for per-model failover */
typedef struct hu_reliable_fallback_model {
    const char *model;
    size_t model_len;
} hu_reliable_fallback_model_t;

typedef struct hu_reliable_model_fallback_entry {
    const char *model;
    size_t model_len;
    const hu_reliable_fallback_model_t *fallbacks;
    size_t fallbacks_count;
} hu_reliable_model_fallback_entry_t;

typedef struct hu_reliable_config {
    hu_provider_t primary;
    hu_provider_t fallback;       /* optional, zeroed if none */
    int max_retries;              /* default 3 */
    int base_delay_ms;            /* default 1000 */
    int max_delay_ms;             /* default 30000 */
    int failure_threshold;        /* default 5 */
    int recovery_timeout_seconds; /* default 60 */
} hu_reliable_config_t;

/* Create a reliable provider from config (retry, fallback, circuit breaker). */
hu_error_t hu_reliable_provider_create(hu_allocator_t *alloc, const hu_reliable_config_t *config,
                                       hu_provider_t *out);

/* Create a reliable provider that wraps an inner provider with retry and exponential backoff.
 * max_retries: number of retries (0 = no retries, 1 = 2 total attempts)
 * backoff_ms: initial backoff in ms (min 50), doubles each retry up to 10000ms.
 * extras: optional fallback providers (NULL = none). Caller owns the array and providers.
 * model_fallbacks: optional per-model fallback chains (NULL = none). Caller owns the array.
 * In HU_IS_TEST: skips sleep, retries immediately.
 * Multi-provider: tries inner, then each extra in order for each model in the chain.
 * Model fallback: for model X, if configured, tries [X, fallback1, fallback2, ...] */
hu_error_t hu_reliable_create(hu_allocator_t *alloc, hu_provider_t inner, uint32_t max_retries,
                              uint64_t backoff_ms, hu_provider_t *out);

hu_error_t hu_reliable_create_ex(hu_allocator_t *alloc, hu_provider_t inner, uint32_t max_retries,
                                 uint64_t backoff_ms, const hu_reliable_provider_entry_t *extras,
                                 size_t extras_count,
                                 const hu_reliable_model_fallback_entry_t *model_fallbacks,
                                 size_t model_fallbacks_count, hu_provider_t *out);

#endif /* HU_RELIABLE_H */
