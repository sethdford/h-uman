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

/** Phase 1 (RL SOTA) — entry-aware factory.
 *
 *  Constructs a provider from a hu_provider_entry_t, forwarding all
 *  fields (including llamacpp-specific tuning: context_size / threads /
 *  use_gpu / n_gpu_layers). For non-llamacpp providers this falls
 *  through to hu_provider_create with (name, api_key, base_url).
 *
 *  Use this when you have a parsed config entry; use hu_provider_create
 *  directly when you only have name + creds (e.g. CLI flag overrides).
 */
struct hu_provider_entry;
hu_error_t hu_provider_create_from_entry(hu_allocator_t *alloc,
                                         const struct hu_provider_entry *entry,
                                         hu_provider_t *out);

#ifdef HU_IS_TEST
/* Test-only hook: returns the most recent llamacpp config that
 * hu_provider_create_from_entry built (for tests/test_llamacpp_factory_config.c).
 * Returns NULL if no llamacpp entry has been processed yet. The
 * returned pointer is valid until the next entry is processed or
 * hu_llamacpp_factory_reset_for_test() is called. The model_path is
 * deep-copied so it remains valid after the factory frees the source. */
struct hu_llamacpp_config;
const struct hu_llamacpp_config *hu_llamacpp_factory_last_config(void);
void hu_llamacpp_factory_reset_for_test(void);
#endif

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
