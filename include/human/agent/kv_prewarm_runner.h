#ifndef HU_AGENT_KV_PREWARM_RUNNER_H
#define HU_AGENT_KV_PREWARM_RUNNER_H

/* W14 — KV cache prewarm / eviction runner.
 *
 * Drives `hu_kv_cache_manager_t` housekeeping from the W14 scheduler so
 * the daemon doesn't have to call `hu_kv_cache_prune` directly on a
 * timer. Single function dispatched by `spec->kind` on EVICTION or
 * WARMING. `user_data` carries the cache manager (NULL → no-op).
 *
 * Tested in tests/test_w14_runners.c. */

#include "human/agent/kv_cache.h"
#include "human/agent/scheduler.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_kv_prewarm_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec, int64_t budget_ms,
                                void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_KV_PREWARM_RUNNER_H */
