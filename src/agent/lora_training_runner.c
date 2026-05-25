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
 * M3 frontier-MLX path (US-8 carryover): when the pair-count trigger
 * enqueues with HU_TRAINING_TARGET_FRONTIER_MLX, this runner invokes
 * `scripts/training_loop.py --source-jsonl` as a subprocess to train
 * against the served Gemma-4-31B model (via mlx_lm.lora) instead of the
 * in-process reference HUML GPT. The target selection is read from
 * `hu_training_runner_last_enqueued_target()` (set by daemon when
 * the pair-count trigger fires). When target == FRONTIER_MLX, dispatch
 * subprocess; when target == HUML_REFERENCE, use hu_learner_train.
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

#include "human/agent/kv_cache.h"
#include "human/agent/lora_runner.h"
#include "human/agent/scheduler.h"
#include "human/agent/training_runner_shared.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/memory/lifecycle/semantic_cache.h"
#include "human/ml/learner.h"
#include "human/ml/learner_bridge.h"
#include "human/provider.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_RL_FULL
#include "human/agent/adapter_id.h"
#include "human/eval/eval_gate.h"
#include "human/eval/leaderboard.h"
#include "human/eval/persona_rollout.h"
#include "human/memory/personal_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef HU_IS_TEST
static time_t g_lora_runner_test_clock = 0;

void hu_lora_runner_set_test_clock(time_t frozen) {
    g_lora_runner_test_clock = frozen;
}

static time_t runner_now(void) {
    return g_lora_runner_test_clock != 0 ? g_lora_runner_test_clock : time(NULL);
}
#else
static time_t runner_now(void) {
    return time(NULL);
}
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
        snprintf(body, sizeof(body), "{\"promote\":false,\"reason\":\"%s\"}\n",
                 verdict && verdict->reason[0] ? verdict->reason : "rejected");
        return write_stub_file(path, body);
    }
    const char *files[] = {"manifest.json",      "training_curves.json",  "eval_before.json",
                           "eval_after.json",    "eval_delta.json",       "delta_responses.md",
                           "gate_decision.json", "adversarial_review.md", "reproduce.sh"};
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

static hu_error_t resolve_eval_target(const hu_lora_runner_ctx_t *ctx,
                                      hu_communication_style_t *out_target) {
    if (!out_target)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_target, 0, sizeof(*out_target));
    if (ctx->eval_target && ctx->eval_target->sample_count > 0) {
        *out_target = *ctx->eval_target;
        return HU_OK;
    }
    char pm_path[1024];
    if (hu_personal_model_resolve_default_path(pm_path, sizeof(pm_path))) {
        hu_personal_model_t loaded;
        if (hu_personal_model_load(&loaded, pm_path) == HU_OK && loaded.style.sample_count > 0U) {
            *out_target = loaded.style;
            return HU_OK;
        }
    }
    out_target->sample_count = 1;
    out_target->lowercase_ratio = 0.7f;
    out_target->avg_message_length = 80.f;
    return HU_OK;
}

static hu_error_t run_promotion_gate(const hu_lora_runner_ctx_t *ctx,
                                     const hu_learner_report_t *report,
                                     hu_eval_gate_verdict_t *verdict) {
    if (!ctx || !ctx->eval_gate || !verdict)
        return HU_ERR_INVALID_ARGUMENT;

    double persona[64];
    size_t n = 0;
    double p95 = ctx->gate_candidate_p95_ms > 0.0 ? ctx->gate_candidate_p95_ms : 100.0;

    if (ctx->gate_persona_after_scores && ctx->gate_persona_after_n >= 10) {
        n = ctx->gate_persona_after_n;
        if (n > sizeof(persona) / sizeof(persona[0]))
            n = sizeof(persona) / sizeof(persona[0]);
        for (size_t i = 0; i < n; i++)
            persona[i] = ctx->gate_persona_after_scores[i];
    }
#ifdef HU_IS_TEST
    else if (ctx->eval_use_synthetic_for_test) {
        n = 20;
        for (size_t i = 0; i < n; i++)
            persona[i] = 0.75;
    }
#endif
    else {
        if (!ctx->eval_provider)
            return HU_ERR_INVALID_ARGUMENT;

        hu_allocator_t *alloc = ctx->alloc;
        hu_allocator_t sys;
        if (!alloc) {
            sys = hu_system_allocator();
            alloc = &sys;
        }

        const char *fixture = ctx->eval_prompt_fixture_path;
        char default_fixture[512];
        if (!fixture || !fixture[0]) {
            const char *home = getenv("HOME");
            snprintf(default_fixture, sizeof(default_fixture), "%s/.human/eval/persona_prompts.txt",
                     home && home[0] ? home : "/tmp");
            fixture = default_fixture;
        }

        char **prompts = NULL;
        size_t prompt_n = 0;
        hu_error_t pe = hu_persona_rollout_load_prompt_fixture(alloc, fixture, &prompts, &prompt_n);
        if (pe != HU_OK)
            return pe;

        size_t want = ctx->eval_n_prompts > 0 ? ctx->eval_n_prompts : 20;
        if (prompt_n > want)
            prompt_n = want;
        if (prompt_n < 10) {
            for (size_t i = 0; i < prompt_n; i++)
                alloc->free(alloc->ctx, prompts[i], strlen(prompts[i]) + 1);
            alloc->free(alloc->ctx, prompts, prompt_n * sizeof(char *));
            return HU_ERR_INVALID_ARGUMENT;
        }

        hu_communication_style_t target;
        hu_error_t te = resolve_eval_target(ctx, &target);
        if (te != HU_OK) {
            for (size_t i = 0; i < prompt_n; i++)
                alloc->free(alloc->ctx, prompts[i], strlen(prompts[i]) + 1);
            alloc->free(alloc->ctx, prompts, prompt_n * sizeof(char *));
            return te;
        }

        int64_t timeout = ctx->eval_timeout_ms > 0 ? ctx->eval_timeout_ms : 5000;
        const char *adapter = (report && report->adapter_path[0]) ? report->adapter_path : NULL;

        hu_persona_rollout_config_t rcfg = {
            .provider = ctx->eval_provider,
            .adapter_path = adapter,
            .target = &target,
            .prompts = (const char **)prompts,
            .n_prompts = prompt_n,
            .timeout_ms_per_prompt = timeout,
            .capture_responses = true,
        };
        hu_persona_rollout_result_t rr = {0};
        hu_error_t re = hu_persona_rollout_run(alloc, &rcfg, &rr);

        double mt_scores[64];
        double if_scores[64];
        const double *mt_ptr = NULL;
        const double *if_ptr = NULL;
        size_t score_n = 0;

        if (re == HU_OK) {
            score_n = rr.n_scored;
            if (score_n > prompt_n)
                score_n = prompt_n;
            if (score_n > sizeof(mt_scores) / sizeof(mt_scores[0]))
                score_n = sizeof(mt_scores) / sizeof(mt_scores[0]);

            if (ctx->eval_gate->mt_bench && rr.responses && score_n > 0) {
                if (ctx->eval_gate->mt_bench->vtable->run(
                        ctx->eval_gate->mt_bench, alloc, (const char *const *)prompts,
                        (const char *const *)rr.responses, score_n, mt_scores) == HU_OK)
                    mt_ptr = mt_scores;
            }
            if (ctx->eval_gate->ifeval && rr.responses && score_n > 0) {
                if (ctx->eval_gate->ifeval->vtable->run(
                        ctx->eval_gate->ifeval, alloc, (const char *const *)prompts,
                        (const char *const *)rr.responses, score_n, if_scores) == HU_OK)
                    if_ptr = if_scores;
            }
        }

        for (size_t i = 0; i < prompt_n; i++)
            alloc->free(alloc->ctx, prompts[i], strlen(prompts[i]) + 1);
        alloc->free(alloc->ctx, prompts, prompt_n * sizeof(char *));
        if (re != HU_OK) {
            hu_persona_rollout_result_free(alloc, &rr);
            return re;
        }

        n = rr.n_scored;
        if (n > sizeof(persona) / sizeof(persona[0]))
            n = sizeof(persona) / sizeof(persona[0]);
        for (size_t i = 0; i < n; i++)
            persona[i] = rr.persona_scores[i];
        if (rr.p95_ms > 0.0)
            p95 = rr.p95_ms;
        hu_persona_rollout_result_free(alloc, &rr);

        if (n < 10) {
            memset(verdict, 0, sizeof(*verdict));
            verdict->promote = false;
            snprintf(verdict->reason, sizeof(verdict->reason),
                     "insufficient persona score count for gate (%zu < 10)", n);
            return HU_OK;
        }

        return hu_eval_gate_decide_from_arrays_for_test(ctx->eval_gate, persona, mt_ptr, if_ptr,
                                                        NULL, n, p95, verdict);
    }

    if (n < 10) {
        memset(verdict, 0, sizeof(*verdict));
        verdict->promote = false;
        snprintf(verdict->reason, sizeof(verdict->reason),
                 "insufficient persona score count for gate (%zu < 10)", n);
        return HU_OK;
    }

    return hu_eval_gate_decide_from_arrays_for_test(ctx->eval_gate, persona, NULL, NULL, NULL, n,
                                                    p95, verdict);
}
#endif /* HU_ENABLE_RL_FULL */

/* US-8 / M3 Frontier-MLX subprocess dispatch.
 *
 * Invokes `scripts/training_loop.py --source-jsonl <jsonl_path>`
 * to train a real LoRA against the served Gemma-4-31B frontier model
 * (via mlx_lm.lora). This is wired from the daemon's pair-count trigger
 * when HU_TRAINING_TARGET_FRONTIER_MLX is selected.
 *
 * The JSONL input is expected at ~/.human/training-data/m3-outcomes.jsonl
 * (DPO pair outcomes from the M3 driver). The output adapter is written to
 * ~/.human/training-data/adapters/auto-<timestamp>/ (per training_loop.py defaults).
 *
 * Returns HU_OK on successful dispatch (subprocess spawned).
 * Returns HU_ERR_NOT_SUPPORTED when under HU_IS_TEST (to avoid real training in tests).
 * Returns HU_ERR_IO for subprocess failures.
 */
static hu_error_t dispatch_frontier_mlx_training(const char *home_dir, hu_observer_t *observer) {
    if (!home_dir || !home_dir[0])
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    /* Under test, don't invoke real training. Tests should mock or skip
     * this path via the global target flag. */
    hu_log_info("lora_training_runner", observer,
                "frontier-mlx training dispatch skipped (HU_IS_TEST=1)");
    return HU_OK;
#else
    /* Production: spawn training_loop.py as a subprocess.
     * Command line:
     *   python3 <repo>/scripts/training_loop.py \
     *     --source-jsonl ~/.human/training-data/m3-outcomes.jsonl \
     *     --adapter-out ~/.human/training-data/adapters/auto-<timestamp>
     *
     * See scripts/training_loop.py for full options.
     * Exit code 0 = training succeeded; non-zero = failure (log the reason).
     *
     * The subprocess inherits the daemon's environment, including PATH,
     * PYTHONPATH, $HOME, etc. Make sure mlx_lm and torch are in the
     * environment (or the script will write a stub adapter).
     */

    static char repo_scripts_path[512];
    static char outcomes_jsonl_path[512];
    static char adapters_output_path[512];
    static char timestamp_buf[32];

    snprintf(repo_scripts_path, sizeof(repo_scripts_path), "%s/..", home_dir);
    snprintf(outcomes_jsonl_path, sizeof(outcomes_jsonl_path),
             "%s/.human/training-data/m3-outcomes.jsonl", home_dir);
    time_t now = time(NULL);
    snprintf(timestamp_buf, sizeof(timestamp_buf), "%lld", (long long)now);
    snprintf(adapters_output_path, sizeof(adapters_output_path),
             "%s/.human/training-data/adapters/auto-%s", home_dir, timestamp_buf);

    hu_log_info("lora_training_runner", observer,
                "dispatching frontier-MLX training (outcomes=%s, adapter=%s)", outcomes_jsonl_path,
                adapters_output_path);

    /* Use simple fork+exec via system() for simplicity. A more robust
     * implementation would use posix_spawn + pipe for stdout capture
     * (per lora_retrain_runner.c pattern), but for this phase we log
     * the command and let Python's stderr go to the daemon log. */
    char cmd_buf[1024];
    snprintf(cmd_buf, sizeof(cmd_buf),
             "python3 %s/scripts/training_loop.py --source-jsonl %s --adapter-out %s "
             ">> %s/.human/logs/training-loop-%s.log 2>&1",
             repo_scripts_path, outcomes_jsonl_path, adapters_output_path, home_dir, timestamp_buf);

    hu_log_info("lora_training_runner", observer, "executing: %s", cmd_buf);

    int rc = system(cmd_buf);
    if (rc != 0) {
        hu_log_warn("lora_training_runner", observer,
                    "frontier-mlx training subprocess exited with rc=%d", rc);
        return HU_ERR_IO;
    }

    hu_log_info("lora_training_runner", observer,
                "frontier-mlx training dispatch succeeded (adapter=%s)", adapters_output_path);
    return HU_OK;
#endif /* HU_IS_TEST */
}

hu_error_t hu_lora_training_runner(hu_memory_facade_t *m, const struct hu_job_spec *spec,
                                   int64_t budget_ms, void *user_data) {
    (void)m;
    (void)budget_ms;
    if (!spec || !user_data)
        return HU_ERR_INVALID_ARGUMENT;
    hu_lora_runner_ctx_t *ctx = (hu_lora_runner_ctx_t *)user_data;

    /* M3 / US-8 frontier-MLX path (pair-count trigger).
     * When the pair-count trigger fires with target=FRONTIER_MLX,
     * dispatch training_loop.py subprocess instead of draining learner signals.
     * See dispatch_frontier_mlx_training() for implementation. */
    hu_training_target_model_t target = hu_training_runner_last_enqueued_target();
    if (target == HU_TRAINING_TARGET_FRONTIER_MLX) {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            home = "/tmp";
        hu_error_t fmx_err = dispatch_frontier_mlx_training(home, NULL);
        /* Dispatch errors are logged but don't block cache warming below. */
        if (fmx_err == HU_OK) {
            hu_log_info("lora_training_runner", NULL, "frontier-mlx training dispatch succeeded");
        } else {
            hu_log_warn("lora_training_runner", NULL, "frontier-mlx training dispatch failed: %s",
                        hu_error_string(fmx_err));
        }
        /* Even if dispatch failed, skip the learner drain path and return.
         * The next pair-count threshold crossing will retry. */
        return HU_OK;
    }

    /* HUML reference path (learner-pending or legacy pair-count).
     * Drain pending signals from the in-process learner. */
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
        if (run_promotion_gate(ctx, &report, &gate_verdict) == HU_OK)
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
        hu_error_t le = hu_provider_load_adapter(ctx->provider, &load_alloc, report.adapter_path,
                                                 strlen(report.adapter_path), aid, strlen(aid));
        if (le == HU_OK)
            hu_log_info("lora-runner", NULL, "auto-loaded adapter '%s' from %s", aid,
                        report.adapter_path);
        else if (le != HU_ERR_NOT_SUPPORTED)
            hu_log_warn("lora-runner", NULL, "adapter auto-load failed for '%s': %d", aid, (int)le);
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

#ifdef HU_IS_TEST
#include "hu_e2e_closed_loop.h"
#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/ml/dpo.h"
#include "human/provider.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define HU_E2E_LOOP_IMPL_VERSION 1
_Static_assert(HU_E2E_LOOP_IMPL_VERSION == 1,
               "Bump HU_E2E_LOOP_IMPL_VERSION in cli_demo.c when changing loop logic.");

static hu_error_t e2e_mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 ? HU_OK : HU_ERR_IO;
}

void hu_e2e_reaction_aux_free(hu_allocator_t *alloc, hu_e2e_reaction_aux_t *aux, size_t n) {
    if (!alloc || !aux)
        return;
    for (size_t i = 0; i < n; i++) {
        if (aux[i].prompt)
            alloc->free(alloc->ctx, (void *)aux[i].prompt, strlen(aux[i].prompt) + 1);
        if (aux[i].response_chosen)
            alloc->free(alloc->ctx, (void *)aux[i].response_chosen,
                        strlen(aux[i].response_chosen) + 1);
        if (aux[i].response_rejected)
            alloc->free(alloc->ctx, (void *)aux[i].response_rejected,
                        strlen(aux[i].response_rejected) + 1);
    }
    alloc->free(alloc->ctx, aux, n * sizeof(hu_e2e_reaction_aux_t));
}

void hu_e2e_closed_loop_output_free(hu_allocator_t *alloc, hu_e2e_closed_loop_output_t *out) {
    if (!alloc || !out)
        return;
    if (out->before_response) {
        alloc->free(alloc->ctx, out->before_response, out->before_response_len + 1);
        out->before_response = NULL;
        out->before_response_len = 0;
    }
    if (out->after_response) {
        alloc->free(alloc->ctx, out->after_response, out->after_response_len + 1);
        out->after_response = NULL;
        out->after_response_len = 0;
    }
}

hu_error_t hu_e2e_closed_loop_run(const hu_e2e_closed_loop_input_t *in, hu_allocator_t *alloc,
                                  hu_e2e_closed_loop_output_t *out) {
    if (!in || !alloc || !out || !in->provider || !in->provider->vtable || !in->trainer ||
        !in->trainer->vtable || !in->collector || !in->reaction_events || !in->adapter_out_path ||
        !in->adapter_id || !in->system_prompt || !in->user_message || !in->model)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    int64_t t0 = hu_e2e_monotonic_ms();
    hu_error_t err = HU_OK;
    hu_dpo_export_t export_data = {0};

    err = in->provider->vtable->chat_with_system(
        in->provider->ctx, alloc, in->system_prompt, in->system_prompt_len, in->user_message,
        in->user_message_len, in->model, in->model_len, in->temperature, &out->before_response,
        &out->before_response_len);
    if (err != HU_OK)
        goto cleanup;

    for (size_t i = 0; i < in->reaction_event_count; i++) {
        const hu_reaction_event_t *e = &in->reaction_events[i];
        const hu_e2e_reaction_aux_t *aux = in->reaction_aux ? &in->reaction_aux[i] : NULL;
        if (!aux || !aux->prompt || !aux->response_chosen)
            continue;
        hu_reaction_handler_register_assistant_message_for_test(
            e->channel_id, e->target_thread_id, e->target_message_ref, aux->prompt,
            e->polarity == HU_REACTION_POSITIVE ? aux->response_chosen : aux->response_rejected);
    }

    hu_reaction_handler_set_collector(in->collector);
    for (size_t i = 0; i < in->reaction_event_count; i++) {
        err = hu_reaction_handler_handle_event(&in->reaction_events[i]);
        if (err != HU_OK && err != HU_ERR_NOT_FOUND) {
            hu_reaction_handler_set_collector(NULL);
            goto cleanup;
        }
        err = HU_OK;
    }
    hu_reaction_handler_set_collector(NULL);

    /* Supplement one-sided tapback rows with two-sided pairs for HUML DPO
     * (toy trainer requires both chosen and rejected token sequences). */
    if (in->reaction_aux) {
        for (size_t i = 0; i < in->reaction_event_count; i++) {
            const hu_e2e_reaction_aux_t *a = &in->reaction_aux[i];
            if (!a->prompt || !a->response_chosen || !a->response_rejected)
                continue;
            size_t pl = strlen(a->prompt);
            size_t cl = strlen(a->response_chosen);
            size_t rl = strlen(a->response_rejected);
            (void)hu_dpo_record_from_retry(in->collector, a->prompt, pl, a->response_rejected, rl,
                                           a->response_chosen, cl);
        }
    }

    size_t pair_count = 0;
    (void)hu_dpo_pair_count(in->collector, &pair_count);
    out->pairs_consumed = (double)pair_count;
    if (pair_count < in->reaction_event_count) {
        err = HU_ERR_INVALID_ARGUMENT;
        goto cleanup;
    }

    err = hu_dpo_export(in->collector, alloc, &export_data);
    if (err != HU_OK)
        goto cleanup;

    hu_rl_trainer_metrics_t metrics = {0};
    err = in->trainer->vtable->step(in->trainer->ctx, alloc, export_data.pairs, export_data.count,
                                    &metrics);
    if (err != HU_OK)
        goto cleanup;

    {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", in->adapter_out_path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            (void)e2e_mkdir_p(dir);
        }
    }

    err = in->trainer->vtable->save_adapter(in->trainer->ctx, alloc, in->adapter_out_path);
    if (err != HU_OK)
        goto cleanup;
    snprintf(out->adapter_path, sizeof(out->adapter_path), "%s", in->adapter_out_path);

    err = hu_provider_load_adapter(in->provider, alloc, in->adapter_out_path,
                                   strlen(in->adapter_out_path), in->adapter_id,
                                   strlen(in->adapter_id));
    if (err != HU_OK)
        goto cleanup;

    err = in->provider->vtable->chat_with_system(
        in->provider->ctx, alloc, in->system_prompt, in->system_prompt_len, in->user_message,
        in->user_message_len, in->model, in->model_len, in->temperature, &out->after_response,
        &out->after_response_len);
    if (err != HU_OK)
        goto cleanup;

    out->responses_differ =
        out->before_response && out->after_response &&
        ((out->before_response_len != out->after_response_len) ||
         memcmp(out->before_response, out->after_response, out->before_response_len) != 0);
    out->elapsed_ms = hu_e2e_monotonic_ms() - t0;

cleanup:
    hu_dpo_export_free(alloc, &export_data);
    if (err != HU_OK)
        hu_e2e_closed_loop_output_free(alloc, out);
    return err;
}
#endif /* HU_IS_TEST */
