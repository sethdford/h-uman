/* Local MLX health probe — checks adapter file presence and server reachability.
 *
 * Sets cfg->mlx_local_healthy based on:
 * (a) Adapter file exists and is nonzero size (path from personalization.lora_adapter_path)
 * (b) MLX server reachability via cached ping (~60s TTL, reuses hu_http_post_json via
 *     hu_mlx_admin_probe_health which calls hu_http_get)
 *
 * Never blocks the turn — stale-but-cached is acceptable. Network ping is guarded
 * by HU_IS_TEST so tests never hit the network. Follows the pattern at
 * src/agent/model_router.c:219 ("router stays pure: mlx_local_healthy is set by the
 * caller").
 */

#include "human/agent/model_router_health.h"

#include "human/core/error.h"
#include "human/core/log.h"
#include "human/ml/mlx_admin.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Check if a file exists and has nonzero size. */
static bool adapter_file_healthy(const char *path) {
    if (!path || path[0] == '\0')
        return false;

#ifdef HU_IS_TEST
    /* Tests mock file access via test helpers; allow stat() for fixture validation. */
#endif

    struct stat st;
    if (stat(path, &st) != 0)
        return false; /* file doesn't exist or is inaccessible */

    return st.st_size > 0;
}

void hu_model_router_health_probe(hu_allocator_t *alloc, const hu_config_t *cfg,
                                  hu_model_router_config_t *mr_cfg) {
    if (!cfg || !mr_cfg)
        return;

    /* Health requires BOTH:
     * (a) Adapter file present and nonzero size
     * (b) MLX server reachable (cached ping, network guarded by HU_IS_TEST) */

    if (!cfg->personalization.lora_adapter_path ||
        cfg->personalization.lora_adapter_path[0] == '\0') {
        mr_cfg->mlx_local_healthy = false;
        return;
    }

    if (!adapter_file_healthy(cfg->personalization.lora_adapter_path)) {
        mr_cfg->mlx_local_healthy = false;
        return;
    }

    /* Adapter file is present; check server reachability.
     * hu_mlx_admin_probe_health handles the network ping with caching and
     * HU_IS_TEST guards internally. */
    const char *mlx_url = hu_config_get_provider_base_url(cfg, "mlx_local");
    if (mlx_url && mlx_url[0]) {
        mr_cfg->mlx_local_healthy = hu_mlx_admin_probe_health(alloc, mlx_url, strlen(mlx_url));
    } else {
        mr_cfg->mlx_local_healthy = false;
    }
}
