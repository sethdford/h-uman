/* Data preparation utilities for ML training. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/ml.h"
#include "human/ml/prepare.h"
#include "human/ml/tokenizer_ml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#endif

/* ─── hu_ml_prepare_tokenize_file ─────────────────────────────────────────── */

hu_error_t hu_ml_prepare_tokenize_file(hu_allocator_t *alloc, hu_bpe_tokenizer_t *tok,
                                      const char *input_path, const char *output_path)
{
    if (!alloc || !tok || !input_path || !output_path)
        return HU_ERR_INVALID_ARGUMENT;

#ifndef _WIN32
    FILE *in = fopen(input_path, "rb");
    if (!in)
        return HU_ERR_IO;

    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return HU_ERR_IO;
    }
    long sz = ftell(in);
    if (sz < 0) {
        fclose(in);
        return HU_ERR_IO;
    }
    rewind(in);

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(in);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t nread = fread(buf, 1, (size_t)sz, in);
    fclose(in);
    if (nread != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_IO;
    }
    buf[nread] = '\0';

    int32_t *ids = NULL;
    size_t ids_count = 0;
    hu_error_t err = hu_bpe_tokenizer_encode(tok, buf, nread, &ids, &ids_count);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    if (err != HU_OK)
        return err;

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        alloc->free(alloc->ctx, ids, ids_count * sizeof(int32_t));
        return HU_ERR_IO;
    }
    if (fwrite(ids, sizeof(int32_t), ids_count, out) != ids_count) {
        fclose(out);
        alloc->free(alloc->ctx, ids, ids_count * sizeof(int32_t));
        return HU_ERR_IO;
    }
    fclose(out);
    alloc->free(alloc->ctx, ids, ids_count * sizeof(int32_t));
    return HU_OK;
#else
    (void)alloc;
    (void)tok;
    (void)input_path;
    (void)output_path;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

/* ─── hu_ml_prepare_tokenize_dir ──────────────────────────────────────────── */

hu_error_t hu_ml_prepare_tokenize_dir(hu_allocator_t *alloc, hu_bpe_tokenizer_t *tok,
                                     const char *input_dir, const char *output_dir)
{
    if (!alloc || !tok || !input_dir || !output_dir)
        return HU_ERR_INVALID_ARGUMENT;

#ifndef _WIN32
    DIR *d = opendir(input_dir);
    if (!d)
        return HU_ERR_IO;

    hu_error_t last_err = HU_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        size_t nlen = strlen(e->d_name);
        if (nlen < 4 || strcmp(e->d_name + nlen - 4, ".txt") != 0)
            continue;

        char in_path[1024];
        char out_path[1024];
        char base[512];
        size_t base_len = nlen - 4;
        if (base_len >= sizeof(base))
            continue;
        memcpy(base, e->d_name, base_len);
        base[base_len] = '\0';

        int r1 = snprintf(in_path, sizeof(in_path), "%s/%s", input_dir, e->d_name);
        int r2 = snprintf(out_path, sizeof(out_path), "%s/%s.bin", output_dir, base);
        if (r1 < 0 || (size_t)r1 >= sizeof(in_path) ||
            r2 < 0 || (size_t)r2 >= sizeof(out_path))
            continue;

        hu_error_t err = hu_ml_prepare_tokenize_file(alloc, tok, in_path, out_path);
        if (err != HU_OK)
            last_err = err;
    }
    closedir(d);
    return last_err;
#else
    (void)alloc;
    (void)tok;
    (void)input_dir;
    (void)output_dir;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

/* ─── hu_ml_prepare_token_bytes ───────────────────────────────────────────── */

hu_error_t hu_ml_prepare_token_bytes(hu_allocator_t *alloc, hu_bpe_tokenizer_t *tok,
                                    int32_t **token_bytes_out, size_t *count)
{
    if (!alloc || !tok || !token_bytes_out || !count)
        return HU_ERR_INVALID_ARGUMENT;

    size_t vsz = hu_bpe_tokenizer_vocab_size(tok);
    int32_t *bytes = (int32_t *)alloc->alloc(alloc->ctx, vsz * sizeof(int32_t));
    if (!bytes)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < vsz; i++) {
        size_t len = hu_bpe_tokenizer_token_byte_length(tok, (int32_t)i);
        bytes[i] = (int32_t)(len > (size_t)2147483647 ? 2147483647 : len);
    }

    *token_bytes_out = bytes;
    *count = vsz;
    return HU_OK;
}

/* ─── hu_experiment_config_default ─────────────────────────────────────────── */

hu_experiment_config_t hu_experiment_config_default(void)
{
    hu_experiment_config_t c = {0};

    c.gpt.sequence_len = 2048;
    c.gpt.vocab_size = 8192;
    c.gpt.n_layer = 8;
    c.gpt.n_head = 4;
    c.gpt.n_kv_head = 4;
    c.gpt.n_embd = 512;
    c.gpt.head_dim = 128;
    memcpy(c.gpt.window_pattern, "SSSL", 5);
    c.gpt.activation = HU_ML_ACT_RELU_SQ;

    c.optimizer.embedding_lr = 0.6f;
    c.optimizer.unembedding_lr = 0.004f;
    c.optimizer.matrix_lr = 0.04f;
    c.optimizer.scalar_lr = 0.5f;
    c.optimizer.weight_decay = 0.2f;
    c.optimizer.adam_beta1 = 0.8f;
    c.optimizer.adam_beta2 = 0.95f;
    c.optimizer.warmup_ratio = 0.0f;
    c.optimizer.warmdown_ratio = 0.5f;
    c.optimizer.final_lr_frac = 0.0f;

    c.training.total_batch_size = 524288;
    c.training.device_batch_size = 128;
    c.training.time_budget_secs = 300;
    c.training.eval_tokens = 20971520;

    c.backend = HU_ML_BACKEND_CPU;

    return c;
}
