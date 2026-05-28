#ifndef HU_MODEL_ROUTER_HEALTH_H
#define HU_MODEL_ROUTER_HEALTH_H

#include "human/agent/model_router.h"
#include "human/config.h"
#include "human/core/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probe local MLX health: checks adapter file presence + server reachability.
 *
 * Sets cfg->mlx_local_healthy based on:
 * (a) Adapter file exists and is nonzero size (from personalization.lora_adapter_path)
 * (b) MLX server responds to a health check (cached ~60s TTL via hu_mlx_admin_probe_health)
 *
 * If either check fails, mlx_local_healthy is set false. Never blocks the turn;
 * stale-but-cached server response is acceptable. Network ping is guarded by
 * HU_IS_TEST internally by hu_mlx_admin_probe_health.
 *
 * Call this once per turn (or at daemon startup) before hu_model_route to populate
 * the router's input. The router itself stays pure: mlx_local_healthy is caller-set. */
void hu_model_router_health_probe(hu_allocator_t *alloc, const hu_config_t *cfg,
                                  hu_model_router_config_t *mr_cfg);

#ifdef __cplusplus
}
#endif

#endif /* HU_MODEL_ROUTER_HEALTH_H */
