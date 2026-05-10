/* W14 KV cache prewarm/eviction runner.
 *
 * Single function that handles both `HU_JOB_KV_CACHE_EVICTION` and
 * `HU_JOB_KV_CACHE_WARMING` so the scheduler can drive cache
 * housekeeping uniformly. The behavior split is intentionally minimal
 * because the underlying kv_cache_manager_t exposes a small surface:
 *
 *   EVICTION:  if utilization > threshold, prune unpinned segments.
 *              No-op when the cache is healthy.
 *   WARMING:   no-op today — the agent turn re-adds segments lazily on
 *              demand. We keep the runner so the scheduler gets the
 *              follow-up call after an adapter swap (`lora_training_runner`
 *              enqueues this kind), and so a future implementation can
 *              materialise system-prompt/persona segments here.
 *
 * `user_data` is the `hu_kv_cache_manager_t *`. NULL is treated as
 * a silent no-op so callers can register the runner unconditionally. */

#include "human/agent/kv_cache.h"
#include "human/agent/scheduler.h"

#define HU_KV_PRUNE_MAX_SLOTS 8

hu_error_t hu_kv_prewarm_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec, int64_t budget_ms,
                                void *user_data) {
    (void)m;
    (void)budget_ms;
    if (!spec)
        return HU_ERR_INVALID_ARGUMENT;
    hu_kv_cache_manager_t *mgr = (hu_kv_cache_manager_t *)user_data;
    if (!mgr)
        return HU_OK;

    switch (spec->kind) {
    case HU_JOB_KV_CACHE_EVICTION: {
        if (!hu_kv_cache_needs_eviction(mgr))
            return HU_OK;
        const char *evicted[HU_KV_PRUNE_MAX_SLOTS];
        (void)hu_kv_cache_prune(mgr, evicted, HU_KV_PRUNE_MAX_SLOTS);
        return HU_OK;
    }
    case HU_JOB_KV_CACHE_WARMING:
        /* Warming hook — nothing to materialise yet. The agent turn
         * lazily adds the system prompt + persona segments. Once we
         * have a stable "warm-set", populate it here. */
        return HU_OK;
    default:
        return HU_OK;
    }
}
