#ifndef HU_PROVIDERS_LOCAL_ONLY_CONFIG_H
#define HU_PROVIDERS_LOCAL_ONLY_CONFIG_H

/* Config half of the local-only switch (core/local_only_guard.h holds the
 * mode and the backstop). Lives in providers/ because "which endpoint does
 * the primary provider talk to" needs the compatible-provider URL table. */

#include "human/config.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>

/* Name of the provider every reply goes through first: default_provider, or
 * for the composites "reliable"/"router" the provider they wrap first
 * (reliability.primary_provider / router.standard, "openai" when unset —
 * the same defaults providers/from_config.c applies). */
const char *hu_config_primary_provider_name(const hu_config_t *cfg);

/* That provider's base URL: the configured providers[].base_url, else the
 * compatible-provider default. NULL when neither exists. */
const char *hu_config_primary_endpoint(const hu_config_t *cfg);

/* hu_provider_endpoint_is_local(hu_config_primary_endpoint(cfg)). */
bool hu_config_primary_is_local(const hu_config_t *cfg);

/* hu_local_only_resolve(getenv("HU_LOCAL_ONLY"), privacy.local_only, primary
 * endpoint locality). */
hu_gate_mode_t hu_config_local_only_mode(const hu_config_t *cfg);

/* Resolve, hand the result to hu_local_only_configure(), log one INFO line
 * naming the mode and why. Called once at startup (app bootstrap). */
hu_gate_mode_t hu_config_apply_local_only(const hu_config_t *cfg);

#endif /* HU_PROVIDERS_LOCAL_ONLY_CONFIG_H */
