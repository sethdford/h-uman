/* Phase 0 Task 3 — proves that hu_ml_train with vocab_size=0 + token_bytes=NULL
 * (the pre-fix call shape used in src/ml/cli.c:190 and src/ml/cli.c:2016)
 * silently no-ops: training reports OK and num_steps>0 but no per-token grad
 * is ever computed and val_bpb is never recorded. The patched cli paths must
 * pass a non-zero vocab_size and a non-NULL token_bytes table so the CE
 * objective actually runs and BPB is reported.
 *
 * See spec §1.5.2 issues #1, #2 and the May 11 2026 audit baseline. */

#include "test_framework.h"
#include "human/core/allocator.h"
#include "human/ml/dataloader.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include "human/ml/train.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HU_ENABLE_ML

static void mkdir_p_local(const char *path) {
#ifndef _WIN32
    mkdir(path, 0755);
#endif
}

static void write_bin_file_local(const char *path, const int32_t *tokens, size_t count) {
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(tokens, sizeof(int32_t), count, f);
        fclose(f);
    }
}

/* Build a tiny GPT + MuonAdamW + 2-shard dataloader pair backed by /tmp.
 * Mirrors the test_train_pipeline pattern at tests/test_ml.c:583-653 so the
 * same disk layout invariants apply. */
typedef struct pipeline {
    hu_model_t model;
    hu_ml_optimizer_t opt;
    hu_ml_dataloader_t *train_dl;
    hu_ml_dataloader_t *val_dl;
    char dir[256];
    char path1[320];
    char path2[320];
} pipeline_t;

static void build_pipeline(hu_allocator_t *alloc, const char *dirname, pipeline_t *p) {
    snprintf(p->dir, sizeof(p->dir), "/tmp/%s", dirname);
    mkdir_p_local(p->dir);

    int32_t tokens[200];
    for (int i = 0; i < 200; i++)
        tokens[i] = i % 128;
    snprintf(p->path1, sizeof(p->path1), "%s/shard_00000.bin", p->dir);
    snprintf(p->path2, sizeof(p->path2), "%s/shard_00001.bin", p->dir);
    write_bin_file_local(p->path1, tokens, 200);
    write_bin_file_local(p->path2, tokens, 200);

    HU_ASSERT_EQ(hu_ml_dataloader_create(alloc, p->dir, 2, 8, "train", &p->train_dl), HU_OK);
    HU_ASSERT_EQ(hu_ml_dataloader_create(alloc, p->dir, 2, 8, "val", &p->val_dl), HU_OK);

    hu_gpt_config_t gpt_cfg = {0};
    gpt_cfg.sequence_len = 16;
    gpt_cfg.vocab_size = 128;
    gpt_cfg.n_layer = 1;
    gpt_cfg.n_head = 2;
    gpt_cfg.n_kv_head = 2;
    gpt_cfg.n_embd = 64;
    gpt_cfg.head_dim = 32;
    gpt_cfg.activation = HU_ML_ACT_RELU_SQ;

    memset(&p->model, 0, sizeof(p->model));
    HU_ASSERT_EQ(hu_gpt_create(alloc, &gpt_cfg, &p->model), HU_OK);

    hu_optimizer_config_t opt_cfg = hu_experiment_config_default().optimizer;
    memset(&p->opt, 0, sizeof(p->opt));
    HU_ASSERT_EQ(hu_muon_adamw_create(alloc, &opt_cfg, &p->opt), HU_OK);
    HU_ASSERT_EQ(hu_gpt_register_params(&p->model, &p->opt), HU_OK);
}

static void cleanup_pipeline(hu_allocator_t *alloc, pipeline_t *p) {
    p->opt.vtable->deinit(p->opt.ctx, alloc);
    p->model.vtable->deinit(p->model.ctx, alloc);
    hu_ml_dataloader_deinit(p->val_dl);
    hu_ml_dataloader_deinit(p->train_dl);
    remove(p->path1);
    remove(p->path2);
    rmdir(p->dir);
}

/* The bug, as actually observed (May 11 2026 — the audit baseline said "silent
 * no-op" but the real behavior is hard-fail with an uninformative error). With
 * token_bytes=NULL and vocab_size=0 (the call shape in cli.c:190 and
 * cli.c:2016 today), the first batch's gpt_backward at src/ml/gpt.c:567
 * rejects the zero-sized grad_tensor and hu_ml_train returns
 * HU_ERR_INVALID_ARGUMENT immediately — no training, no BPB, but the CLI just
 * prints "Training failed: 0 steps, 0.00 bpb" without telling the user *why*.
 *
 * Either way (silent no-op or hard fail) it is a bug: the CLI flagship
 * subcommand cannot train. The fix in Task 4 wires real vocab_size +
 * token_bytes and lets the call return HU_OK with a real loss curve. */
static void test_ml_cli_train_with_zero_vocab_does_nothing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    pipeline_t p;
    build_pipeline(&alloc, "test_ml_cli_zero_vocab", &p);

    hu_training_config_t train_cfg = {0};
    train_cfg.device_batch_size = 2;
    train_cfg.time_budget_secs = 1;
    train_cfg.eval_tokens = 32;

    hu_ml_train_result_t result_buggy = {0};
    hu_error_t err = hu_ml_train(&alloc, &p.model, &p.opt, p.train_dl, p.val_dl, &train_cfg,
                                 /*token_bytes=*/NULL, /*vocab_size=*/0, &result_buggy);

    /* Two failure modes both prove the bug: either the call returns OK but
     * val_bpb is never populated (silent no-op), or the call returns
     * INVALID_ARGUMENT immediately (gpt_backward rejects the zero-shape
     * grad_tensor). Both are unacceptable. The fix in Task 4 produces neither. */
    HU_ASSERT(err == HU_ERR_INVALID_ARGUMENT ||
              (err == HU_OK && result_buggy.val_bpb == 0.0));

    cleanup_pipeline(&alloc, &p);
}

/* The fix shape. With a real vocab_size and an int32_t[vocab] token_bytes
 * lookup, the CE branch is entered (per-token grad is computed) and BPB is
 * recorded by hu_ml_evaluate_bpb at the end of the run. */
static void test_ml_cli_train_with_real_vocab_actually_trains(void) {
    hu_allocator_t alloc = hu_system_allocator();
    pipeline_t p;
    build_pipeline(&alloc, "test_ml_cli_real_vocab", &p);

    hu_training_config_t train_cfg = {0};
    train_cfg.device_batch_size = 2;
    train_cfg.time_budget_secs = 1;
    train_cfg.eval_tokens = 32;

    /* int32_t per spec — token_bytes[t] = #bytes that token t encodes.
     * For a synthetic byte-vocab of 128, every token is 1 byte. */
    int32_t token_bytes[128];
    for (int i = 0; i < 128; i++)
        token_bytes[i] = 1;

    hu_ml_train_result_t result = {0};
    hu_error_t err = hu_ml_train(&alloc, &p.model, &p.opt, p.train_dl, p.val_dl, &train_cfg,
                                 token_bytes, 128, &result);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_GT(result.num_steps, 0);
    HU_ASSERT_GT(result.total_tokens, 0);
    /* BPB is in bits-per-byte, finite and strictly positive when CE actually
     * ran. This is the assertion the bug shape cannot satisfy. We use
     * HU_ASSERT directly to avoid HU_ASSERT_GT's long-long truncation. */
    HU_ASSERT(result.val_bpb > 0.0);

    cleanup_pipeline(&alloc, &p);
}

#endif /* HU_ENABLE_ML */

void run_ml_cli_actually_trains_tests(void) {
    HU_TEST_SUITE("ml-cli-actually-trains");
#ifdef HU_ENABLE_ML
    HU_RUN_TEST(test_ml_cli_train_with_zero_vocab_does_nothing);
    HU_RUN_TEST(test_ml_cli_train_with_real_vocab_actually_trains);
#endif
}
