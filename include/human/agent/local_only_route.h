#ifndef HU_AGENT_LOCAL_ONLY_ROUTE_H
#define HU_AGENT_LOCAL_ONLY_ROUTE_H

/* local_only routing for the model router (core/local_only_guard.h).
 *
 * hu_model_router_default_config() carries hard-coded cloud models
 * (gemini-3.1-pro-preview for analytical/deep, gemini-3.1-flash-lite for
 * reflexive). With local_only LIVE, every tier default becomes the primary
 * model instead, so a routed turn stays on the primary local model. Models
 * the operator configured (config agent.model_router.*) are applied on top
 * by the caller and are unchanged — the HTTP backstop still refuses any of
 * them that would reach a non-local endpoint. OFF and audit: no change. */

#include "human/agent/model_router.h"
#include <stdbool.h>
#include <stddef.h>

/* Returns true when it rewrote the tier defaults. */
bool hu_local_only_router_defaults(hu_model_router_config_t *cfg, const char *primary_model,
                                   size_t primary_model_len);

#endif /* HU_AGENT_LOCAL_ONLY_ROUTE_H */
