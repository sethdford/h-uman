/* src/ml/cli_demo.c — Phase 6 RL closed-loop demo CLI.
 *
 * CF-2 closure: the evidence bundle previously wrote 6 of 9 files as
 * empty `{}` stubs and hard-coded `persona_delta=0.06` plus
 * `gate_decision={"promote":true,"reason":"demo"}` in `manifest.json`.
 * Each file now contains real measured content derived from the
 * trainer's metrics + the eval gate's verdict.
 *
 * Honest scope: this is a *synthetic* closed-loop demo. The reactions
 * are generated deterministically (alternating LOVE/DISLIKE) and the
 * before/after persona scores are derived from the trainer's
 * `chosen_logprob_delta` rather than from a real preference scorer
 * applied to model outputs. The evidence files now state this
 * explicitly so a reviewer cannot mistake the demo's numbers for a
 * real-corpus measurement. To plug in real data, replace the
 * synthetic reaction loop with a real corpus and the synthetic
 * before/after derivation with a real scorer (see `reproduce.sh`).
 */
#include "human/ml/cli_demo.h"
#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/error.h"
#include "human/eval/bootstrap_ci.h"
#include "human/eval/eval_gate.h"
#include <stdbool.h>
#include "human/ml/dpo.h"
#include "human/ml/dpo_real.h"
#include "human/ml/rl_trainer.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define HU_CF2_MAX_SCORES 64

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#define HU_E2E_LOOP_IMPL_VERSION 1
_Static_assert(HU_E2E_LOOP_IMPL_VERSION == 1,
               "Bump HU_E2E_LOOP_IMPL_VERSION in lora_training_runner.c when changing.");

typedef struct demo_args {
    const char *persona;
    const char *method;
    const char *backend;
    int reaction_count;
    const char *prompt;
    const char *out_dir;
    bool require_positive_delta;
} demo_args_t;

typedef struct closed_loop_run {
    double persona_delta;
    bool delta_passed;
    size_t pairs_consumed;
    size_t reactions_emitted;
    char before_response[256];
    char after_response[256];
    /* CF-2: real measured fields populated by the closed-loop run.
     * Used by write_evidence_dir to emit non-stub JSON / markdown. */
    hu_rl_trainer_metrics_t trainer_metrics;
    char trainer_name[64];
    double before_scores[HU_CF2_MAX_SCORES];
    double after_scores[HU_CF2_MAX_SCORES];
    size_t score_n;
    double before_mean;
    double after_mean;
    double bootstrap_p_value;
    /* Gate verdict (only populated when score_n >= 10 so the
     * bootstrap CI floor in hu_eval_gate is satisfied). */
    bool gate_ran;
    bool gate_promote;
    char gate_reason[512];
    double gate_persona_ci_lower;
    double gate_persona_ci_upper;
    /* Demo args echoed back for reproduce.sh. */
    char repro_persona[64];
    char repro_method[16];
    char repro_backend[16];
    int repro_reaction_count;
    char repro_prompt[256];
} closed_loop_run_t;

static time_t hu_e2e_now(void) {
    const char *fixed_ts = getenv("HU_E2E_FIXED_TIMESTAMP");
    if (fixed_ts && *fixed_ts)
        return (time_t)strtoll(fixed_ts, NULL, 10);
    return time(NULL);
}

/* Recursive mkdir, idempotent: treats EEXIST as success at every
 * level so re-running `human demo rl-closed-loop --out <same_dir>`
 * is well-defined. Returns HU_ERR_IO only when the final path is
 * not a directory we can write to. */
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
    if (mkdir(tmp, 0755) == 0)
        return HU_OK;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode))
            return HU_OK;
    }
    return HU_ERR_IO;
}

static hu_error_t write_stub(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if (!f)
        return HU_ERR_IO;
    fputs(body, f);
    fclose(f);
    return HU_OK;
}

/* Format a per-conversation score array as a JSON document with
 * mean + n + the full array. Returns number of bytes written into
 * buf (excluding NUL), or -1 on truncation. */
static int format_scores_json(char *buf, size_t cap, const char *kind, double mean,
                              const double *scores, size_t n) {
    int off = 0;
    int w = snprintf(buf + off, cap - (size_t)off,
                     "{\"kind\":\"%s\",\"n\":%zu,\"mean\":%.6f,\"scores\":[",
                     kind, n, mean);
    if (w < 0 || (size_t)w >= cap - (size_t)off) return -1;
    off += w;
    for (size_t i = 0; i < n; i++) {
        w = snprintf(buf + off, cap - (size_t)off, "%s%.6f", i ? "," : "", scores[i]);
        if (w < 0 || (size_t)w >= cap - (size_t)off) return -1;
        off += w;
    }
    w = snprintf(buf + off, cap - (size_t)off, "]}\n");
    if (w < 0 || (size_t)w >= cap - (size_t)off) return -1;
    off += w;
    return off;
}

static hu_error_t write_evidence_dir(const char *dir, const closed_loop_run_t *run) {
    if (!dir || !run)
        return HU_ERR_INVALID_ARGUMENT;
    if (mkdir_p(dir) != HU_OK)
        return HU_ERR_IO;

    char path[768];
    char created_at[32];
    time_t t = hu_e2e_now();
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(created_at, sizeof(created_at), "%Y-%m-%dT%H:%M:%SZ", &tm);

    /* manifest.json: real persona_delta from before/after means (no
     * longer a hard-coded 0.06 literal). */
    snprintf(path, sizeof(path), "%s/manifest.json", dir);
    char manifest[2048];
    snprintf(manifest, sizeof(manifest),
             "{\"created_at\":\"%s\","
             "\"preference_pairs_consumed\":%zu,"
             "\"reactions_emitted\":%zu,"
             "\"persona_delta\":%.6f,"
             "\"persona_delta_source\":\"after_mean - before_mean\","
             "\"trainer\":\"%s\","
             "\"trainer_final_loss\":%.6f,"
             "\"trainer_iters\":%zu,"
             "\"synthetic\":true,"
             "\"synthetic_note\":\"reactions and scores are deterministic; "
             "see reproduce.sh and adversarial_review.md\"}\n",
             created_at, run->pairs_consumed, run->reactions_emitted,
             run->persona_delta, run->trainer_name,
             run->trainer_metrics.final_loss, run->trainer_metrics.iters_completed);
    if (write_stub(path, manifest) != HU_OK) return HU_ERR_IO;

    /* training_curves.json: real trainer metrics (no longer `{}`). */
    snprintf(path, sizeof(path), "%s/training_curves.json", dir);
    char curves[1024];
    snprintf(curves, sizeof(curves),
             "{\"trainer\":\"%s\","
             "\"iters_completed\":%zu,"
             "\"final_loss\":%.6f,"
             "\"chosen_logprob_delta\":%.6f,"
             "\"rejected_logprob_delta\":%.6f,"
             "\"adapter_path\":\"%s\"}\n",
             run->trainer_name, run->trainer_metrics.iters_completed,
             run->trainer_metrics.final_loss,
             run->trainer_metrics.chosen_logprob_delta,
             run->trainer_metrics.rejected_logprob_delta,
             run->trainer_metrics.adapter_path);
    if (write_stub(path, curves) != HU_OK) return HU_ERR_IO;

    /* eval_before.json / eval_after.json: real per-conversation
     * score arrays (no longer `{}`). */
    char scores_buf[4096];
    snprintf(path, sizeof(path), "%s/eval_before.json", dir);
    if (format_scores_json(scores_buf, sizeof(scores_buf), "persona_fidelity_before",
                           run->before_mean, run->before_scores, run->score_n) < 0)
        return HU_ERR_IO;
    if (write_stub(path, scores_buf) != HU_OK) return HU_ERR_IO;

    snprintf(path, sizeof(path), "%s/eval_after.json", dir);
    if (format_scores_json(scores_buf, sizeof(scores_buf), "persona_fidelity_after",
                           run->after_mean, run->after_scores, run->score_n) < 0)
        return HU_ERR_IO;
    if (write_stub(path, scores_buf) != HU_OK) return HU_ERR_IO;

    /* eval_delta.json: real bootstrap-derived delta + p-value (no
     * longer `{}`). */
    snprintf(path, sizeof(path), "%s/eval_delta.json", dir);
    char delta[1024];
    snprintf(delta, sizeof(delta),
             "{\"before_mean\":%.6f,"
             "\"after_mean\":%.6f,"
             "\"delta_mean\":%.6f,"
             "\"bootstrap_p_value\":%.6f,"
             "\"n_before\":%zu,"
             "\"n_after\":%zu}\n",
             run->before_mean, run->after_mean,
             run->after_mean - run->before_mean,
             run->bootstrap_p_value, run->score_n, run->score_n);
    if (write_stub(path, delta) != HU_OK) return HU_ERR_IO;

    /* delta_responses.md (already mostly real — preserved). */
    snprintf(path, sizeof(path), "%s/delta_responses.md", dir);
    char md[1024];
    snprintf(md, sizeof(md),
             "# Delta responses\n\n"
             "Before adapter: %s\n\n"
             "After adapter:  %s\n\n"
             "Persona-fidelity delta: %.4f (after %.4f - before %.4f, bootstrap p=%.4f)\n",
             run->before_response, run->after_response,
             run->after_mean - run->before_mean,
             run->after_mean, run->before_mean, run->bootstrap_p_value);
    if (write_stub(path, md) != HU_OK) return HU_ERR_IO;

    /* gate_decision.json: real verdict from hu_eval_gate (or an
     * honest "not run" marker when the bootstrap floor n>=10 isn't
     * satisfied). No longer a hard-coded `{"promote":true,"reason":"demo"}`. */
    snprintf(path, sizeof(path), "%s/gate_decision.json", dir);
    char gate[1024];
    if (run->gate_ran) {
        snprintf(gate, sizeof(gate),
                 "{\"promote\":%s,"
                 "\"persona_ci_lower\":%.6f,"
                 "\"persona_ci_upper\":%.6f,"
                 "\"reason\":\"%s\","
                 "\"source\":\"hu_eval_gate_decide_from_arrays_for_test\"}\n",
                 run->gate_promote ? "true" : "false",
                 run->gate_persona_ci_lower, run->gate_persona_ci_upper,
                 run->gate_reason[0] ? run->gate_reason : "(all checks passed)");
    } else {
        snprintf(gate, sizeof(gate),
                 "{\"promote\":%s,"
                 "\"reason\":\"gate not run: score_n %zu below bootstrap floor 10\","
                 "\"fallback_check\":\"after_mean - before_mean >= 0.05\"}\n",
                 run->delta_passed ? "true" : "false", run->score_n);
    }
    if (write_stub(path, gate) != HU_OK) return HU_ERR_IO;

    /* adversarial_review.md: structured automated review covering
     * what the demo actually checked. No longer `{}`. */
    snprintf(path, sizeof(path), "%s/adversarial_review.md", dir);
    char review[2048];
    snprintf(review, sizeof(review),
             "# Automated adversarial review\n\n"
             "_This is an offline automated review. No external LLM critic was run.\n"
             "It enumerates the deterministic checks the closed-loop demo passes\n"
             "(or honestly reports any that did not)._\n\n"
             "## Reactions\n"
             "- emitted: %zu\n"
             "- preference pairs consumed by trainer: %zu\n"
             "- balanced positive/negative (alternating LOVE/DISLIKE): yes\n\n"
             "## Trainer (%s)\n"
             "- iterations completed: %zu\n"
             "- final loss: %.6f\n"
             "- chosen-logprob delta: %.6f\n"
             "- rejected-logprob delta: %.6f\n"
             "- adapter path: %s\n\n"
             "## Eval gate\n"
             "- score sample size (n): %zu\n"
             "- before mean: %.6f\n"
             "- after mean:  %.6f\n"
             "- bootstrap p-value (two-sided, pooled): %.6f\n"
             "- gate ran: %s\n"
             "- gate promote: %s\n"
             "- gate reason: %s\n\n"
             "## Caveats (honest)\n"
             "- This demo's reactions are synthetic (alternating LOVE/DISLIKE on a\n"
             "  single prompt). Replace with a real corpus to make the numbers\n"
             "  load-bearing.\n"
             "- The after-adapter persona scores are derived from\n"
             "  `tanh(chosen_logprob_delta)`, not from a real preference scorer\n"
             "  applied to model outputs. The trainer metrics themselves ARE real.\n"
             "- See `reproduce.sh` for the exact command to re-run.\n",
             run->reactions_emitted, run->pairs_consumed, run->trainer_name,
             run->trainer_metrics.iters_completed, run->trainer_metrics.final_loss,
             run->trainer_metrics.chosen_logprob_delta,
             run->trainer_metrics.rejected_logprob_delta,
             run->trainer_metrics.adapter_path,
             run->score_n, run->before_mean, run->after_mean,
             run->bootstrap_p_value,
             run->gate_ran ? "yes" : "no",
             run->gate_ran ? (run->gate_promote ? "true" : "false") : "n/a",
             run->gate_reason[0] ? run->gate_reason : "(none)");
    if (write_stub(path, review) != HU_OK) return HU_ERR_IO;

    /* reproduce.sh: real shell snippet that re-runs the demo with
     * the same args. No longer `echo reproduce`. */
    snprintf(path, sizeof(path), "%s/reproduce.sh", dir);
    char repro[1024];
    snprintf(repro, sizeof(repro),
             "#!/bin/sh\n"
             "# Reproduce this RL closed-loop evidence bundle byte-for-byte.\n"
             "# Requires a build with HU_ENABLE_RL_FULL=ON (e.g. cmake preset rl_sota).\n"
             "set -e\n"
             "export HU_E2E_FIXED_TIMESTAMP=%ld\n"
             "./build-rl-sota/human demo rl-closed-loop \\\n"
             "    --persona %s \\\n"
             "    --method %s \\\n"
             "    --backend %s \\\n"
             "    --reaction-count %d \\\n"
             "    --prompt \"%s\" \\\n"
             "    --out \"$1\"\n",
             (long)t, run->repro_persona, run->repro_method, run->repro_backend,
             run->repro_reaction_count, run->repro_prompt);
    if (write_stub(path, repro) != HU_OK) return HU_ERR_IO;
    return HU_OK;
}

static hu_error_t parse_args(int argc, const char **argv, demo_args_t *out) {
    memset(out, 0, sizeof(*out));
    out->persona = "demo_persona_e2e";
    out->method = "dpo";
    out->backend = "huml";
    out->reaction_count = 50;
    out->prompt = "what should i do first?";
    out->out_dir = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--persona") == 0 && i + 1 < argc)
            out->persona = argv[++i];
        else if (strcmp(argv[i], "--method") == 0 && i + 1 < argc)
            out->method = argv[++i];
        else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
            out->backend = argv[++i];
        else if (strcmp(argv[i], "--reaction-count") == 0 && i + 1 < argc)
            out->reaction_count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--prompt") == 0 && i + 1 < argc)
            out->prompt = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out->out_dir = argv[++i];
        else if (strcmp(argv[i], "--require-positive-delta") == 0)
            out->require_positive_delta = true;
    }
    (void)out->method;
    (void)out->persona;
    return HU_OK;
}

static hu_error_t cli_demo_run_closed_loop(hu_allocator_t *alloc, const demo_args_t *args,
                                           closed_loop_run_t *run) {
    if (!alloc || !args || !run)
        return HU_ERR_INVALID_ARGUMENT;
    memset(run, 0, sizeof(*run));

#ifdef HU_ENABLE_SQLITE
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK)
        return HU_ERR_IO;
    hu_dpo_collector_t collector = {0};
    if (hu_dpo_collector_create(alloc, db, 256, &collector) != HU_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }
    if (hu_dpo_init_tables(&collector) != HU_OK) {
        hu_dpo_collector_deinit(&collector);
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    hu_rl_trainer_config_t tcfg = {
        .backend = strcmp(args->backend, "mlx") == 0 ? HU_DPO_BACKEND_MLX : HU_DPO_BACKEND_HUML,
        .beta = 0.1,
        .learning_rate = 0.1,
    };
    hu_rl_trainer_t trainer = {0};
    hu_error_t err = hu_rl_trainer_create_dpo(alloc, &tcfg, &trainer);
    if (err != HU_OK) {
        hu_dpo_collector_deinit(&collector);
        sqlite3_close(db);
        return err;
    }

    for (int i = 0; i < args->reaction_count; i++) {
        char thread[64], msg[64];
        snprintf(thread, sizeof(thread), "chat-synth-%03d", i + 1);
        snprintf(msg, sizeof(msg), "msg-synth-%03d", i + 1);
        const char *resp = (i % 2 == 0) ? "ship the small fix." : "perhaps consider exploring.";
        hu_reaction_handler_register_assistant_message_for_production("imessage", thread, msg,
                                                                      args->prompt, resp);

        hu_reaction_event_t evt = {
            .channel_id = "imessage",
            .target_thread_id = thread,
            .target_message_ref = msg,
            .sender_handle = "+15550100001",
            .kind = (i % 2 == 0) ? HU_REACTION_LOVE : HU_REACTION_DISLIKE,
            .polarity = (i % 2 == 0) ? HU_REACTION_POSITIVE : HU_REACTION_NEGATIVE,
            .timestamp_unix = (int64_t)hu_e2e_now() + i,
            .is_removal = 0,
        };
        hu_reaction_handler_set_collector(&collector);
        (void)hu_reaction_handler_handle_event(&evt);
        hu_reaction_handler_set_collector(NULL);
    }

    size_t pair_count = 0;
    (void)hu_dpo_pair_count(&collector, &pair_count);
    run->pairs_consumed = pair_count;
    run->reactions_emitted = (size_t)args->reaction_count;

    hu_dpo_export_t ex = {0};
    err = hu_dpo_export(&collector, alloc, &ex);
    if (err == HU_OK && ex.count > 0) {
        (void)trainer.vtable->step(trainer.ctx, alloc, ex.pairs, ex.count, &run->trainer_metrics);
    }
    hu_dpo_export_free(alloc, &ex);

    /* CF-2: capture real trainer name + before/after persona scores
     * derived from the trainer's chosen_logprob_delta. The synthetic
     * derivation is documented in adversarial_review.md so a reviewer
     * never mistakes it for a real-corpus measurement. */
    const char *tname = (trainer.vtable && trainer.vtable->name)
                        ? trainer.vtable->name(trainer.ctx) : "dpo";
    snprintf(run->trainer_name, sizeof(run->trainer_name), "%s",
             tname ? tname : "dpo");

    size_t score_n = (size_t)args->reaction_count;
    if (score_n > HU_CF2_MAX_SCORES) score_n = HU_CF2_MAX_SCORES;
    run->score_n = score_n;

    /* Synthetic before-scores: deterministic noise around 0.50 (unaligned
     * baseline). After-scores: baseline + tanh(chosen_logprob_delta) * 0.25
     * (the trainer's actual policy delta drives the lift; capped at +/-0.25). */
    double lift = tanh(run->trainer_metrics.chosen_logprob_delta) * 0.25;
    double before_sum = 0.0, after_sum = 0.0;
    for (size_t i = 0; i < score_n; i++) {
        double noise = 0.05 * sin((double)i * 1.7);
        run->before_scores[i] = 0.50 + noise;
        run->after_scores[i] = 0.50 + noise + lift;
        before_sum += run->before_scores[i];
        after_sum += run->after_scores[i];
    }
    run->before_mean = score_n > 0 ? before_sum / (double)score_n : 0.0;
    run->after_mean = score_n > 0 ? after_sum / (double)score_n : 0.0;
    run->persona_delta = run->after_mean - run->before_mean;
    run->delta_passed = run->persona_delta >= 0.05;

    /* Real bootstrap two-sample test on before vs after. Requires
     * n_a, n_b >= 2 per hu_bootstrap_compare_means. */
    if (score_n >= 2) {
        double ma = 0.0, mb = 0.0;
        if (hu_bootstrap_compare_means(run->before_scores, score_n,
                                       run->after_scores, score_n,
                                       1000, 42, &ma, &mb,
                                       &run->bootstrap_p_value) != HU_OK) {
            run->bootstrap_p_value = -1.0;
        }
    } else {
        run->bootstrap_p_value = -1.0;
    }

    /* Real eval-gate verdict when n satisfies the bootstrap floor. */
    if (score_n >= 10) {
        hu_eval_gate_t gate = {
            .baseline_persona_fidelity_mean = run->before_mean,
            .baseline_p95_latency_ms = 100.0,
            .persona_delta_min = 0.05,
            .latency_delta_max_ms = 50.0,
            .bootstrap_samples = 500,
            .bootstrap_seed = 42,
        };
        hu_eval_gate_verdict_t verdict = {0};
        if (hu_eval_gate_decide_from_arrays_for_test(&gate, run->after_scores, NULL, NULL,
                                                     NULL, score_n, 100.0, &verdict) == HU_OK) {
            run->gate_ran = true;
            run->gate_promote = verdict.promote;
            run->gate_persona_ci_lower = verdict.persona_ci_lower;
            run->gate_persona_ci_upper = verdict.persona_ci_upper;
            snprintf(run->gate_reason, sizeof(run->gate_reason), "%s",
                     verdict.reason);
        }
    }

    snprintf(run->before_response, sizeof(run->before_response), "before_%s", args->prompt);
    snprintf(run->after_response, sizeof(run->after_response), "after_%s", args->prompt);

    /* Echo args back for reproduce.sh. */
    snprintf(run->repro_persona, sizeof(run->repro_persona), "%s", args->persona);
    snprintf(run->repro_method, sizeof(run->repro_method), "%s", args->method);
    snprintf(run->repro_backend, sizeof(run->repro_backend), "%s", args->backend);
    run->repro_reaction_count = args->reaction_count;
    snprintf(run->repro_prompt, sizeof(run->repro_prompt), "%s", args->prompt);

    if (trainer.vtable && trainer.vtable->deinit)
        trainer.vtable->deinit(trainer.ctx, alloc);
    hu_dpo_collector_deinit(&collector);
    sqlite3_close(db);
    return HU_OK;
#else
    (void)args;
    (void)run;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_ml_cli_demo_rl_closed_loop(int argc, const char **argv, hu_allocator_t *alloc) {
    demo_args_t args;
    if (parse_args(argc, argv, &args) != HU_OK)
        return HU_ERR_INVALID_ARGUMENT;

    closed_loop_run_t run;
    hu_error_t err = cli_demo_run_closed_loop(alloc, &args, &run);
    if (err != HU_OK)
        return HU_ERR_PROVIDER_RESPONSE;

    char out_dir[512];
    if (args.out_dir) {
        snprintf(out_dir, sizeof(out_dir), "%s", args.out_dir);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            home = "/tmp";
        snprintf(out_dir, sizeof(out_dir), "%s/.human/proofs/demo-%ld", home, (long)hu_e2e_now());
    }
    if (write_evidence_dir(out_dir, &run) != HU_OK)
        return HU_ERR_IO;

    if (args.require_positive_delta && !run.delta_passed)
        return HU_ERR_PERMISSION_DENIED;
    return HU_OK;
}
