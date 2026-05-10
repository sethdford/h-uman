#ifndef HU_PROVIDERS_FACTORY_H
#define HU_PROVIDERS_FACTORY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>

struct hu_config;
hu_error_t hu_provider_create_from_config(hu_allocator_t *alloc, const struct hu_config *cfg,
                                          const char *name, size_t name_len, hu_provider_t *out);

hu_error_t hu_provider_create(hu_allocator_t *alloc, const char *name, size_t name_len,
                              const char *api_key, size_t api_key_len, const char *base_url,
                              size_t base_url_len, hu_provider_t *out);

/** Construct the agent's default provider from `cfg->default_provider`, automatically
 *  wrapping it with `cfg->reliability.fallback_providers[]` (if any) so a single
 *  failing provider transparently fails over to the next.
 *
 *  Behaviour matrix:
 *  - default is "router" / "ensemble" / "reliable"  → no auto-wrap (composite handles it)
 *  - default is plain AND fallback list empty       → identical to plain create
 *  - default is plain AND fallback list non-empty   → wraps default + chain in
 *                                                     hu_reliable_create_ex with
 *                                                     `provider_retries` retries and
 *                                                     `provider_backoff_ms` base backoff
 *
 *  On error, no resources are leaked and `*out` is left zeroed.
 */
hu_error_t hu_provider_create_default(hu_allocator_t *alloc, const struct hu_config *cfg,
                                      hu_provider_t *out);

/** Returns base URL for compatible providers (groq, mistral, etc.), NULL if unknown. */
const char *hu_compatible_provider_url(const char *name);

#endif /* HU_PROVIDERS_FACTORY_H */
