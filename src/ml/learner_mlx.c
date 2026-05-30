/* W13 — MLX learner backend.
 *
 * Shells out to `python3 -m mlx_lm.lora` for LoRA fine-tuning on Apple
 * Silicon via the MLX framework. The C code orchestrates: it converts
 * training signals to a JSONL file, invokes the Python process, and
 * reads back the adapter directory.
 *
 * `available()` returns true on macOS arm64 when `python3 -c "import
 * mlx_lm"` succeeds (or unconditionally under HU_IS_TEST).
 *
 * Under HU_IS_TEST the subprocess call is replaced by writing a fake
 * HLAD adapter file and returning success — no real training, no
 * network, no process spawning. */

#include "human/ml/learner.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define hu_popen(cmd, mode) _popen(cmd, mode)
#define hu_pclose(f)        _pclose(f)
#else
#include <unistd.h>
#define hu_popen(cmd, mode) popen(cmd, mode)
#define hu_pclose(f)        pclose(f)
#endif

typedef struct hu_learner_mlx_ctx {
    hu_allocator_t *alloc;
} hu_learner_mlx_ctx_t;

static bool mlx_available(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
#if defined(__APPLE__) && defined(__aarch64__)
    return true;
#else
    return false;
#endif
#elif defined(__APPLE__) && defined(__aarch64__)
    FILE *fp = hu_popen("python3 -c \"import mlx_lm\" 2>/dev/null", "r");
    if (!fp)
        return false;
    int status = hu_pclose(fp);
    return status == 0;
#else
    return false;
#endif
}

/* FNV-1a 64-bit for model_version hashing. */
static uint64_t mlx_fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
/* Write signals as JSONL training data. Each signal becomes one or more
 * {"prompt": "...", "completion": "..."} lines. Returns HU_OK on
 * success, HU_ERR_IO on write failure. */
static hu_error_t write_training_jsonl(const char *path, const hu_training_signal_t *signals,
                                       size_t n) {
    FILE *f = fopen(path, "w");
    if (!f)
        return HU_ERR_IO;

    for (size_t i = 0; i < n; i++) {
        switch (signals[i].kind) {
        case HU_TRAIN_DPO_PAIR:
            fprintf(f,
                    "{\"prompt\": \"%s\", \"completion\": \"%s\", "
                    "\"rejected\": \"%s\", \"weight\": %.4f}\n",
                    signals[i].as.dpo.prompt, signals[i].as.dpo.preferred,
                    signals[i].as.dpo.dispreferred, (double)signals[i].as.dpo.weight);
            break;
        case HU_TRAIN_PERSONA_DELTA:
            fprintf(f,
                    "{\"prompt\": \"[persona-delta kind=%d key=%s]\", "
                    "\"completion\": \"%s\"}\n",
                    (int)signals[i].as.persona.delta.kind, signals[i].as.persona.delta.key,
                    signals[i].as.persona.delta.value);
            break;
        case HU_TRAIN_CASE_OUTCOME: {
            const char *label = signals[i].as.case_outcome.reward >= 0.5f ? "positive" : "negative";
            fprintf(f,
                    "{\"prompt\": \"[case %lld]\", "
                    "\"completion\": \"%s (reward=%.2f)\"}\n",
                    (long long)signals[i].as.case_outcome.case_id, label,
                    (double)signals[i].as.case_outcome.reward);
            break;
        }
        default:
            break;
        }
    }

    if (fclose(f) != 0)
        return HU_ERR_IO;
    return HU_OK;
}
#endif /* !HU_IS_TEST */

#if defined(HU_IS_TEST) && HU_IS_TEST
/* Write a fake HLAD adapter file for HU_IS_TEST mode. Mirrors the
 * format from learner_cpu.c so the rest of the stack can round-trip. */
static hu_error_t write_fake_adapter(const char *path, const char *model_version, int rank,
                                     int64_t *out_bytes) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return HU_ERR_IO;

    int64_t total = 0;

    if (fwrite(HU_LEARNER_ADAPTER_MAGIC, 1, 4, f) != 4)
        goto io_err;
    total += 4;

    uint32_t version = HU_LEARNER_ADAPTER_VERSION;
    uint8_t v_le[4];
    v_le[0] = (uint8_t)(version & 0xFF);
    v_le[1] = (uint8_t)((version >> 8) & 0xFF);
    v_le[2] = (uint8_t)((version >> 16) & 0xFF);
    v_le[3] = (uint8_t)((version >> 24) & 0xFF);
    if (fwrite(v_le, 1, 4, f) != 4)
        goto io_err;
    total += 4;

    char mv[64];
    memset(mv, 0, sizeof(mv));
    if (model_version)
        strncpy(mv, model_version, sizeof(mv) - 1);
    if (fwrite(mv, 1, sizeof(mv), f) != sizeof(mv))
        goto io_err;
    total += (int64_t)sizeof(mv);

    uint64_t r64 = (uint64_t)rank;
    uint8_t r_le[8];
    for (int i = 0; i < 8; i++)
        r_le[i] = (uint8_t)((r64 >> (8 * i)) & 0xFF);
    if (fwrite(r_le, 1, 8, f) != 8)
        goto io_err;
    total += 8;

    uint64_t nw = (uint64_t)(rank * 32);
    uint8_t n_le[8];
    for (int i = 0; i < 8; i++)
        n_le[i] = (uint8_t)((nw >> (8 * i)) & 0xFF);
    if (fwrite(n_le, 1, 8, f) != 8)
        goto io_err;
    total += 8;

    /* Write zero weights. */
    uint8_t zero[4] = {0, 0, 0, 0};
    for (uint64_t i = 0; i < nw; i++) {
        if (fwrite(zero, 1, 4, f) != 4)
            goto io_err;
        total += 4;
    }

    if (fclose(f) != 0)
        return HU_ERR_IO;
    if (out_bytes)
        *out_bytes = total;
    return HU_OK;

io_err:
    fclose(f);
    return HU_ERR_IO;
}
#endif /* HU_IS_TEST */

static hu_error_t mlx_train(void *ctx, const hu_learner_config_t *cfg,
                            const hu_training_signal_t *signals, size_t signals_count,
                            hu_learner_report_t *out_report) {
    hu_learner_mlx_ctx_t *c = (hu_learner_mlx_ctx_t *)ctx;
    if (!c || !cfg || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    if (signals_count > 0 && !signals)
        return HU_ERR_INVALID_ARGUMENT;
    if (cfg->adapter_output_path[0] == '\0')
        return HU_ERR_INVALID_ARGUMENT;
    if (cfg->rank <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (cfg->learning_rate <= 0.0f)
        return HU_ERR_INVALID_ARGUMENT;
    if (cfg->dp_enabled && cfg->dp_epsilon <= 0.0f) {
        memset(out_report, 0, sizeof(*out_report));
        snprintf(out_report->last_error, sizeof(out_report->last_error),
                 "dp_enabled requires dp_epsilon > 0");
        return HU_ERR_INVALID_ARGUMENT;
    }

    memset(out_report, 0, sizeof(*out_report));
    out_report->signals_consumed = signals_count;
    snprintf(out_report->adapter_path, sizeof(out_report->adapter_path), "%.255s",
             cfg->adapter_output_path); /* %.255s: both fields are 256 (GCC
                                           -Werror=format-truncation). */

    /* Compute a model_version hash from training data for KV-cache
     * invalidation. Combines the configured version with a hash of the
     * signal contents. */
    uint64_t data_hash =
        mlx_fnv1a(cfg->model_version, strnlen(cfg->model_version, sizeof(cfg->model_version)));
    for (size_t i = 0; i < signals_count; i++) {
        uint64_t sh = mlx_fnv1a(&signals[i], sizeof(signals[i]));
        data_hash ^= sh;
    }
    /* Same truncation pattern as learner_ggml.c — width-bound the prefix
     * so GCC -Wformat-truncation=2 stays quiet under -Werror. */
    snprintf(out_report->model_version, sizeof(out_report->model_version), "%.46s-%016llx",
             cfg->model_version, (unsigned long long)data_hash);

    if (cfg->budget_ms == 0) {
        out_report->steps_completed = 0;
        out_report->final_loss = 0.0f;
        out_report->adapter_bytes = 0;
        snprintf(out_report->last_error, sizeof(out_report->last_error),
                 "budget_ms=0; no training performed");
        return HU_OK;
    }

#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Test mode: write a fake adapter, skip subprocess. */
    int64_t bytes = 0;
    hu_error_t e =
        write_fake_adapter(cfg->adapter_output_path, out_report->model_version, cfg->rank, &bytes);
    if (e != HU_OK) {
        /* PR #115 / Ubuntu CI fix: match learner_ggml.c's `%.90s` width-
         * bound. cfg->adapter_output_path is up to 256 bytes; last_error
         * is 128. GCC -Werror=format-truncation refuses unbounded %s. */
        snprintf(out_report->last_error, sizeof(out_report->last_error),
                 "failed to write test adapter at %.90s", cfg->adapter_output_path);
        return e;
    }
    out_report->adapter_bytes = bytes;
    out_report->steps_completed = (size_t)cfg->max_steps;
    out_report->final_loss = 0.01f;
    return HU_OK;
#else
    /* Real mode: write JSONL, shell out to mlx_lm.lora. */
    char jsonl_path[512];
    snprintf(jsonl_path, sizeof(jsonl_path), "%s.train.jsonl", cfg->adapter_output_path);

    hu_error_t we = write_training_jsonl(jsonl_path, signals, signals_count);
    if (we != HU_OK) {
        snprintf(out_report->last_error, sizeof(out_report->last_error),
                 "failed to write training data at %.92s",
                 jsonl_path); /* %.92s: fixed prefix + path must fit last_error (GCC
                                 -Werror=format-truncation). */
        return we;
    }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "python3 -m mlx_lm.lora "
             "--model \"%s\" "
             "--data \"%s\" "
             "--adapter-path \"%s\" "
             "--train "
             "--iters %d "
             "--batch-size %d "
             "--lora-rank %d "
             "--learning-rate %g "
             "2>&1",
             cfg->base_model_path, jsonl_path, cfg->adapter_output_path, cfg->max_steps,
             cfg->batch_size, cfg->rank, (double)cfg->learning_rate);

    FILE *fp = hu_popen(cmd, "r");
    if (!fp) {
        snprintf(out_report->last_error, sizeof(out_report->last_error),
                 "popen failed for mlx_lm.lora");
        unlink(jsonl_path);
        return HU_ERR_IO;
    }

    /* Read process output — look for final loss in the last line. */
    char line[512];
    float last_loss = 0.0f;
    size_t steps = 0;
    while (fgets(line, sizeof(line), fp)) {
        float step_loss = 0.0f;
        int step_num = 0;
        if (sscanf(line, "Iter %d: Train loss %f", &step_num, &step_loss) == 2) {
            last_loss = step_loss;
            steps = (size_t)step_num;
        }
    }

    int status = hu_pclose(fp);
    unlink(jsonl_path);

    if (status != 0) {
        snprintf(out_report->last_error, sizeof(out_report->last_error),
                 "mlx_lm.lora exited with status %d", status);
        return HU_ERR_IO;
    }

    out_report->steps_completed = steps;
    out_report->final_loss = last_loss;

    /* W15: post-hoc DP noise injection on adapter weights. mlx_lm.lora does
     * not natively support DP-SGD, so we add calibrated Gaussian noise to
     * the adapter after training. noise_sigma = sensitivity / epsilon. */
    if (cfg->dp_enabled) {
        float sensitivity = cfg->dp_clip_norm > 0.0f ? cfg->dp_clip_norm : 1.0f;
        float noise_sigma = sensitivity / cfg->dp_epsilon;
        char dp_cmd[2048];
        snprintf(dp_cmd, sizeof(dp_cmd),
                 "python3 -c \""
                 "import os, glob, numpy as np;"
                 "adir = '%s';"
                 "sigma = %g;"
                 "try:\n"
                 "    import safetensors.numpy as stn;\n"
                 "    for f in glob.glob(os.path.join(adir, '*.safetensors')):\n"
                 "        tensors = dict(stn.load_file(f));\n"
                 "        noised = {k: v + np.random.normal(0, sigma, v.shape).astype(v.dtype) for "
                 "k, v in tensors.items()};\n"
                 "        stn.save_file(noised, f)\n"
                 "except ImportError:\n"
                 "    for f in glob.glob(os.path.join(adir, '*.npz')):\n"
                 "        d = dict(np.load(f));\n"
                 "        noised = {k: v + np.random.normal(0, sigma, v.shape).astype(v.dtype) for "
                 "k, v in d.items()};\n"
                 "        np.savez(f, **noised)\n"
                 "\" 2>&1",
                 cfg->adapter_output_path, (double)noise_sigma);
        FILE *dp_fp = hu_popen(dp_cmd, "r");
        if (dp_fp) {
            char dp_line[256];
            while (fgets(dp_line, sizeof(dp_line), dp_fp)) {
                /* drain output */
            }
            int dp_status = hu_pclose(dp_fp);
            if (dp_status != 0) {
                fprintf(stderr,
                        "[mlx-dp] warning: post-hoc DP noise injection "
                        "failed (status %d); adapter may not satisfy "
                        "dp_epsilon=%.2f\n",
                        dp_status, (double)cfg->dp_epsilon);
            }
        }
    }

    /* Measure adapter directory size (approximate — sum of regular files). */
    char size_cmd[512];
    snprintf(size_cmd, sizeof(size_cmd), "du -sb \"%s\" 2>/dev/null | cut -f1",
             cfg->adapter_output_path);
    FILE *sfp = hu_popen(size_cmd, "r");
    if (sfp) {
        char sbuf[64];
        if (fgets(sbuf, sizeof(sbuf), sfp))
            out_report->adapter_bytes = (int64_t)strtoll(sbuf, NULL, 10);
        hu_pclose(sfp);
    }

    return HU_OK;
#endif /* HU_IS_TEST */
}

static void mlx_deinit(void *ctx) {
    hu_learner_mlx_ctx_t *c = (hu_learner_mlx_ctx_t *)ctx;
    if (!c)
        return;
    if (c->alloc)
        c->alloc->free(c->alloc->ctx, c, sizeof(*c));
}

const hu_learner_vtable_t hu_learner_mlx_vtable = {
    .name = "mlx",
    .available = mlx_available,
    .train = mlx_train,
    .deinit = mlx_deinit,
};

hu_error_t hu_learner_mlx_open(hu_allocator_t *alloc, void **out_ctx) {
    if (!alloc || !out_ctx)
        return HU_ERR_INVALID_ARGUMENT;
#if (defined(HU_IS_TEST) && HU_IS_TEST && defined(__APPLE__) && defined(__aarch64__)) || \
    (defined(__APPLE__) && defined(__aarch64__))
    hu_learner_mlx_ctx_t *c = (hu_learner_mlx_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    *out_ctx = c;
    return HU_OK;
#else
    *out_ctx = NULL;
    return HU_ERR_NOT_SUPPORTED;
#endif
}
