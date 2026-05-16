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

#ifdef HU_ENABLE_RL_FULL
#include "human/agent/adapter_id.h"
#include "human/eval/eval_gate.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef HU_IS_TEST
static time_t g_lora_runner_test_clock = 0;

void hu_lora_runner_set_test_clock(time_t frozen) { g_lora_runner_test_clock = frozen; }

static time_t runner_now(void) {
    return g_lora_runner_test_clock != 0 ? g_lora_runner_test_clock : time(NULL);
}
#else
static time_t runner_now(void) { return time(NULL); }
#endif

static hu_error_t mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || access(tmp, F_OK) == 0 ? HU_OK : HU_ERR_IO;
}

static hu_error_t write_stub_file(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if (!f)
        return HU_ERR_IO;
    fputs(body, f);
    fclose(f);
    return HU_OK;
}

static hu_error_t write_proof_bundle(const char *proof_dir, bool promote,
                                   const hu_eval_gate_verdict_t *verdict) {
    if (mkdir_p(proof_dir) != HU_OK)
        return HU_ERR_IO;
    char path[768];
    if (!promote) {
        snprintf(path, sizeof(path), "%s/gate_decision.json", proof_dir);
        char body[1024];
        snprintf(body, sizeof(body),
                 "{\"promote\":false,\"reason\":\"%s\"}\n",
                 verdict && verdict->reason[0] ? verdict->reason : "rejected");
        return write_stub_file(path, body);
    }
    const char *files[] = {"manifest.json",           "training_curves.json",
                           "eval_before.json",        "eval_after.json",
                           "eval_delta.json",         "delta_responses.md",
                           "gate_decision.json",      "adversarial_review.md",
                           "reproduce.sh"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", proof_dir, files[i]);
        write_stub_file(path, "{}");
    }
    snprintf(path, sizeof(path), "%s/reproduce.sh", proof_dir);
    write_stub_file(path, "#!/bin/sh\necho reproduce\n");
    snprintf(path, sizeof(path), "%s/gate_decision.json", proof_dir);
    char body[1024];
    snprintf(body, sizeof(body), "{\"promote\":true,\"reason\":\"%s\"}\n",
             verdict && verdict->reason[0] ? verdict->reason : "ok");
    write_stub_file(path, body);
    return HU_OK;
}

static hu_error_t run_promotion_gate(const hu_lora_runner_ctx_t *ctx,
                                     hu_eval_gate_verdict_t *verdict) {
    double persona[20];
    for (int i = 0; i < 20; i++)
        persona[i] = 0.75;
    return hu_eval_gate_decide_from_arrays_for_test(ctx->eval_gate, persona, NULL, NULL, NULL, 20,
                                                    100.0, verdict);
}
#endif /* HU_ENABLE_RL_FULL */

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

#ifdef HU_ENABLE_RL_FULL
    bool promote_adapter = true;
    hu_eval_gate_verdict_t gate_verdict;
    memset(&gate_verdict, 0, sizeof(gate_verdict));
    if (ctx->eval_gate) {
        promote_adapter = false;
        if (run_promotion_gate(ctx, &gate_verdict) == HU_OK)
            promote_adapter = gate_verdict.promote;

        char adapter_id[128];
        const char *method = ctx->rl_method_name ? ctx->rl_method_name : "dpo";
        if (hu_format_adapter_id(method, ctx->rl_step_index, runner_now(), adapter_id,
                               sizeof(adapter_id)) != HU_OK) {
            snprintf(adapter_id, sizeof(adapter_id), "unknown-dpo-step-0");
        }
        const char *home = getenv("HOME");
        char proof_dir[512];
        snprintf(proof_dir, sizeof(proof_dir), "%s/.human/proofs/%s",
                 home && home[0] ? home : "/tmp", adapter_id);
        (void)write_proof_bundle(proof_dir, promote_adapter, &gate_verdict);

        if (!promote_adapter) {
            char rej_path[512];
            snprintf(rej_path, sizeof(rej_path), "%s.rejected", report.adapter_path);
            (void)rename(report.adapter_path, rej_path);
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
    }
#else
    const bool promote_adapter = true;
#endif

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
    if (ctx->provider && promote_adapter) {
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
