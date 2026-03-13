/* Tests for ML subsystem: BPE tokenizer, dataloader, prepare, experiment. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dataloader.h"
#include "human/ml/evaluator.h"
#include "human/ml/experiment.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include "human/ml/prepare.h"
#include "human/ml/cli.h"
#include "human/ml/tokenizer_ml.h"
#include "human/ml/train.h"
#include "test_framework.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ─── helpers ─────────────────────────────────────────────────────────────── */

static void mkdir_p(const char *path) {
#ifndef _WIN32
    mkdir(path, 0755);
#endif
}

static void write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static void write_bin_file(const char *path, const int32_t *tokens, size_t count) {
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(tokens, sizeof(int32_t), count, f);
        fclose(f);
    }
}

/* ─── BPE tokenizer: create and destroy ───────────────────────────────────── */

static void test_bpe_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bpe_tokenizer_t *tok = NULL;
    hu_error_t err = hu_bpe_tokenizer_create(&alloc, &tok);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tok);
    HU_ASSERT_EQ(hu_bpe_tokenizer_vocab_size(tok), 256);
    hu_bpe_tokenizer_deinit(tok);
}

/* ─── BPE tokenizer: encode single bytes ──────────────────────────────────── */

static void test_bpe_encode_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bpe_tokenizer_t *tok = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok);

    int32_t *ids = NULL;
    size_t count = 0;
    hu_error_t err = hu_bpe_tokenizer_encode(tok, "ABC", 3, &ids, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 3);
    HU_ASSERT_EQ(ids[0], 'A');
    HU_ASSERT_EQ(ids[1], 'B');
    HU_ASSERT_EQ(ids[2], 'C');

    alloc.free(alloc.ctx, ids, count * sizeof(int32_t));
    hu_bpe_tokenizer_deinit(tok);
}

/* ─── BPE tokenizer: decode roundtrip ─────────────────────────────────────── */

static void test_bpe_decode_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bpe_tokenizer_t *tok = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok);

    const char *text = "Hello, world!";
    int32_t *ids = NULL;
    size_t count = 0;
    hu_bpe_tokenizer_encode(tok, text, strlen(text), &ids, &count);

    char *decoded = NULL;
    size_t decoded_len = 0;
    hu_error_t err = hu_bpe_tokenizer_decode(tok, ids, count, &decoded, &decoded_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(decoded, text);
    HU_ASSERT_EQ(decoded_len, strlen(text));

    alloc.free(alloc.ctx, decoded, decoded_len + 1);
    alloc.free(alloc.ctx, ids, count * sizeof(int32_t));
    hu_bpe_tokenizer_deinit(tok);
}

/* ─── BPE tokenizer: train merges pairs ───────────────────────────────────── */

static void test_bpe_train_merges(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bpe_tokenizer_t *tok = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok);

    const char *texts[] = {
        "aaaa bbbb aaaa bbbb aaaa bbbb",
        "aaaa bbbb cccc aaaa bbbb cccc",
    };
    hu_error_t err = hu_bpe_tokenizer_train(tok, texts, 2, 260, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_GT(hu_bpe_tokenizer_vocab_size(tok), 256);

    /* After training, "aa" should be a single token, so encoding "aaaa"
       should produce fewer than 4 tokens. */
    int32_t *ids = NULL;
    size_t count = 0;
    hu_bpe_tokenizer_encode(tok, "aaaa", 4, &ids, &count);
    HU_ASSERT(count < 4);

    /* Roundtrip still works */
    char *decoded = NULL;
    size_t decoded_len = 0;
    hu_bpe_tokenizer_decode(tok, ids, count, &decoded, &decoded_len);
    HU_ASSERT_STR_EQ(decoded, "aaaa");

    alloc.free(alloc.ctx, decoded, decoded_len + 1);
    alloc.free(alloc.ctx, ids, count * sizeof(int32_t));
    hu_bpe_tokenizer_deinit(tok);
}

/* ─── BPE tokenizer: save and load ────────────────────────────────────────── */

static void test_bpe_save_load(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Train a tokenizer */
    hu_bpe_tokenizer_t *tok = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok);
    const char *texts[] = {"aabb aabb aabb ccdd ccdd ccdd"};
    hu_bpe_tokenizer_train(tok, texts, 1, 260, NULL);
    size_t orig_vocab = hu_bpe_tokenizer_vocab_size(tok);

    /* Save */
    const char *path = "/tmp/test_bpe_vocab.bin";
    hu_error_t err = hu_bpe_tokenizer_save(tok, path);
    HU_ASSERT_EQ(err, HU_OK);

    /* Load into fresh tokenizer */
    hu_bpe_tokenizer_t *tok2 = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok2);
    err = hu_bpe_tokenizer_load(tok2, path);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(hu_bpe_tokenizer_vocab_size(tok2), orig_vocab);

    /* Encode same text and verify match */
    int32_t *ids1 = NULL, *ids2 = NULL;
    size_t c1 = 0, c2 = 0;
    hu_bpe_tokenizer_encode(tok, "aabb", 4, &ids1, &c1);
    hu_bpe_tokenizer_encode(tok2, "aabb", 4, &ids2, &c2);
    HU_ASSERT_EQ(c1, c2);
    for (size_t i = 0; i < c1; i++)
        HU_ASSERT_EQ(ids1[i], ids2[i]);

    alloc.free(alloc.ctx, ids1, c1 * sizeof(int32_t));
    alloc.free(alloc.ctx, ids2, c2 * sizeof(int32_t));
    hu_bpe_tokenizer_deinit(tok);
    hu_bpe_tokenizer_deinit(tok2);
    remove(path);
}

/* ─── BPE tokenizer: empty input ──────────────────────────────────────────── */

static void test_bpe_encode_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bpe_tokenizer_t *tok = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok);

    int32_t *ids = NULL;
    size_t count = 0;
    hu_error_t err = hu_bpe_tokenizer_encode(tok, "", 0, &ids, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);

    hu_bpe_tokenizer_deinit(tok);
}

/* ─── BPE tokenizer: null arguments ───────────────────────────────────────── */

static void test_bpe_null_args(void) {
    hu_error_t err = hu_bpe_tokenizer_create(NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_allocator_t alloc = hu_system_allocator();
    err = hu_bpe_tokenizer_create(&alloc, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* ─── experiment config default ───────────────────────────────────────────── */

static void test_experiment_config_default(void) {
    hu_experiment_config_t cfg = hu_experiment_config_default();
    HU_ASSERT_EQ(cfg.gpt.n_layer, 8);
    HU_ASSERT_EQ(cfg.gpt.sequence_len, 2048);
    HU_ASSERT_EQ(cfg.gpt.vocab_size, 8192);
    HU_ASSERT_EQ(cfg.training.time_budget_secs, 300);
    HU_ASSERT_EQ(cfg.backend, HU_ML_BACKEND_CPU);
    HU_ASSERT_STR_EQ(cfg.gpt.window_pattern, "SSSL");
    HU_ASSERT_FLOAT_EQ(cfg.optimizer.adam_beta1, 0.8f, 0.001f);
    HU_ASSERT_FLOAT_EQ(cfg.optimizer.warmdown_ratio, 0.5f, 0.001f);
}

/* ─── experiment result to TSV ────────────────────────────────────────────── */

static void test_experiment_result_to_tsv(void) {
    hu_experiment_result_t result = {0};
    result.val_bpb = 0.997900;
    result.peak_memory_mb = 44000.0;
    result.status = HU_EXPERIMENT_KEEP;
    snprintf(result.description, sizeof(result.description), "baseline");
    result.iteration = 1;

    char buf[512];
    hu_error_t err = hu_experiment_result_to_tsv(&result, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(buf, "0.997900"));
    HU_ASSERT_NOT_NULL(strstr(buf, "keep"));
    HU_ASSERT_NOT_NULL(strstr(buf, "baseline"));
}

/* ─── experiment loop stub ────────────────────────────────────────────────── */

static void test_experiment_loop_null_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_experiment_loop(&alloc, NULL, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* ─── dataloader: create with missing dir ─────────────────────────────────── */

static void test_dataloader_missing_dir(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ml_dataloader_t *dl = NULL;
    hu_error_t err = hu_ml_dataloader_create(&alloc, "/tmp/nonexistent_ml_dir_xyz",
                                              4, 32, "train", &dl);
    /* Should fail because directory doesn't exist or has no .bin files */
    HU_ASSERT(err != HU_OK || dl == NULL);
    if (dl)
        hu_ml_dataloader_deinit(dl);
}

/* ─── dataloader: load and iterate ────────────────────────────────────────── */

static void test_dataloader_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();

    /* Create temp dir with two .bin shard files */
    const char *dir = "/tmp/test_ml_dataloader";
    mkdir_p(dir);

    /* Each shard has 200 tokens — enough for a few batches with batch_size=2, seq_len=16 */
    int32_t tokens[200];
    for (int i = 0; i < 200; i++)
        tokens[i] = i % 100;

    char path1[256], path2[256];
    snprintf(path1, sizeof(path1), "%s/shard_00000.bin", dir);
    snprintf(path2, sizeof(path2), "%s/shard_00001.bin", dir);
    write_bin_file(path1, tokens, 200);
    write_bin_file(path2, tokens, 200);

    /* Train split uses all but last shard */
    hu_ml_dataloader_t *dl = NULL;
    hu_error_t err = hu_ml_dataloader_create(&alloc, dir, 2, 16, "train", &dl);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(dl);

    /* Get a batch */
    hu_ml_batch_t batch = {0};
    err = hu_ml_dataloader_next(dl, &batch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(batch.batch_size, 2);
    HU_ASSERT_EQ(batch.seq_len, 16);
    HU_ASSERT_NOT_NULL(batch.input_ids);
    HU_ASSERT_NOT_NULL(batch.target_ids);

    /* input[0][i] + 1 == target[0][i] (since tokens are sequential mod 100) */
    for (size_t i = 0; i < 16; i++) {
        int32_t expected_target = (batch.input_ids[i] + 1) % 100;
        HU_ASSERT_EQ(batch.target_ids[i], expected_target);
    }

    hu_ml_batch_free(&alloc, &batch);
    hu_ml_dataloader_deinit(dl);

    /* Val split uses only last shard */
    err = hu_ml_dataloader_create(&alloc, dir, 2, 16, "val", &dl);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(dl);

    err = hu_ml_dataloader_next(dl, &batch);
    HU_ASSERT_EQ(err, HU_OK);
    hu_ml_batch_free(&alloc, &batch);
    hu_ml_dataloader_deinit(dl);

    remove(path1);
    remove(path2);
    rmdir(dir);
}

/* ─── evaluator: null model returns error ─────────────────────────────────── */

static void test_evaluator_null_model(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ml_eval_result_t result = {0};
    int32_t token_bytes[] = {1, 1, 1};
    hu_error_t err = hu_ml_evaluate_bpb(&alloc, NULL, NULL, token_bytes, 3, 100, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* ─── prepare: token bytes lookup ─────────────────────────────────────────── */

static void test_prepare_token_bytes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bpe_tokenizer_t *tok = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok);

    int32_t *token_bytes = NULL;
    size_t count = 0;
    hu_error_t err = hu_ml_prepare_token_bytes(&alloc, tok, &token_bytes, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 256);
    HU_ASSERT_NOT_NULL(token_bytes);

    /* ASCII bytes should decode to 1 byte each */
    HU_ASSERT_EQ(token_bytes['A'], 1);
    HU_ASSERT_EQ(token_bytes[' '], 1);

    alloc.free(alloc.ctx, token_bytes, count * sizeof(int32_t));
    hu_bpe_tokenizer_deinit(tok);
}

/* ─── prepare: tokenize file ──────────────────────────────────────────────── */

static void test_prepare_tokenize_file(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bpe_tokenizer_t *tok = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok);

    const char *txt_path = "/tmp/test_ml_input.txt";
    const char *bin_path = "/tmp/test_ml_output.bin";

    write_text_file(txt_path, "Hello world test data for tokenizer");

    hu_error_t err = hu_ml_prepare_tokenize_file(&alloc, tok, txt_path, bin_path);
    HU_ASSERT_EQ(err, HU_OK);

    /* Verify bin file exists and has content */
    FILE *f = fopen(bin_path, "rb");
    HU_ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    HU_ASSERT(size > 0);
    HU_ASSERT_EQ(size % (long)sizeof(int32_t), 0);

    hu_bpe_tokenizer_deinit(tok);
    remove(txt_path);
    remove(bin_path);
}

/* ─── BPE tokenizer: UTF-8 roundtrip ─────────────────────────────────────── */

static void test_bpe_utf8_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bpe_tokenizer_t *tok = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok);

    const char *text = "\xc3\xa9\xc3\xa0\xc3\xbc";  /* éàü in UTF-8 */
    int32_t *ids = NULL;
    size_t count = 0;
    hu_bpe_tokenizer_encode(tok, text, strlen(text), &ids, &count);
    HU_ASSERT_EQ(count, 6);  /* 3 chars * 2 bytes each */

    char *decoded = NULL;
    size_t decoded_len = 0;
    hu_bpe_tokenizer_decode(tok, ids, count, &decoded, &decoded_len);
    HU_ASSERT_STR_EQ(decoded, text);

    alloc.free(alloc.ctx, decoded, decoded_len + 1);
    alloc.free(alloc.ctx, ids, count * sizeof(int32_t));
    hu_bpe_tokenizer_deinit(tok);
}

/* ─── GPT: create and destroy ─────────────────────────────────────────────── */

static void test_gpt_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {0};
    cfg.sequence_len = 32;
    cfg.vocab_size = 256;
    cfg.n_layer = 2;
    cfg.n_head = 2;
    cfg.n_kv_head = 2;
    cfg.n_embd = 64;
    cfg.head_dim = 32;
    cfg.activation = HU_ML_ACT_RELU_SQ;

    hu_model_t model = {0};
    hu_error_t err = hu_gpt_create(&alloc, &cfg, &model);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(model.ctx);
    HU_ASSERT_NOT_NULL(model.vtable);

    size_t n = model.vtable->num_params(model.ctx);
    HU_ASSERT_GT(n, 0);

    model.vtable->deinit(model.ctx, &alloc);
}

/* ─── GPT: null args ─────────────────────────────────────────────────────── */

static void test_gpt_create_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {0};
    cfg.sequence_len = 32;
    cfg.vocab_size = 256;
    cfg.n_layer = 2;
    cfg.n_head = 2;
    cfg.n_kv_head = 2;
    cfg.n_embd = 64;
    cfg.head_dim = 32;

    hu_model_t model = {0};
    HU_ASSERT_EQ(hu_gpt_create(NULL, &cfg, &model), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_gpt_create(&alloc, NULL, &model), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_gpt_create(&alloc, &cfg, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* ─── GPT: invalid config ─────────────────────────────────────────────────── */

static void test_gpt_create_invalid_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {0};
    cfg.sequence_len = 32;
    cfg.vocab_size = 256;
    cfg.n_layer = 2;
    cfg.n_head = 2;
    cfg.n_kv_head = 2;
    cfg.n_embd = 64;
    cfg.head_dim = 32;

    hu_model_t model = {0};
    cfg.n_embd = 63;
    HU_ASSERT_EQ(hu_gpt_create(&alloc, &cfg, &model), HU_ERR_INVALID_ARGUMENT);
}

/* ─── GPT: forward pass ───────────────────────────────────────────────────── */

static void test_gpt_forward(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {0};
    cfg.sequence_len = 16;
    cfg.vocab_size = 128;
    cfg.n_layer = 1;
    cfg.n_head = 2;
    cfg.n_kv_head = 2;
    cfg.n_embd = 64;
    cfg.head_dim = 32;
    cfg.activation = HU_ML_ACT_RELU_SQ;

    hu_model_t model = {0};
    hu_error_t err = hu_gpt_create(&alloc, &cfg, &model);
    HU_ASSERT_EQ(err, HU_OK);

    int32_t ids[4 * 8];
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        ids[i] = (int)(i % 128);

    hu_ml_tensor_t input = {0};
    input.data = ids;
    input.shape[0] = 4;
    input.shape[1] = 8;
    input.ndim = 2;
    input.dtype = HU_ML_DTYPE_I32;

    hu_ml_tensor_t output = {0};
    err = model.vtable->forward(model.ctx, &input, &output);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(output.data);
    HU_ASSERT_EQ(output.ndim, 3);
    HU_ASSERT_EQ(output.shape[0], 4);
    HU_ASSERT_EQ(output.shape[1], 8);
    HU_ASSERT_EQ(output.shape[2], 128);
    HU_ASSERT_EQ(output.dtype, HU_ML_DTYPE_F32);

    alloc.free(alloc.ctx, output.data, output.size_bytes);
    model.vtable->deinit(model.ctx, &alloc);
}

/* ─── MuonAdamW: create and destroy ───────────────────────────────────────── */

static void test_muon_adamw_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_optimizer_config_t cfg = hu_experiment_config_default().optimizer;
    hu_ml_optimizer_t opt = {0};

    hu_error_t err = hu_muon_adamw_create(&alloc, &cfg, &opt);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(opt.ctx);
    HU_ASSERT_NOT_NULL(opt.vtable);

    opt.vtable->deinit(opt.ctx, &alloc);
}

/* ─── MuonAdamW: step changes params with non-zero grads ──────────────────── */

static void test_muon_adamw_step(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_optimizer_config_t cfg = hu_experiment_config_default().optimizer;
    cfg.embedding_lr = 0.1f;
    cfg.scalar_lr = 0.1f;
    cfg.matrix_lr = 0.1f;
    cfg.weight_decay = 0.01f;

    hu_ml_optimizer_t opt = {0};
    HU_ASSERT_EQ(hu_muon_adamw_create(&alloc, &cfg, &opt), HU_OK);

    float param[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float grad[4] = {0.5f, -0.3f, 0.2f, -0.1f};

    HU_ASSERT_EQ(hu_muon_adamw_add_param(&opt, param, grad, 2, 2, HU_PARAM_MATRIX), HU_OK);

    float before = param[0];
    opt.vtable->step(opt.ctx, NULL, NULL, 0);
    float after = param[0];
    HU_ASSERT_NEQ(before, after);

    opt.vtable->deinit(opt.ctx, &alloc);
}

/* ─── LR schedule: warmup and warmdown curve ──────────────────────────────── */

static void test_lr_schedule(void) {
    float r;

    r = hu_ml_lr_schedule(0.0f, 0.1f, 0.2f, 0.1f);
    HU_ASSERT_FLOAT_EQ(r, 0.0f, 0.001f);

    r = hu_ml_lr_schedule(0.05f, 0.1f, 0.2f, 0.1f);
    HU_ASSERT_FLOAT_EQ(r, 0.5f, 0.01f);

    r = hu_ml_lr_schedule(0.1f, 0.1f, 0.2f, 0.1f);
    HU_ASSERT_FLOAT_EQ(r, 1.0f, 0.001f);

    r = hu_ml_lr_schedule(0.5f, 0.1f, 0.2f, 0.1f);
    HU_ASSERT_FLOAT_EQ(r, 1.0f, 0.001f);

    r = hu_ml_lr_schedule(0.9f, 0.1f, 0.2f, 0.1f);
    HU_ASSERT_FLOAT_EQ(r, 0.55f, 0.05f);

    r = hu_ml_lr_schedule(1.0f, 0.1f, 0.2f, 0.1f);
    HU_ASSERT_FLOAT_EQ(r, 0.1f, 0.001f);
}

/* ─── Train pipeline: model + optimizer + dataloader ──────────────────────── */

static void test_train_pipeline(void) {
    hu_allocator_t alloc = hu_system_allocator();

    const char *dir = "/tmp/test_ml_train";
    mkdir_p(dir);

    int32_t tokens[200];
    for (int i = 0; i < 200; i++)
        tokens[i] = i % 128;

    char path1[256], path2[256];
    snprintf(path1, sizeof(path1), "%s/shard_00000.bin", dir);
    snprintf(path2, sizeof(path2), "%s/shard_00001.bin", dir);
    write_bin_file(path1, tokens, 200);
    write_bin_file(path2, tokens, 200);

    hu_ml_dataloader_t *train_dl = NULL;
    hu_error_t err = hu_ml_dataloader_create(&alloc, dir, 2, 8, "train", &train_dl);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(train_dl);

    hu_ml_dataloader_t *val_dl = NULL;
    err = hu_ml_dataloader_create(&alloc, dir, 2, 8, "val", &val_dl);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(val_dl);

    hu_gpt_config_t gpt_cfg = {0};
    gpt_cfg.sequence_len = 16;
    gpt_cfg.vocab_size = 128;
    gpt_cfg.n_layer = 1;
    gpt_cfg.n_head = 2;
    gpt_cfg.n_kv_head = 2;
    gpt_cfg.n_embd = 64;
    gpt_cfg.head_dim = 32;
    gpt_cfg.activation = HU_ML_ACT_RELU_SQ;

    hu_model_t model = {0};
    err = hu_gpt_create(&alloc, &gpt_cfg, &model);
    HU_ASSERT_EQ(err, HU_OK);

    hu_optimizer_config_t opt_cfg = hu_experiment_config_default().optimizer;
    hu_ml_optimizer_t optimizer = {0};
    err = hu_muon_adamw_create(&alloc, &opt_cfg, &optimizer);
    HU_ASSERT_EQ(err, HU_OK);

    int32_t token_bytes[128];
    for (int i = 0; i < 128; i++)
        token_bytes[i] = 1;

    hu_training_config_t train_cfg = {0};
    train_cfg.device_batch_size = 2;
    train_cfg.time_budget_secs = 1;
    train_cfg.eval_tokens = 32;

    hu_ml_train_result_t result = {0};
    err = hu_ml_train(&alloc, &model, &optimizer, train_dl, val_dl,
                     &train_cfg, token_bytes, 128, &result);

    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_GT(result.num_steps, 0);
    HU_ASSERT_GT(result.total_tokens, 0);
    HU_ASSERT_EQ(result.converged, 1);

    optimizer.vtable->deinit(optimizer.ctx, &alloc);
    model.vtable->deinit(model.ctx, &alloc);
    hu_ml_dataloader_deinit(val_dl);
    hu_ml_dataloader_deinit(train_dl);
    remove(path1);
    remove(path2);
    rmdir(dir);
}

/* ─── GPT: backward returns not supported ──────────────────────────────────── */

static void test_gpt_backward_not_supported(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {0};
    cfg.sequence_len = 8;
    cfg.vocab_size = 64;
    cfg.n_layer = 1;
    cfg.n_head = 2;
    cfg.n_kv_head = 2;
    cfg.n_embd = 64;
    cfg.head_dim = 32;

    hu_model_t model = {0};
    hu_gpt_create(&alloc, &cfg, &model);

    hu_ml_tensor_t grad = {0};
    HU_ASSERT_EQ(model.vtable->backward(model.ctx, &grad), HU_ERR_NOT_SUPPORTED);

    model.vtable->deinit(model.ctx, &alloc);
}

/* ─── GPT: forward produces finite logits ──────────────────────────────── */

static void test_gpt_forward_logits_finite(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {0};
    cfg.sequence_len = 16;
    cfg.vocab_size = 128;
    cfg.n_layer = 2;
    cfg.n_head = 2;
    cfg.n_kv_head = 2;
    cfg.n_embd = 64;
    cfg.head_dim = 32;
    cfg.activation = HU_ML_ACT_RELU_SQ;

    hu_model_t model = {0};
    HU_ASSERT_EQ(hu_gpt_create(&alloc, &cfg, &model), HU_OK);

    int32_t ids[2 * 8];
    for (size_t i = 0; i < 16; i++)
        ids[i] = (int32_t)(i % 128);

    hu_ml_tensor_t input = {.data = ids, .shape = {2, 8, 0, 0}, .ndim = 2, .dtype = HU_ML_DTYPE_I32};
    hu_ml_tensor_t output = {0};
    HU_ASSERT_EQ(model.vtable->forward(model.ctx, &input, &output), HU_OK);

    float *logits = (float *)output.data;
    size_t total = output.shape[0] * output.shape[1] * output.shape[2];
    for (size_t i = 0; i < total; i++) {
        HU_ASSERT(isfinite(logits[i]));
    }

    alloc.free(alloc.ctx, output.data, output.size_bytes);
    model.vtable->deinit(model.ctx, &alloc);
}

/* ─── GPT: get_params returns NOT_SUPPORTED ────────────────────────────── */

static void test_gpt_get_params_stub(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_gpt_config_t cfg = {0};
    cfg.sequence_len = 8;
    cfg.vocab_size = 64;
    cfg.n_layer = 1;
    cfg.n_head = 2;
    cfg.n_kv_head = 2;
    cfg.n_embd = 64;
    cfg.head_dim = 32;

    hu_model_t model = {0};
    hu_gpt_create(&alloc, &cfg, &model);

    hu_ml_tensor_t *params = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(model.vtable->get_params(model.ctx, &params, &count), HU_ERR_NOT_SUPPORTED);

    model.vtable->deinit(model.ctx, &alloc);
}

/* ─── MuonAdamW: optimizer step moves params in gradient direction ─────── */

static void test_muon_adamw_step_direction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_optimizer_config_t cfg = hu_experiment_config_default().optimizer;
    cfg.embedding_lr = 0.1f;
    cfg.scalar_lr = 0.1f;
    cfg.weight_decay = 0.0f;

    hu_ml_optimizer_t opt = {0};
    HU_ASSERT_EQ(hu_muon_adamw_create(&alloc, &cfg, &opt), HU_OK);

    float param[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float grad[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
    float before[4];
    memcpy(before, param, sizeof(param));

    HU_ASSERT_EQ(hu_muon_adamw_add_param(&opt, param, grad, 1, 4, HU_PARAM_SCALAR), HU_OK);
    opt.vtable->step(opt.ctx, NULL, NULL, 0);

    for (int i = 0; i < 4; i++) {
        HU_ASSERT(param[i] < before[i]);
    }

    opt.vtable->deinit(opt.ctx, &alloc);
}

/* ─── MuonAdamW: zero_grad zeroes gradients ────────────────────────────── */

static void test_muon_adamw_zero_grad(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_optimizer_config_t cfg = hu_experiment_config_default().optimizer;
    hu_ml_optimizer_t opt = {0};
    HU_ASSERT_EQ(hu_muon_adamw_create(&alloc, &cfg, &opt), HU_OK);

    float param[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float grad[4]  = {0.5f, -0.3f, 0.2f, -0.1f};
    HU_ASSERT_EQ(hu_muon_adamw_add_param(&opt, param, grad, 1, 4, HU_PARAM_SCALAR), HU_OK);

    opt.vtable->zero_grad(opt.ctx);
    for (int i = 0; i < 4; i++) {
        HU_ASSERT_FLOAT_EQ(grad[i], 0.0f, 1e-10f);
    }

    opt.vtable->deinit(opt.ctx, &alloc);
}

/* ─── MuonAdamW: set_lr_multiplier affects param updates ───────────────── */

static void test_muon_adamw_lr_multiplier(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_optimizer_config_t cfg = hu_experiment_config_default().optimizer;
    cfg.scalar_lr = 0.1f;
    cfg.weight_decay = 0.0f;

    /* Step with multiplier=1 */
    float param_a[2] = {1.0f, 1.0f};
    float grad_a[2]  = {1.0f, 1.0f};
    hu_ml_optimizer_t opt_a = {0};
    HU_ASSERT_EQ(hu_muon_adamw_create(&alloc, &cfg, &opt_a), HU_OK);
    HU_ASSERT_EQ(hu_muon_adamw_add_param(&opt_a, param_a, grad_a, 1, 2, HU_PARAM_SCALAR), HU_OK);
    opt_a.vtable->step(opt_a.ctx, NULL, NULL, 0);

    /* Step with multiplier=0 (effectively no update) */
    float param_b[2] = {1.0f, 1.0f};
    float grad_b[2]  = {1.0f, 1.0f};
    hu_ml_optimizer_t opt_b = {0};
    HU_ASSERT_EQ(hu_muon_adamw_create(&alloc, &cfg, &opt_b), HU_OK);
    HU_ASSERT_EQ(hu_muon_adamw_add_param(&opt_b, param_b, grad_b, 1, 2, HU_PARAM_SCALAR), HU_OK);
    opt_b.vtable->set_lr_multiplier(opt_b.ctx, 0.0f);
    opt_b.vtable->step(opt_b.ctx, NULL, NULL, 0);

    HU_ASSERT(fabsf(param_a[0] - 1.0f) > fabsf(param_b[0] - 1.0f));

    opt_a.vtable->deinit(opt_a.ctx, &alloc);
    opt_b.vtable->deinit(opt_b.ctx, &alloc);
}

/* ─── MuonAdamW: null args ─────────────────────────────────────────────── */

static void test_muon_adamw_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_optimizer_config_t cfg = hu_experiment_config_default().optimizer;
    hu_ml_optimizer_t opt = {0};

    HU_ASSERT_EQ(hu_muon_adamw_create(NULL, &cfg, &opt), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_muon_adamw_create(&alloc, NULL, &opt), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_muon_adamw_create(&alloc, &cfg, NULL), HU_ERR_INVALID_ARGUMENT);

    HU_ASSERT_EQ(hu_muon_adamw_add_param(NULL, NULL, NULL, 1, 1, HU_PARAM_SCALAR), HU_ERR_INVALID_ARGUMENT);
}

/* ─── MuonAdamW: add_param with zero size ──────────────────────────────── */

static void test_muon_adamw_add_param_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_optimizer_config_t cfg = hu_experiment_config_default().optimizer;
    hu_ml_optimizer_t opt = {0};
    HU_ASSERT_EQ(hu_muon_adamw_create(&alloc, &cfg, &opt), HU_OK);

    float param = 1.0f, grad = 0.5f;
    HU_ASSERT_EQ(hu_muon_adamw_add_param(&opt, &param, &grad, 0, 1, HU_PARAM_SCALAR), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_muon_adamw_add_param(&opt, &param, &grad, 1, 0, HU_PARAM_SCALAR), HU_ERR_INVALID_ARGUMENT);

    opt.vtable->deinit(opt.ctx, &alloc);
}

/* ─── Dataloader: reset resets position ────────────────────────────────── */

static void test_dataloader_reset(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *dir = "/tmp/test_ml_dl_reset";
    mkdir_p(dir);

    int32_t tokens[200];
    for (int i = 0; i < 200; i++)
        tokens[i] = i % 100;
    char path1[256], path2[256];
    snprintf(path1, sizeof(path1), "%s/shard_00000.bin", dir);
    snprintf(path2, sizeof(path2), "%s/shard_00001.bin", dir);
    write_bin_file(path1, tokens, 200);
    write_bin_file(path2, tokens, 200);

    hu_ml_dataloader_t *dl = NULL;
    HU_ASSERT_EQ(hu_ml_dataloader_create(&alloc, dir, 2, 16, "train", &dl), HU_OK);

    hu_ml_batch_t b1 = {0};
    HU_ASSERT_EQ(hu_ml_dataloader_next(dl, &b1), HU_OK);
    int32_t first_id = b1.input_ids[0];
    hu_ml_batch_free(&alloc, &b1);

    HU_ASSERT_EQ(hu_ml_dataloader_next(dl, &b1), HU_OK);
    hu_ml_batch_free(&alloc, &b1);

    hu_ml_dataloader_reset(dl);

    hu_ml_batch_t b2 = {0};
    HU_ASSERT_EQ(hu_ml_dataloader_next(dl, &b2), HU_OK);
    HU_ASSERT_EQ(b2.input_ids[0], first_id);
    hu_ml_batch_free(&alloc, &b2);

    hu_ml_dataloader_deinit(dl);
    remove(path1);
    remove(path2);
    rmdir(dir);
}

/* ─── Dataloader: null args ────────────────────────────────────────────── */

static void test_dataloader_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ml_dataloader_t *dl = NULL;
    HU_ASSERT_EQ(hu_ml_dataloader_create(NULL, "/tmp", 1, 1, "train", &dl), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_ml_dataloader_create(&alloc, NULL, 1, 1, "train", &dl), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_ml_dataloader_create(&alloc, "/tmp", 1, 1, "train", NULL), HU_ERR_INVALID_ARGUMENT);
}

/* ─── BPE tokenizer: token_byte_length ─────────────────────────────────── */

static void test_bpe_token_byte_length(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bpe_tokenizer_t *tok = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok);

    HU_ASSERT_EQ(hu_bpe_tokenizer_token_byte_length(tok, 'A'), 1);
    HU_ASSERT_EQ(hu_bpe_tokenizer_token_byte_length(tok, 0), 1);
    HU_ASSERT_EQ(hu_bpe_tokenizer_token_byte_length(tok, 255), 1);
    HU_ASSERT_EQ(hu_bpe_tokenizer_token_byte_length(tok, 9999), 0);

    hu_bpe_tokenizer_deinit(tok);
}

/* ─── BPE tokenizer: trained merge token byte length > 1 ──────────────── */

static void test_bpe_trained_token_byte_length(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bpe_tokenizer_t *tok = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok);

    const char *texts[] = {"ab ab ab ab ab ab ab ab ab ab ab"};
    hu_bpe_tokenizer_train(tok, texts, 1, 260, NULL);
    HU_ASSERT_GT(hu_bpe_tokenizer_vocab_size(tok), 256);

    size_t byte_len = hu_bpe_tokenizer_token_byte_length(tok, 256);
    HU_ASSERT_GT(byte_len, 1);

    hu_bpe_tokenizer_deinit(tok);
}

/* ─── Evaluator: happy path with real model ────────────────────────────── */

static void test_evaluator_happy_path(void) {
    hu_allocator_t alloc = hu_system_allocator();

    const char *dir = "/tmp/test_ml_eval_happy";
    mkdir_p(dir);
    int32_t tokens[200];
    for (int i = 0; i < 200; i++)
        tokens[i] = i % 64;
    char path1[256], path2[256];
    snprintf(path1, sizeof(path1), "%s/shard_00000.bin", dir);
    snprintf(path2, sizeof(path2), "%s/shard_00001.bin", dir);
    write_bin_file(path1, tokens, 200);
    write_bin_file(path2, tokens, 200);

    hu_gpt_config_t gpt_cfg = {0};
    gpt_cfg.sequence_len = 16;
    gpt_cfg.vocab_size = 64;
    gpt_cfg.n_layer = 1;
    gpt_cfg.n_head = 2;
    gpt_cfg.n_kv_head = 2;
    gpt_cfg.n_embd = 64;
    gpt_cfg.head_dim = 32;
    gpt_cfg.activation = HU_ML_ACT_RELU_SQ;

    hu_model_t model = {0};
    HU_ASSERT_EQ(hu_gpt_create(&alloc, &gpt_cfg, &model), HU_OK);

    hu_ml_dataloader_t *val_dl = NULL;
    HU_ASSERT_EQ(hu_ml_dataloader_create(&alloc, dir, 2, 8, "val", &val_dl), HU_OK);

    int32_t token_bytes[64];
    for (int i = 0; i < 64; i++)
        token_bytes[i] = 1;

    hu_ml_eval_result_t result = {0};
    hu_error_t err = hu_ml_evaluate_bpb(&alloc, &model, val_dl, token_bytes, 64, 16, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_GT(result.val_bpb, 0.0);
    HU_ASSERT(isfinite(result.val_bpb));
    HU_ASSERT_GT(result.total_bytes, 0);
    HU_ASSERT_GT(result.total_nats, 0.0);

    hu_ml_dataloader_deinit(val_dl);
    model.vtable->deinit(model.ctx, &alloc);
    remove(path1);
    remove(path2);
    rmdir(dir);
}

/* ─── TSV: overflow returns error ──────────────────────────────────────── */

static void test_experiment_result_to_tsv_overflow(void) {
    hu_experiment_result_t result = {0};
    result.val_bpb = 1.0;
    result.status = HU_EXPERIMENT_KEEP;
    snprintf(result.description, sizeof(result.description), "test");

    char tiny_buf[5];
    hu_error_t err = hu_experiment_result_to_tsv(&result, tiny_buf, sizeof(tiny_buf));
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* ─── TSV: null args ───────────────────────────────────────────────────── */

static void test_experiment_result_to_tsv_null(void) {
    char buf[256];
    HU_ASSERT_EQ(hu_experiment_result_to_tsv(NULL, buf, sizeof(buf)), HU_ERR_INVALID_ARGUMENT);
    hu_experiment_result_t result = {0};
    HU_ASSERT_EQ(hu_experiment_result_to_tsv(&result, NULL, sizeof(buf)), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_experiment_result_to_tsv(&result, buf, 0), HU_ERR_INVALID_ARGUMENT);
}

/* ─── Experiment loop: KEEP/DISCARD decision logic ─────────────────────── */

static int keep_count;
static int discard_count;
static int crash_count;

static void track_status_callback(const hu_experiment_result_t *result,
                                   void *user_data)
{
    (void)user_data;
    switch (result->status) {
    case HU_EXPERIMENT_KEEP: keep_count++; break;
    case HU_EXPERIMENT_DISCARD: discard_count++; break;
    case HU_EXPERIMENT_CRASH: crash_count++; break;
    }
}

static void test_experiment_loop_keep_discard(void) {
    hu_allocator_t alloc = hu_system_allocator();

    const char *dir = "/tmp/test_ml_keepdisc";
    mkdir_p(dir);
    int32_t tokens[400];
    for (int i = 0; i < 400; i++)
        tokens[i] = i % 50;
    char path1[256], path2[256];
    snprintf(path1, sizeof(path1), "%s/shard_00000.bin", dir);
    snprintf(path2, sizeof(path2), "%s/shard_00001.bin", dir);
    write_bin_file(path1, tokens, 400);
    write_bin_file(path2, tokens, 400);

    hu_experiment_loop_config_t loop_cfg = {0};
    loop_cfg.max_iterations = 3;
    loop_cfg.base_config = hu_experiment_config_default();
    loop_cfg.base_config.gpt.n_layer = 1;
    loop_cfg.base_config.gpt.n_embd = 64;
    loop_cfg.base_config.gpt.head_dim = 32;
    loop_cfg.base_config.gpt.n_head = 2;
    loop_cfg.base_config.gpt.n_kv_head = 2;
    loop_cfg.base_config.gpt.vocab_size = 50;
    loop_cfg.base_config.gpt.sequence_len = 16;
    loop_cfg.base_config.training.device_batch_size = 2;
    loop_cfg.base_config.training.time_budget_secs = 1;
    loop_cfg.base_config.training.total_batch_size = 32;
    loop_cfg.data_dir = dir;
    loop_cfg.convergence_threshold = 0.0;

    keep_count = discard_count = crash_count = 0;
    hu_error_t err = hu_experiment_loop(&alloc, &loop_cfg, track_status_callback, NULL);
    HU_ASSERT_EQ(err, HU_OK);

    int total = keep_count + discard_count + crash_count;
    HU_ASSERT_EQ(total, 3);
    HU_ASSERT_GT(keep_count, 0);

    remove(path1);
    remove(path2);
    rmdir(dir);
}

/* ─── Experiment loop: zero iterations ─────────────────────────────────── */

static void test_experiment_loop_zero_iterations(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_experiment_loop_config_t loop_cfg = {0};
    loop_cfg.max_iterations = 0;
    loop_cfg.data_dir = "/tmp";
    HU_ASSERT_EQ(hu_experiment_loop(&alloc, &loop_cfg, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* ─── Experiment loop: null data_dir ───────────────────────────────────── */

static void test_experiment_loop_null_data_dir(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_experiment_loop_config_t loop_cfg = {0};
    loop_cfg.max_iterations = 1;
    loop_cfg.data_dir = NULL;
    HU_ASSERT_EQ(hu_experiment_loop(&alloc, &loop_cfg, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* ─── Batch free: null args are safe ───────────────────────────────────── */

static void test_batch_free_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ml_batch_free(NULL, NULL);
    hu_ml_batch_free(&alloc, NULL);
    hu_ml_batch_t batch = {0};
    hu_ml_batch_free(&alloc, &batch);
}

/* ─── Prepare: null args ───────────────────────────────────────────────── */

static void test_prepare_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bpe_tokenizer_t *tok = NULL;
    hu_bpe_tokenizer_create(&alloc, &tok);

    int32_t *tb = NULL;
    size_t cnt = 0;
    HU_ASSERT_EQ(hu_ml_prepare_token_bytes(NULL, tok, &tb, &cnt), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_ml_prepare_token_bytes(&alloc, NULL, &tb, &cnt), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_ml_prepare_token_bytes(&alloc, tok, NULL, &cnt), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_ml_prepare_token_bytes(&alloc, tok, &tb, NULL), HU_ERR_INVALID_ARGUMENT);

    hu_bpe_tokenizer_deinit(tok);
}

/* ─── suite runner ────────────────────────────────────────────────────────── */

/* ─── experiment loop: runs with test data ─────────────────────────────── */

static int experiment_callback_count;
static hu_experiment_result_t last_callback_result;

static void test_experiment_callback(const hu_experiment_result_t *result,
                                      void *user_data)
{
    (void)user_data;
    experiment_callback_count++;
    last_callback_result = *result;
}

static void test_experiment_loop_runs(void) {
    hu_allocator_t alloc = hu_system_allocator();

    const char *dir = "/tmp/test_ml_experiment";
    mkdir_p(dir);

    int32_t tokens[400];
    for (int i = 0; i < 400; i++)
        tokens[i] = i % 50;
    char path1[256], path2[256];
    snprintf(path1, sizeof(path1), "%s/shard_00000.bin", dir);
    snprintf(path2, sizeof(path2), "%s/shard_00001.bin", dir);
    write_bin_file(path1, tokens, 400);
    write_bin_file(path2, tokens, 400);

    hu_experiment_loop_config_t loop_cfg = {0};
    loop_cfg.max_iterations = 2;
    loop_cfg.base_config = hu_experiment_config_default();
    loop_cfg.base_config.gpt.n_layer = 1;
    loop_cfg.base_config.gpt.n_embd = 64;
    loop_cfg.base_config.gpt.head_dim = 32;
    loop_cfg.base_config.gpt.n_head = 2;
    loop_cfg.base_config.gpt.n_kv_head = 2;
    loop_cfg.base_config.gpt.vocab_size = 50;
    loop_cfg.base_config.gpt.sequence_len = 16;
    loop_cfg.base_config.training.device_batch_size = 2;
    loop_cfg.base_config.training.time_budget_secs = 1;
    loop_cfg.base_config.training.total_batch_size = 32;
    loop_cfg.data_dir = dir;
    loop_cfg.convergence_threshold = 0.0;

    experiment_callback_count = 0;
    hu_error_t err = hu_experiment_loop(&alloc, &loop_cfg,
                                         test_experiment_callback, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(experiment_callback_count, 2);
    HU_ASSERT(last_callback_result.status == HU_EXPERIMENT_KEEP ||
              last_callback_result.status == HU_EXPERIMENT_DISCARD ||
              last_callback_result.status == HU_EXPERIMENT_CRASH);

    remove(path1);
    remove(path2);
    rmdir(dir);
}

/* ─── ML CLI: train --help ─────────────────────────────────────────────── */

static void test_ml_cli_train_help(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"human", "ml", "train", "--help"};
    hu_error_t err = hu_ml_cli_train(&alloc, 4, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

/* ─── ML CLI: experiment --help ─────────────────────────────────────────── */

static void test_ml_cli_experiment_help(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"human", "experiment", "--help"};
    hu_error_t err = hu_ml_cli_experiment(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

/* ─── ML CLI: prepare --help ────────────────────────────────────────────── */

static void test_ml_cli_prepare_help(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"human", "ml", "prepare", "--help"};
    hu_error_t err = hu_ml_cli_prepare(&alloc, 4, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

/* ─── ML CLI: status ───────────────────────────────────────────────────── */

static void test_ml_cli_status(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"human", "ml", "status"};
    hu_error_t err = hu_ml_cli_status(&alloc, 3, argv);
    HU_ASSERT_EQ(err, HU_OK);
}

/* ─── experiment loop: convergence threshold stops early ───────────────── */

static void test_experiment_loop_convergence(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_experiment_loop_config_t loop_cfg = {0};
    loop_cfg.max_iterations = 100;
    loop_cfg.base_config = hu_experiment_config_default();
    loop_cfg.data_dir = "/tmp/nonexistent_experiment_data";
    loop_cfg.convergence_threshold = 999.0;

    experiment_callback_count = 0;
    hu_error_t err = hu_experiment_loop(&alloc, &loop_cfg,
                                         test_experiment_callback, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    /* Should run but crash experiments due to missing data dir */
    HU_ASSERT(experiment_callback_count > 0);
    HU_ASSERT_EQ(last_callback_result.status, HU_EXPERIMENT_CRASH);
}

void run_ml_tests(void) {
    HU_TEST_SUITE("ml");
    /* BPE tokenizer */
    HU_RUN_TEST(test_bpe_create_destroy);
    HU_RUN_TEST(test_bpe_encode_bytes);
    HU_RUN_TEST(test_bpe_decode_roundtrip);
    HU_RUN_TEST(test_bpe_train_merges);
    HU_RUN_TEST(test_bpe_save_load);
    HU_RUN_TEST(test_bpe_encode_empty);
    HU_RUN_TEST(test_bpe_null_args);
    HU_RUN_TEST(test_bpe_utf8_roundtrip);
    HU_RUN_TEST(test_bpe_token_byte_length);
    HU_RUN_TEST(test_bpe_trained_token_byte_length);
    /* Config / TSV */
    HU_RUN_TEST(test_experiment_config_default);
    HU_RUN_TEST(test_experiment_result_to_tsv);
    HU_RUN_TEST(test_experiment_result_to_tsv_overflow);
    HU_RUN_TEST(test_experiment_result_to_tsv_null);
    /* Dataloader */
    HU_RUN_TEST(test_dataloader_missing_dir);
    HU_RUN_TEST(test_dataloader_basic);
    HU_RUN_TEST(test_dataloader_reset);
    HU_RUN_TEST(test_dataloader_null_args);
    HU_RUN_TEST(test_batch_free_null);
    /* Evaluator */
    HU_RUN_TEST(test_evaluator_null_model);
    HU_RUN_TEST(test_evaluator_happy_path);
    /* Prepare */
    HU_RUN_TEST(test_prepare_token_bytes);
    HU_RUN_TEST(test_prepare_tokenize_file);
    HU_RUN_TEST(test_prepare_null_args);
    /* GPT model */
    HU_RUN_TEST(test_gpt_create_destroy);
    HU_RUN_TEST(test_gpt_create_null_args);
    HU_RUN_TEST(test_gpt_create_invalid_config);
    HU_RUN_TEST(test_gpt_forward);
    HU_RUN_TEST(test_gpt_forward_logits_finite);
    HU_RUN_TEST(test_gpt_get_params_stub);
    HU_RUN_TEST(test_gpt_backward_not_supported);
    /* Optimizer */
    HU_RUN_TEST(test_muon_adamw_create_destroy);
    HU_RUN_TEST(test_muon_adamw_step);
    HU_RUN_TEST(test_muon_adamw_step_direction);
    HU_RUN_TEST(test_muon_adamw_zero_grad);
    HU_RUN_TEST(test_muon_adamw_lr_multiplier);
    HU_RUN_TEST(test_muon_adamw_null_args);
    HU_RUN_TEST(test_muon_adamw_add_param_zero);
    HU_RUN_TEST(test_lr_schedule);
    /* Training pipeline */
    HU_RUN_TEST(test_train_pipeline);
    /* Experiment loop */
    HU_RUN_TEST(test_experiment_loop_null_config);
    HU_RUN_TEST(test_experiment_loop_zero_iterations);
    HU_RUN_TEST(test_experiment_loop_null_data_dir);
    HU_RUN_TEST(test_experiment_loop_runs);
    HU_RUN_TEST(test_experiment_loop_keep_discard);
    HU_RUN_TEST(test_experiment_loop_convergence);
    /* CLI */
    HU_RUN_TEST(test_ml_cli_train_help);
    HU_RUN_TEST(test_ml_cli_experiment_help);
    HU_RUN_TEST(test_ml_cli_prepare_help);
    HU_RUN_TEST(test_ml_cli_status);
}
