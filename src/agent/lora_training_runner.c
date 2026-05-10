/* W14 LoRA training runner.
 *
 * Drains pending signals from a `hu_learner_t` (populated by
 * `hu_learner_bridge_emit_*` during normal turn handling) and dispatches
 * `hu_learner_train` during a sleep window. After a successful train the
 * runner enqueues a follow-up `HU_JOB_KV_CACHE_WARMING` job so the
 * inference cache is invalidated/repopulated against the freshly written
 * adapter — this is the "wire KV invalidation on adapter swap" half of
 * the W14 P0 contract.
 *
 * `user_data` carries an `hu_lora_runner_ctx_t *` that bundles the
 * learner, the scheduler (for the follow-up enqueue), and the optional
 * KV cache + semantic cache handles. None of the cache handles are
 * required — when absent the swap path simply skips that hook.
 *
 * Determinism: the runner reads no clocks, no RNGs. The learner's own
 * seed (set on the config) plus the drained signal array fully determine
 * the resulting adapter bytes. Replays with an empty pending buffer are
 * a no-op and return HU_OK. */

#include "human/agent/scheduler.h"
#include "human/agent/kv_cache.h"
#include "human/agent/lora_runner.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/memory/lifecycle/semantic_cache.h"
#include "human/ml/learner.h"
#include "human/ml/learner_bridge.h"
#include "human/provider.h"

#include <string.h>

hu_error_t hu_lora_training_runner(hu_memory_facade_t *m, const struct hu_job_spec *spec, int64_t budget_ms,
                                   void *user_data) {
    (void)m;
    if (!spec || !user_data)
        return HU_ERR_INVALID_ARGUMENT;
    hu_lora_runner_ctx_t *ctx = (hu_lora_runner_ctx_t *)user_data;
    if (!ctx->learner)
        return HU_OK;

    /* Drain pending signals. Empty-buffer drain is HU_OK with NULL out. */
    hu_training_signal_t *signals = NULL;
    size_t n = 0;
    hu_error_t e = hu_learner_pending_drain(ctx->learner, &signals, &n);
    if (e != HU_OK)
        return e;
    if (n == 0)
        return HU_OK;

    /* Compose train config. We honor the ctx-provided template (so the
     * caller can pin output path / model_version) and override only the
     * scheduler-derived budget. */
    hu_learner_config_t cfg = ctx->config_template;
    if (cfg.adapter_output_path[0] == '\0') {
        /* Caller forgot to pin a path. Fail fast — silent /tmp writes
         * would defeat the purpose of versioned adapter swap. */
        hu_learner_signals_free(ctx->alloc ? ctx->alloc : NULL, signals, n);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (budget_ms > 0)
        cfg.budget_ms = budget_ms;

    hu_learner_report_t report;
    memset(&report, 0, sizeof(report));
    hu_error_t train_err = hu_learner_train(ctx->learner, &cfg, signals, n, &report);

    /* Free the drained signal array regardless of train outcome. The
     * learner copies values it needs internally. */
    hu_allocator_t *alloc = ctx->alloc;
    if (!alloc) {
        /* Fallback — bridge functions used the system allocator. */
        hu_allocator_t sys = hu_system_allocator();
        hu_learner_signals_free(&sys, signals, n);
    } else {
        hu_learner_signals_free(alloc, signals, n);
    }

    if (train_err != HU_OK)
        return train_err;

    /* Successful adapter write → invalidate KV cache and semantic cache.
     * Both are best-effort: a missing cache is not an error. */
    if (ctx->kv_cache)
        hu_kv_cache_clear(ctx->kv_cache);
    if (ctx->semantic_cache_clear_fn && ctx->semantic_cache)
        ctx->semantic_cache_clear_fn(ctx->semantic_cache);

    /* W13 adapter auto-load: hot-load the new adapter on the active
     * provider so the next user turn uses the freshly-trained weights
     * without requiring a daemon restart. Providers that don't support
     * adapters (cloud APIs) return HU_ERR_NOT_SUPPORTED — harmless. */
    if (ctx->provider) {
        const char *aid = ctx->adapter_id;
        char id_buf[128];
        if (!aid || !*aid) {
            const char *base = strrchr(report.adapter_path, '/');
            base = base ? base + 1 : report.adapter_path;
            size_t blen = strlen(base);
            if (blen >= sizeof(id_buf))
                blen = sizeof(id_buf) - 1;
            memcpy(id_buf, base, blen);
            id_buf[blen] = '\0';
            aid = id_buf;
        }
        hu_allocator_t load_alloc;
        if (ctx->alloc)
            load_alloc = *ctx->alloc;
        else
            load_alloc = hu_system_allocator();
        hu_error_t le = hu_provider_load_adapter(
            ctx->provider, &load_alloc, report.adapter_path,
            strlen(report.adapter_path), aid, strlen(aid));
        if (le == HU_OK)
            hu_log_info("lora-runner", NULL,
                        "auto-loaded adapter '%s' from %s", aid, report.adapter_path);
        else if (le != HU_ERR_NOT_SUPPORTED)
            hu_log_warn("lora-runner", NULL,
                        "adapter auto-load failed for '%s': %d", aid, (int)le);
    }

    /* Enqueue a KV warming job so the cache is repopulated proactively
     * rather than lazily on next user turn. We honor `ctx->scheduler`
     * being NULL (some callers register us standalone). */
    if (ctx->scheduler) {
        hu_job_spec_t warm;
        memset(&warm, 0, sizeof(warm));
        warm.kind = HU_JOB_KV_CACHE_WARMING;
        warm.priority = 1;
        warm.budget_ms = 5000;
        (void)hu_scheduler_enqueue(ctx->scheduler, &warm);
    }
    return HU_OK;
}
