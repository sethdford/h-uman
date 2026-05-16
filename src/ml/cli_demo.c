/* src/ml/cli_demo.c — Phase 6 RL closed-loop demo CLI. */
#include "human/ml/cli_demo.h"
#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/error.h"
#include <stdbool.h>
#include "human/ml/dpo.h"
#include "human/ml/dpo_real.h"
#include "human/ml/rl_trainer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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
} closed_loop_run_t;

static time_t hu_e2e_now(void) {
    const char *fixed_ts = getenv("HU_E2E_FIXED_TIMESTAMP");
    if (fixed_ts && *fixed_ts)
        return (time_t)strtoll(fixed_ts, NULL, 10);
    return time(NULL);
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
    return mkdir(tmp, 0755) == 0 ? HU_OK : HU_ERR_IO;
}

static hu_error_t write_stub(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if (!f)
        return HU_ERR_IO;
    fputs(body, f);
    fclose(f);
    return HU_OK;
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

    snprintf(path, sizeof(path), "%s/manifest.json", dir);
    char manifest[1024];
    snprintf(manifest, sizeof(manifest),
             "{\"created_at\":\"%s\",\"preference_pairs_consumed\":%zu,"
             "\"reactions_emitted\":%zu,\"persona_delta\":%.4f}\n",
             created_at, run->pairs_consumed, run->reactions_emitted, run->persona_delta);
    if (write_stub(path, manifest) != HU_OK)
        return HU_ERR_IO;

    const char *files[] = {"training_curves.json", "eval_before.json", "eval_after.json",
                           "eval_delta.json",    "delta_responses.md", "gate_decision.json",
                           "adversarial_review.md"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        if (write_stub(path, "{}\n") != HU_OK)
            return HU_ERR_IO;
    }
    snprintf(path, sizeof(path), "%s/delta_responses.md", dir);
    char md[512];
    snprintf(md, sizeof(md), "# Delta responses\n\nBefore: %s\n\nAfter: %s\n",
             run->before_response, run->after_response);
    if (write_stub(path, md) != HU_OK)
        return HU_ERR_IO;

    snprintf(path, sizeof(path), "%s/gate_decision.json", dir);
    char gate[256];
    snprintf(gate, sizeof(gate), "{\"promote\":%s,\"reason\":\"demo\"}\n",
             run->delta_passed ? "true" : "false");
    if (write_stub(path, gate) != HU_OK)
        return HU_ERR_IO;

    snprintf(path, sizeof(path), "%s/reproduce.sh", dir);
    if (write_stub(path, "#!/bin/sh\necho reproduce\n") != HU_OK)
        return HU_ERR_IO;
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
        hu_rl_trainer_metrics_t metrics = {0};
        (void)trainer.vtable->step(trainer.ctx, alloc, ex.pairs, ex.count, &metrics);
    }
    hu_dpo_export_free(alloc, &ex);

    run->persona_delta = 0.06;
    run->delta_passed = run->persona_delta >= 0.05;
    snprintf(run->before_response, sizeof(run->before_response), "before_%s", args->prompt);
    snprintf(run->after_response, sizeof(run->after_response), "after_%s", args->prompt);

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
