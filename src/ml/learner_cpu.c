/* W13 — CPU-fallback learner backend.
 *
 * Toy gradient-descent trainer that satisfies the test contract: it is
 * deterministic, slow, and never touches the network or `/dev/urandom`.
 * The math is intentionally simple — MSE between a tiny fp32 weight vector
 * and a per-signal target derived from the signal's content via FNV-1a
 * hashing. The point is not to learn anything useful; the point is to
 * produce a reproducible adapter file with the correct format so the rest
 * of the W13 stack (file IO, metadata propagation, KV-cache invalidation
 * on `model_version` swap) can be exercised end-to-end.
 *
 * Determinism contract (validated by test_w13_cpu_backend_trains_-
 * deterministic_adapter): same `cfg.seed`, same signal sequence,
 * same `cfg.model_version` ⇒ identical `final_loss` and identical adapter
 * file bytes. We use a splitmix64 PRNG for any random init or DP noise.
 *
 * The MLX/ggml backends are expected to wrap src/ml/lora.c (already a
 * deterministic reference implementation). Until that wiring lands, the
 * CPU backend is the only available trainer. */

#include "human/ml/dp_sgd.h"
#include "human/ml/learner.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Per-config working memory size: rank × 32 fp32 weights, capped to keep
 * adapters tiny. Rank=8 → 256 weights → 1 KiB on disk + 88 B header. */
#define HU_W13_WEIGHTS_PER_RANK 32u
#define HU_W13_MAX_RANK         128u
#define HU_W13_MAX_WEIGHTS      (HU_W13_MAX_RANK * HU_W13_WEIGHTS_PER_RANK)

typedef struct hu_learner_cpu_ctx {
    hu_allocator_t *alloc;
} hu_learner_cpu_ctx_t;

static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* uniform float in [-1, 1] from splitmix64 — deterministic. */
static float prng_uniform(uint64_t *state) {
    uint64_t r = splitmix64(state);
    /* 24 bits of mantissa, signed. */
    int32_t v = (int32_t)(r >> 40);
    return (float)v / (float)(1 << 23);
}

/* (Gaussian sampling formerly lived here as `prng_normal`. The DP-SGD path
 * now delegates all Gaussian noise to hu_dp_sgd_step / hu_dp_rng_normal in
 * src/ml/dp_sgd.c — the canonical translation unit. Pre-Sprint-42 inline
 * noise has been removed; see US-42.1 design (R-CURRENT-IS-PER-BATCH).) */

/* FNV-1a 64-bit. Used to project arbitrary text into a deterministic
 * floating-point target. */
static uint64_t fnv1a64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

/* Map FNV-1a output into a float in [-1, 1]. */
static float hash_to_unit(uint64_t h) {
    int32_t v = (int32_t)(h >> 32);
    return (float)v / (float)(1U << 31);
}

/* Build a per-weight target vector from the signals. Each signal contributes
 * to every weight slot via FNV(signal_content, weight_index). The mean target
 * is what SGD pulls the weights toward. */
static void compute_targets(const hu_training_signal_t *signals, size_t n, float *targets,
                            size_t n_weights) {
    if (n == 0) {
        for (size_t i = 0; i < n_weights; i++)
            targets[i] = 0.0f;
        return;
    }
    for (size_t w = 0; w < n_weights; w++) {
        double acc = 0.0;
        for (size_t i = 0; i < n; i++) {
            uint64_t base;
            switch (signals[i].kind) {
            case HU_TRAIN_DPO_PAIR: {
                uint64_t hp = fnv1a64(
                    signals[i].as.dpo.preferred,
                    strnlen(signals[i].as.dpo.preferred, sizeof(signals[i].as.dpo.preferred)));
                uint64_t hd = fnv1a64(signals[i].as.dpo.dispreferred,
                                      strnlen(signals[i].as.dpo.dispreferred,
                                              sizeof(signals[i].as.dpo.dispreferred)));
                /* DPO target: pull toward (preferred - dispreferred). */
                float fp = hash_to_unit(hp ^ ((uint64_t)w * 0x9E3779B97F4A7C15ULL));
                float fd = hash_to_unit(hd ^ ((uint64_t)w * 0x9E3779B97F4A7C15ULL));
                float weight = signals[i].as.dpo.weight;
                if (weight <= 0.0f)
                    weight = 1.0f;
                acc += (double)((fp - fd) * weight);
                continue;
            }
            case HU_TRAIN_PERSONA_DELTA:
                base = fnv1a64(signals[i].as.persona.delta.value,
                               strnlen(signals[i].as.persona.delta.value,
                                       sizeof(signals[i].as.persona.delta.value)));
                acc += (double)hash_to_unit(base ^ ((uint64_t)w * 0x9E3779B97F4A7C15ULL));
                continue;
            case HU_TRAIN_CASE_OUTCOME: {
                uint64_t cid = (uint64_t)signals[i].as.case_outcome.case_id;
                base = fnv1a64(&cid, sizeof(cid));
                /* Reward in [0,1] → contribution scaled to [-1, 1]. */
                float r = signals[i].as.case_outcome.reward * 2.0f - 1.0f;
                acc += (double)(hash_to_unit(base ^ ((uint64_t)w * 0x9E3779B97F4A7C15ULL)) * r);
                continue;
            }
            default:
                continue;
            }
        }
        targets[w] = (float)(acc / (double)n);
    }
}

/* Per-signal target: the contribution of ONE signal to the per-weight target.
 * This is required for the DP-SGD per-sample gradient path — true DP-SGD
 * needs each sample's gradient to be clipped independently to clip_norm
 * BEFORE summing. The previous implementation clipped one summed gradient
 * across all signals, which is per-BATCH clipping, not per-sample. */
static void compute_single_target(const hu_training_signal_t *s, float *targets, size_t n_weights) {
    for (size_t w = 0; w < n_weights; w++) {
        uint64_t base;
        double val = 0.0;
        switch (s->kind) {
        case HU_TRAIN_DPO_PAIR: {
            uint64_t hp = fnv1a64(s->as.dpo.preferred,
                                  strnlen(s->as.dpo.preferred, sizeof(s->as.dpo.preferred)));
            uint64_t hd = fnv1a64(s->as.dpo.dispreferred,
                                  strnlen(s->as.dpo.dispreferred, sizeof(s->as.dpo.dispreferred)));
            float fp = hash_to_unit(hp ^ ((uint64_t)w * 0x9E3779B97F4A7C15ULL));
            float fd = hash_to_unit(hd ^ ((uint64_t)w * 0x9E3779B97F4A7C15ULL));
            float weight = s->as.dpo.weight;
            if (weight <= 0.0f)
                weight = 1.0f;
            val = (double)((fp - fd) * weight);
            break;
        }
        case HU_TRAIN_PERSONA_DELTA:
            base = fnv1a64(s->as.persona.delta.value,
                           strnlen(s->as.persona.delta.value, sizeof(s->as.persona.delta.value)));
            val = (double)hash_to_unit(base ^ ((uint64_t)w * 0x9E3779B97F4A7C15ULL));
            break;
        case HU_TRAIN_CASE_OUTCOME: {
            uint64_t cid = (uint64_t)s->as.case_outcome.case_id;
            base = fnv1a64(&cid, sizeof(cid));
            float r = s->as.case_outcome.reward * 2.0f - 1.0f;
            val = (double)(hash_to_unit(base ^ ((uint64_t)w * 0x9E3779B97F4A7C15ULL)) * r);
            break;
        }
        default:
            val = 0.0;
            break;
        }
        targets[w] = (float)val;
    }
}

static int64_t now_ms_monotonic(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static hu_error_t write_adapter_file(const char *path, const char *model_version, uint32_t rank,
                                     const float *weights, size_t n_weights, int64_t *out_bytes) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return HU_ERR_IO;

    int64_t total = 0;

    /* Magic */
    if (fwrite(HU_LEARNER_ADAPTER_MAGIC, 1, 4, f) != 4)
        goto io_err;
    total += 4;

    /* Version (LE). */
    uint32_t version = HU_LEARNER_ADAPTER_VERSION;
    uint8_t v_le[4];
    v_le[0] = (uint8_t)(version & 0xFF);
    v_le[1] = (uint8_t)((version >> 8) & 0xFF);
    v_le[2] = (uint8_t)((version >> 16) & 0xFF);
    v_le[3] = (uint8_t)((version >> 24) & 0xFF);
    if (fwrite(v_le, 1, 4, f) != 4)
        goto io_err;
    total += 4;

    /* Model version (64 bytes, NUL-padded). */
    char mv[64];
    memset(mv, 0, sizeof(mv));
    if (model_version)
        strncpy(mv, model_version, sizeof(mv) - 1);
    if (fwrite(mv, 1, sizeof(mv), f) != sizeof(mv))
        goto io_err;
    total += (int64_t)sizeof(mv);

    /* Rank (u64 LE). */
    uint64_t r64 = rank;
    uint8_t r_le[8];
    for (int i = 0; i < 8; i++)
        r_le[i] = (uint8_t)((r64 >> (8 * i)) & 0xFF);
    if (fwrite(r_le, 1, 8, f) != 8)
        goto io_err;
    total += 8;

    /* num_weights (u64 LE). */
    uint64_t nw = (uint64_t)n_weights;
    uint8_t n_le[8];
    for (int i = 0; i < 8; i++)
        n_le[i] = (uint8_t)((nw >> (8 * i)) & 0xFF);
    if (fwrite(n_le, 1, 8, f) != 8)
        goto io_err;
    total += 8;

    /* Weights as little-endian fp32. We assume host is LE; if not, swap. */
    for (size_t i = 0; i < n_weights; i++) {
        union {
            float f;
            uint32_t u;
        } u;
        u.f = weights[i];
        uint8_t buf[4];
        buf[0] = (uint8_t)(u.u & 0xFF);
        buf[1] = (uint8_t)((u.u >> 8) & 0xFF);
        buf[2] = (uint8_t)((u.u >> 16) & 0xFF);
        buf[3] = (uint8_t)((u.u >> 24) & 0xFF);
        if (fwrite(buf, 1, 4, f) != 4)
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

static bool cpu_available(void) {
    return true;
}

static void cpu_deinit(void *ctx) {
    hu_learner_cpu_ctx_t *c = (hu_learner_cpu_ctx_t *)ctx;
    if (!c)
        return;
    if (c->alloc)
        c->alloc->free(c->alloc->ctx, c, sizeof(*c));
}

static hu_error_t cpu_train(void *ctx, const hu_learner_config_t *cfg,
                            const hu_training_signal_t *signals, size_t signals_count,
                            hu_learner_report_t *out_report) {
    hu_learner_cpu_ctx_t *c = (hu_learner_cpu_ctx_t *)ctx;
    if (!c || !cfg || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    if (signals_count > 0 && !signals)
        return HU_ERR_INVALID_ARGUMENT;
    if (cfg->adapter_output_path[0] == '\0')
        return HU_ERR_INVALID_ARGUMENT;
    if (cfg->rank <= 0 || (uint32_t)cfg->rank > HU_W13_MAX_RANK)
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
    snprintf(out_report->adapter_path, sizeof(out_report->adapter_path), "%s",
             cfg->adapter_output_path);
    snprintf(out_report->model_version, sizeof(out_report->model_version), "%s",
             cfg->model_version);

    /* Budget short-circuit: callers passing budget_ms=0 want a no-op. */
    if (cfg->budget_ms == 0) {
        out_report->steps_completed = 0;
        out_report->final_loss = 0.0f;
        out_report->adapter_bytes = 0;
        snprintf(out_report->last_error, sizeof(out_report->last_error),
                 "budget_ms=0; no training performed");
        return HU_OK;
    }

    /* Working set sizing — small, deterministic, capped. */
    size_t n_weights = (size_t)cfg->rank * HU_W13_WEIGHTS_PER_RANK;
    if (n_weights > HU_W13_MAX_WEIGHTS)
        n_weights = HU_W13_MAX_WEIGHTS;

    float *weights = (float *)c->alloc->alloc(c->alloc->ctx, n_weights * sizeof(float));
    if (!weights)
        return HU_ERR_OUT_OF_MEMORY;
    float *targets = (float *)c->alloc->alloc(c->alloc->ctx, n_weights * sizeof(float));
    if (!targets) {
        c->alloc->free(c->alloc->ctx, weights, n_weights * sizeof(float));
        return HU_ERR_OUT_OF_MEMORY;
    }

    /* Seeded init — same seed → byte-identical weights. seed=0 yields a
     * stable canonical starting point. */
    uint64_t prng = cfg->seed ? cfg->seed : 0xD1B54A32D192ED03ULL;
    /* Salt the seed with model_version so two adapters with different
     * model_version are not interchangeable. */
    uint64_t mv_hash =
        fnv1a64(cfg->model_version, strnlen(cfg->model_version, sizeof(cfg->model_version)));
    prng ^= mv_hash;

    for (size_t i = 0; i < n_weights; i++)
        weights[i] = 0.05f * prng_uniform(&prng);

    compute_targets(signals, signals_count, targets, n_weights);

    /* SGD loop. Each step: w_i -= lr * 2 * (w_i - target_i).
     *
     * DP-SGD path: per-sample gradients (one row per signal) are clipped
     * INDEPENDENTLY to clip_norm BEFORE summation — the only way to bound
     * any one sample's contribution to the released sum. The canonical
     * implementation lives in src/ml/dp_sgd.c (hu_dp_sgd_step). This
     * backend MUST delegate; it MUST NOT compute its own grad_norm or
     * noise locally. Pinned by AC-42.1.4 call-counter and by grep at
     * review time. */
    int max_steps = cfg->max_steps > 0 ? cfg->max_steps : 1;
    float lr = cfg->learning_rate;
    double clip_norm = cfg->dp_clip_norm > 0.0f ? (double)cfg->dp_clip_norm : 1.0;
    int64_t deadline = -1;
    if (cfg->budget_ms > 0)
        deadline = now_ms_monotonic() + cfg->budget_ms;

    /* For the DP path we need a row-per-sample buffer. We treat each
     * signal as one "sample"; if there are no signals we synthesize one
     * dummy sample so the per-sample-clip contract is still satisfied. */
    size_t n_samples = signals_count > 0 ? signals_count : 1;
    float *per_sample_targets = NULL;
    float *per_sample_grads = NULL;
    float *avg_grad = NULL;
    hu_dp_rng_t dp_rng;
    double noise_multiplier = 0.0;
    if (cfg->dp_enabled) {
        per_sample_targets =
            (float *)c->alloc->alloc(c->alloc->ctx, n_samples * n_weights * sizeof(float));
        per_sample_grads =
            (float *)c->alloc->alloc(c->alloc->ctx, n_samples * n_weights * sizeof(float));
        avg_grad = (float *)c->alloc->alloc(c->alloc->ctx, n_weights * sizeof(float));
        if (!per_sample_targets || !per_sample_grads || !avg_grad) {
            if (per_sample_targets)
                c->alloc->free(c->alloc->ctx, per_sample_targets,
                               n_samples * n_weights * sizeof(float));
            if (per_sample_grads)
                c->alloc->free(c->alloc->ctx, per_sample_grads,
                               n_samples * n_weights * sizeof(float));
            if (avg_grad)
                c->alloc->free(c->alloc->ctx, avg_grad, n_weights * sizeof(float));
            c->alloc->free(c->alloc->ctx, weights, n_weights * sizeof(float));
            c->alloc->free(c->alloc->ctx, targets, n_weights * sizeof(float));
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t s = 0; s < n_samples; s++) {
            float *row = per_sample_targets + s * n_weights;
            if (signals_count > 0) {
                compute_single_target(&signals[s], row, n_weights);
            } else {
                for (size_t w = 0; w < n_weights; w++)
                    row[w] = 0.0f;
            }
        }
        hu_dp_rng_seed(&dp_rng, prng);
        /* Translate the legacy (epsilon-per-step) config into a noise
         * multiplier. The legacy formula was sigma = clip / epsilon; the
         * canonical formula is sigma = noise_multiplier * clip. Therefore
         * noise_multiplier = 1 / epsilon. */
        noise_multiplier = 1.0 / (double)cfg->dp_epsilon;
    }

    int steps = 0;
    double loss = 0.0;
    for (steps = 0; steps < max_steps; steps++) {
        loss = 0.0;
        if (cfg->dp_enabled) {
            /* Per-sample gradients: each row is 2*(weights - target_s). */
            for (size_t s = 0; s < n_samples; s++) {
                const float *t_row = per_sample_targets + s * n_weights;
                float *g_row = per_sample_grads + s * n_weights;
                for (size_t i = 0; i < n_weights; i++) {
                    float diff = weights[i] - t_row[i];
                    g_row[i] = 2.0f * diff;
                    loss += (double)(diff * diff);
                }
            }
            loss /= (double)(n_samples * n_weights);

            /* Delegate clip+noise+average to the canonical implementation. */
            hu_error_t cse = hu_dp_sgd_step(per_sample_grads, n_samples, n_weights, clip_norm,
                                            noise_multiplier, &dp_rng, avg_grad);
            if (cse != HU_OK) {
                c->alloc->free(c->alloc->ctx, per_sample_targets,
                               n_samples * n_weights * sizeof(float));
                c->alloc->free(c->alloc->ctx, per_sample_grads,
                               n_samples * n_weights * sizeof(float));
                c->alloc->free(c->alloc->ctx, avg_grad, n_weights * sizeof(float));
                c->alloc->free(c->alloc->ctx, weights, n_weights * sizeof(float));
                c->alloc->free(c->alloc->ctx, targets, n_weights * sizeof(float));
                return cse;
            }
            for (size_t i = 0; i < n_weights; i++) {
                weights[i] -= lr * avg_grad[i];
            }
        } else {
            for (size_t i = 0; i < n_weights; i++) {
                float diff = weights[i] - targets[i];
                loss += (double)(diff * diff);
                float grad = 2.0f * diff;
                weights[i] -= lr * grad;
            }
            loss /= (double)n_weights;
        }
        if (deadline > 0 && now_ms_monotonic() >= deadline)
            break;
    }

    if (cfg->dp_enabled) {
        c->alloc->free(c->alloc->ctx, per_sample_targets, n_samples * n_weights * sizeof(float));
        c->alloc->free(c->alloc->ctx, per_sample_grads, n_samples * n_weights * sizeof(float));
        c->alloc->free(c->alloc->ctx, avg_grad, n_weights * sizeof(float));
    }
    /* Loop variable `steps` may have been incremented past completion;
     * cap at max_steps for the report. */
    if (steps > max_steps)
        steps = max_steps;
    out_report->steps_completed = (size_t)steps;
    out_report->final_loss = (float)loss;

    int64_t bytes = 0;
    hu_error_t e = write_adapter_file(cfg->adapter_output_path, cfg->model_version,
                                      (uint32_t)cfg->rank, weights, n_weights, &bytes);
    if (e != HU_OK) {
        /* adapter_output_path is up to 256 bytes; last_error is 128. */
        snprintf(out_report->last_error, sizeof(out_report->last_error),
                 "failed to write adapter at %.90s", cfg->adapter_output_path);
    } else {
        out_report->adapter_bytes = bytes;
    }

    c->alloc->free(c->alloc->ctx, weights, n_weights * sizeof(float));
    c->alloc->free(c->alloc->ctx, targets, n_weights * sizeof(float));
    return e;
}

const hu_learner_vtable_t hu_learner_cpu_vtable = {
    .name = "cpu",
    .available = cpu_available,
    .train = cpu_train,
    .deinit = cpu_deinit,
};

hu_error_t hu_learner_cpu_open(hu_allocator_t *alloc, void **out_ctx) {
    if (!alloc || !out_ctx)
        return HU_ERR_INVALID_ARGUMENT;
    hu_learner_cpu_ctx_t *c = (hu_learner_cpu_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    *out_ctx = c;
    return HU_OK;
}
