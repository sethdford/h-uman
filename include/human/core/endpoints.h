#ifndef HU_CORE_ENDPOINTS_H
#define HU_CORE_ENDPOINTS_H

/* Canonical default endpoint for the local MLX server (scripts/mlx-server.py).
 *
 * Before 2026-07-27 this address existed as ~16 bare literals across
 * providers/factory.c, agent/agent.c, agent/cli.c, agent/lora_training_runner.c,
 * voice/provider_factory.c, voice/mlx_local.c, ml/lora_nightly.c, daemon.c and
 * evaluation/evaluation_locomo.c — in three spellings ("http://127.0.0.1:8741/v1",
 * "http://127.0.0.1:8741", "http://localhost:8741/v1") plus the bare port 8741
 * for reachability probes. Moving the serving base (the 2026-07-26 GLM flip)
 * therefore meant finding and editing every one of them, and missing one failed
 * silently: a stale default just points at whatever is on the old port.
 *
 * The three forms below derive from a single port so they cannot drift apart.
 * Use the widest one that fits the call site:
 *
 *   HU_MLX_DEFAULT_PORT       int    — socket reachability probes
 *   HU_MLX_DEFAULT_ORIGIN     string — scheme+host+port, no path (voice/SSE)
 *   HU_MLX_DEFAULT_BASE_URL   string — OpenAI-compatible base (`/v1`)
 *
 * These are DEFAULTS only. Any call site that can read an operator-supplied
 * base URL from config must still prefer the config value; these apply when
 * nothing is configured. */

#define HU_MLX_DEFAULT_PORT     8741
#define HU_MLX_DEFAULT_PORT_STR "8741"

#define HU_MLX_DEFAULT_ORIGIN   "http://127.0.0.1:" HU_MLX_DEFAULT_PORT_STR
#define HU_MLX_DEFAULT_BASE_URL HU_MLX_DEFAULT_ORIGIN "/v1"

#endif /* HU_CORE_ENDPOINTS_H */
