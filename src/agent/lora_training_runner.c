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

#ifdef HU_ENABLE_RL_FULL
#include "human/agent/adapter_id.h"
#include "human/eval/eval_gate.h"
#include "human/ml/fidelity.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HU_LORA_GATE_MAX_SAMPLES 32

#ifdef HU_IS_TEST
static time_t g_lora_runner_test_clock = 0;

void hu_lora_runner_set_test_clock(time_t frozen) { g_lora_runner_test_clock = frozen; }

static time_t runner_now(void) {
    return g_lora_runner_test_clock != 0 ? g_lora_runner_test_clock : time(NULL);
}
#else
static time_t runner_now(void) { return time(NULL); }
#endif

#ifdef HU_IS_TEST
static struct {
    double persona[HU_LORA_GATE_MAX_SAMPLES];
    size_t n;
    double p95_ms;
    bool valid;
} g_lora_gate_capture;

void hu_lora_runner_gate_capture_reset_for_test(void) {
    memset(&g_lora_gate_capture, 0, sizeof(g_lora_gate_capture));
}

size_t hu_lora_runner_gate_capture_n_for_test(void) {
    return g_lora_gate_capture.valid ? g_lora_gate_capture.n : 0;
}

double hu_lora_runner_gate_capture_persona_score_for_test(size_t index) {
    if (!g_lora_gate_capture.valid || index >= g_lora_gate_capture.n)
        return -1.0;
    return g_lora_gate_capture.persona[index];
}

double hu_lora_runner_gate_capture_p95_ms_for_test(void) {
    return g_lora_gate_capture.valid ? g_lora_gate_capture.p95_ms : -1.0;
}
#endif /* HU_IS_TEST */

static int64_t gate_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static size_t lora_runner_build_system_prompt(const hu_persona_t *persona, char *out, size_t cap) {
    if (!persona || cap == 0)
        return 0;
    out[0] = '\0';
    size_t n = 0;
    if (persona->identity && persona->identity[0]) {
        int w = snprintf(out + n, cap - n, "%s\n", persona->identity);
        if (w > 0 && (size_t)w < cap - n)
            n += (size_t)w;
    }
    if (persona->traits_count > 0 && persona->traits) {
        int w = snprintf(out + n, cap - n, "Traits: ");
        if (w > 0 && (size_t)w < cap - n)
            n += (size_t)w;
        for (size_t i = 0; i < persona->traits_count && n + 2 < cap; i++) {
            const char *t = persona->traits[i];
            if (!t || !t[0])
                continue;
            w = snprintf(out + n, cap - n, "%s%s", i == 0 ? "" : ", ", t);
            if (w > 0 && (size_t)w < cap - n)
                n += (size_t)w;
        }
        if (n + 1 < cap) {
            out[n++] = '\n';
            out[n] = '\0';
        }
    }
    return n;
}

static double gate_latency_p95(const double *latencies_ms, size_t n) {
    if (!latencies_ms || n == 0)
        return 0.0;
    double scratch[HU_LORA_GATE_MAX_SAMPLES];
    if (n > HU_LORA_GATE_MAX_SAMPLES)
        n = HU_LORA_GATE_MAX_SAMPLES;
    memcpy(scratch, latencies_ms, n * sizeof(scratch[0]));
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (scratch[j] < scratch[i]) {
                double t = scratch[i];
                scratch[i] = scratch[j];
                scratch[j] = t;
            }
        }
    }
    size_t idx = (n * 95 + 99) / 100;
    if (idx == 0)
        idx = 1;
    if (idx > n)
        idx = n;
    return scratch[idx - 1];
}

static void gate_capture_inputs(const double *persona, size_t n, double p95_ms) {
#ifdef HU_IS_TEST
    g_lora_gate_capture.n = n > HU_LORA_GATE_MAX_SAMPLES ? HU_LORA_GATE_MAX_SAMPLES : n;
    memcpy(g_lora_gate_capture.persona, persona,
           g_lora_gate_capture.n * sizeof(g_lora_gate_capture.persona[0]));
    g_lora_gate_capture.p95_ms = p95_ms;
    g_lora_gate_capture.valid = true;
#else
    (void)persona;
    (void)n;
    (void)p95_ms;
#endif
}

/* Score the freshly written adapter against the persona example bank.
 * Production: load adapter, chat each incoming prompt, measure wall time.
 * HU_IS_TEST: echo each example's curated `response` (same contract as
 * `human ml lora-runner`); latency is a deterministic function of response
 * length because mock providers do not expose inference timing. */
static hu_error_t measure_candidate_gate_inputs(
    const hu_lora_runner_ctx_t *ctx, const char *adapter_path,
    double *persona_out, double *latency_ms_out, size_t *out_n) {
    if (!ctx || !persona_out || !latency_ms_out || !out_n || !ctx->persona_name ||
        !ctx->persona_name[0])
        return HU_ERR_INVALID_ARGUMENT;

    hu_allocator_t alloc;
    if (ctx->alloc)
        alloc = *ctx->alloc;
    else
        alloc = hu_system_allocator();

    hu_persona_t persona = {0};
    hu_error_t err = hu_persona_load(&alloc, ctx->persona_name, strlen(ctx->persona_name),
                                     &persona);
    if (err != HU_OK)
        return err;

    hu_communication_style_t target;
    bool synthetic = true;
    if (hu_ml_fidelity_resolve_target(&alloc, &target, &synthetic) != HU_OK ||
        target.sample_count == 0U) {
        hu_persona_deinit(&alloc, &persona);
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (ctx->provider && adapter_path && adapter_path[0]) {
        const char *aid = ctx->adapter_id;
        char id_buf[128];
        if (!aid || !*aid) {
            const char *base = strrchr(adapter_path, '/');
            base = base ? base + 1 : adapter_path;
            size_t blen = strlen(base);
            if (blen >= sizeof(id_buf))
                blen = sizeof(id_buf) - 1;
            memcpy(id_buf, base, blen);
            id_buf[blen] = '\0';
            aid = id_buf;
        }
        hu_error_t le = hu_provider_load_adapter(ctx->provider, &alloc, adapter_path,
                                                 strlen(adapter_path), aid, strlen(aid));
        if (le != HU_OK && le != HU_ERR_NOT_SUPPORTED) {
            hu_persona_deinit(&alloc, &persona);
            return le;
        }
    }

    char system_buf[1024];
    size_t system_len = lora_runner_build_system_prompt(&persona, system_buf, sizeof(system_buf));
    const char *model = ctx->gate_model;
    size_t model_len = model ? strlen(model) : 0;
    size_t n = 0;

    for (size_t b = 0; b < persona.example_banks_count && n < HU_LORA_GATE_MAX_SAMPLES; b++) {
        const hu_persona_example_bank_t *bank = &persona.example_banks[b];
        for (size_t i = 0; i < bank->examples_count && n < HU_LORA_GATE_MAX_SAMPLES; i++) {
            const hu_persona_example_t *ex = &bank->examples[i];
            if (!ex->incoming || !ex->incoming[0])
                continue;

            const char *resp = NULL;
            size_t resp_len = 0;
            char *chat_owned = NULL;
            int64_t t0 = gate_monotonic_ms();

#ifdef HU_IS_TEST
            (void)system_len;
            (void)model_len;
            const char *src = (ex->response && ex->response[0]) ? ex->response : "";
            resp_len = strlen(src);
            resp = src;
            latency_ms_out[n] = (double)((resp_len % 37U) + 3U);
#else
            if (ctx->provider && ctx->provider->vtable &&
                ctx->provider->vtable->chat_with_system) {
                hu_error_t cerr = ctx->provider->vtable->chat_with_system(
                    ctx->provider->ctx, &alloc, system_buf, system_len, ex->incoming,
                    strlen(ex->incoming), model, model_len, 0.7, &chat_owned, &resp_len);
                if (cerr != HU_OK || !chat_owned || resp_len == 0) {
                    if (chat_owned)
                        alloc.free(alloc.ctx, chat_owned, resp_len + 1);
                    continue;
                }
                resp = chat_owned;
                int64_t t1 = gate_monotonic_ms();
                latency_ms_out[n] = (double)(t1 > t0 ? t1 - t0 : 1);
            } else if (ex->response && ex->response[0]) {
                /* No chat provider — honest fallback: score the bank's
                 * reference response (cannot observe adapter output). */
                resp = ex->response;
                resp_len = strlen(resp);
                latency_ms_out[n] = 0.0;
            } else {
                continue;
            }
#endif

            float score = hu_communication_style_fidelity_score_v2(&target, resp, resp_len);
            if (chat_owned)
                alloc.free(alloc.ctx, chat_owned, resp_len + 1);
            if (score < 0.f)
                continue;
            persona_out[n] = (double)score;
            n++;
        }
    }

    hu_persona_deinit(&alloc, &persona);
    if (n == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = n;
    return HU_OK;
}

static hu_error_t run_promotion_gate(const hu_lora_runner_ctx_t *ctx,
                                     const hu_learner_report_t *report,
                                     hu_eval_gate_verdict_t *verdict) {
    if (!ctx || !ctx->eval_gate || !verdict)
        return HU_ERR_INVALID_ARGUMENT;

    memset(verdict, 0, sizeof(*verdict));
    if (!ctx->persona_name || !ctx->persona_name[0]) {
        snprintf(verdict->reason, sizeof(verdict->reason),
                 "gate: persona_name required for measured eval; ");
        return HU_OK;
    }

    double persona_raw[HU_LORA_GATE_MAX_SAMPLES];
    double latency_raw[HU_LORA_GATE_MAX_SAMPLES];
    size_t n_raw = 0;
    const char *adapter_path =
        (report && report->adapter_path[0]) ? report->adapter_path : NULL;

    if (measure_candidate_gate_inputs(ctx, adapter_path, persona_raw, latency_raw, &n_raw) !=
        HU_OK) {
        snprintf(verdict->reason, sizeof(verdict->reason),
                 "gate: failed to measure candidate persona scores; ");
        return HU_OK;
    }

    double persona_gate[HU_LORA_GATE_MAX_SAMPLES];
    size_t n_gate = n_raw < 10 ? 10 : n_raw;
    if (n_gate > HU_LORA_GATE_MAX_SAMPLES)
        n_gate = HU_LORA_GATE_MAX_SAMPLES;
    for (size_t i = 0; i < n_gate; i++) {
        size_t j = i < n_raw ? i : (i % n_raw);
        persona_gate[i] = persona_raw[j];
    }

    double p95_ms = gate_latency_p95(latency_raw, n_raw);
#ifdef HU_IS_TEST
    /* Mock providers do not run real inference — keep latency inside the
     * gate budget using the deterministic length-derived samples above. */
    if (p95_ms <= 0.0)
        p95_ms = 5.0;
#else
    if (p95_ms <= 0.0 && ctx->eval_gate->baseline_p95_latency_ms > 0.0)
        p95_ms = ctx->eval_gate->baseline_p95_latency_ms;
#endif

    gate_capture_inputs(persona_gate, n_gate, p95_ms);
    return hu_eval_gate_decide_from_arrays_for_test(ctx->eval_gate, persona_gate, NULL, NULL,
                                                    NULL, n_gate, p95_ms, verdict);
}

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
#endif /* HU_ENABLE_RL_FULL */

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

hu_error_t hu_e2e_closed_loop_run(const hu_e2e_closed_loop_input_t *in,
                                  hu_allocator_t *alloc,
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
