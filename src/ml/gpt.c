/* GPT model — CPU reference implementation for human ML subsystem.
 * Architecture from autoresearch train.py. Float32 only, no flash attention.
 * Phase 2 will add ggml backend for GPU acceleration. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ─── Internal struct ────────────────────────────────────────────────────── */

typedef struct hu_gpt {
    hu_allocator_t *alloc;
    hu_gpt_config_t config;

    float *wte;
    float *lm_head;

    float **attn_q_w;
    float **attn_k_w;
    float **attn_v_w;
    float **attn_o_w;
    float **mlp_up_w;
    float **mlp_down_w;

    float *resid_lambdas;
    float *x0_lambdas;

    float *rope_cos;
    float *rope_sin;

    size_t total_params;
} hu_gpt_t;

/* ─── PRNG for weight init ───────────────────────────────────────────────── */

static float prng_next(uint64_t *seed)
{
    *seed = *seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((float)(*seed >> 33) / (float)(1ULL << 31)) - 0.5f;
}

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static float relu_sq(float x)
{
    float r = (x > 0.0f) ? x : 0.0f;
    return r * r;
}

static void softmax(float *x, int n)
{
    float max_val = x[0];
    for (int i = 1; i < n; i++)
        if (x[i] > max_val)
            max_val = x[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }
    for (int i = 0; i < n; i++)
        x[i] /= sum;
}

/* matmul_atb: out = A @ B^T, A is m×k, B is n×k, out is m×n */
static void matmul_atb(float *out, const float *a, const float *b, int m, int n, int k)
{
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            float s = 0.0f;
            for (int t = 0; t < k; t++)
                s += a[i * k + t] * b[j * k + t];
            out[i * n + j] = s;
        }
    }
}

#define RMS_EPS 1e-6f

static void rms_norm(float *out, const float *x, size_t n)
{
    float sum_sq = 0.0f;
    for (size_t i = 0; i < n; i++)
        sum_sq += x[i] * x[i];
    float rms = sqrtf(sum_sq / (float)n + RMS_EPS);
    for (size_t i = 0; i < n; i++)
        out[i] = x[i] / rms;
}

/* Apply RoPE to Q and K. q,k are [n_head, seq_len, head_dim]. cos,sin [seq_len, head_dim/2]. */
static void apply_rope(float *q, float *k, const float *cos, const float *sin,
                      int n_head, int n_kv_head, int head_dim, int seq_len)
{
    int half = head_dim / 2;
    for (int h = 0; h < n_head; h++) {
        int kv_h = h % n_kv_head;
        for (int s = 0; s < seq_len; s++) {
            const float *c = cos + s * half;
            const float *sn = sin + s * half;
            float *qh = q + h * seq_len * head_dim + s * head_dim;
            float *kh = k + kv_h * seq_len * head_dim + s * head_dim;
            for (int d = 0; d < half; d++) {
                float q0 = qh[d], q1 = qh[d + half];
                float k0 = kh[d], k1 = kh[d + half];
                qh[d] = q0 * c[d] - q1 * sn[d];
                qh[d + half] = q0 * sn[d] + q1 * c[d];
                kh[d] = k0 * c[d] - k1 * sn[d];
                kh[d + half] = k0 * sn[d] + k1 * c[d];
            }
        }
    }
}

/* ─── Forward pass ───────────────────────────────────────────────────────── */

static hu_error_t gpt_forward(void *ctx, const hu_ml_tensor_t *input,
                             hu_ml_tensor_t *output)
{
    hu_gpt_t *g = (hu_gpt_t *)ctx;
    const hu_gpt_config_t *cfg = &g->config;

    if (!input || !output || input->ndim != 2 || input->dtype != HU_ML_DTYPE_I32)
        return HU_ERR_INVALID_ARGUMENT;

    size_t batch_size = input->shape[0];
    size_t seq_len = input->shape[1];
    if (seq_len > cfg->sequence_len)
        return HU_ERR_INVALID_ARGUMENT;

    const int32_t *ids = (const int32_t *)input->data;
    size_t n_embd = cfg->n_embd;
    size_t vocab_size = cfg->vocab_size;
    size_t n_layer = cfg->n_layer;
    size_t n_head = cfg->n_head;
    size_t n_kv_head = cfg->n_kv_head;
    size_t head_dim = cfg->head_dim;
    size_t n_mlp = 4 * n_embd;

    size_t B = batch_size, S = seq_len, E = n_embd, V = vocab_size;

    /* Allocate workspace: x, x0, x_norm, q,k,v,attn_out, up, mlp_out */
    hu_allocator_t *alloc = g->alloc;
    size_t x_size = B * S * E * sizeof(float);
    size_t x0_size = B * S * E * sizeof(float);
    size_t x_norm_size = B * S * E * sizeof(float);
    size_t q_size = B * n_head * S * head_dim * sizeof(float);
    size_t k_size = B * n_kv_head * S * head_dim * sizeof(float);
    size_t v_size = B * n_kv_head * S * head_dim * sizeof(float);
    size_t attn_out_size = B * S * E * sizeof(float);
    size_t up_size = B * S * n_mlp * sizeof(float);
    size_t mlp_out_size = B * S * E * sizeof(float);

    float *x = (float *)alloc->alloc(alloc->ctx, x_size);
    float *x0 = (float *)alloc->alloc(alloc->ctx, x0_size);
    float *x_norm = (float *)alloc->alloc(alloc->ctx, x_norm_size);
    float *q = (float *)alloc->alloc(alloc->ctx, q_size);
    float *k = (float *)alloc->alloc(alloc->ctx, k_size);
    float *v = (float *)alloc->alloc(alloc->ctx, v_size);
    float *attn_out = (float *)alloc->alloc(alloc->ctx, attn_out_size);
    float *up = (float *)alloc->alloc(alloc->ctx, up_size);
    float *mlp_out = (float *)alloc->alloc(alloc->ctx, mlp_out_size);
    if (!x || !x0 || !x_norm || !q || !k || !v || !attn_out || !up || !mlp_out) {
        if (x) alloc->free(alloc->ctx, x, x_size);
        if (x0) alloc->free(alloc->ctx, x0, x0_size);
        if (x_norm) alloc->free(alloc->ctx, x_norm, x_norm_size);
        if (q) alloc->free(alloc->ctx, q, q_size);
        if (k) alloc->free(alloc->ctx, k, k_size);
        if (v) alloc->free(alloc->ctx, v, v_size);
        if (attn_out) alloc->free(alloc->ctx, attn_out, attn_out_size);
        if (up) alloc->free(alloc->ctx, up, up_size);
        if (mlp_out) alloc->free(alloc->ctx, mlp_out, mlp_out_size);
        return HU_ERR_OUT_OF_MEMORY;
    }

    /* 1. Token embedding lookup */
    for (size_t b = 0; b < B; b++) {
        for (size_t s = 0; s < S; s++) {
            int32_t tid = ids[b * S + s];
            if (tid < 0 || (size_t)tid >= vocab_size)
                tid = 0;
            memcpy(x + (b * S + s) * E, g->wte + (size_t)tid * E, E * sizeof(float));
        }
    }

    /* 2. RMS norm x → x0 */
    for (size_t i = 0; i < B * S; i++)
        rms_norm(x0 + i * E, x + i * E, E);

    /* 3. Transformer blocks */
    for (size_t L = 0; L < n_layer; L++) {
        float rl = g->resid_lambdas[L];
        float x0l = g->x0_lambdas[L];

        /* a. x = resid_lambda * x + x0_lambda * x0 */
        for (size_t i = 0; i < B * S * E; i++)
            x[i] = rl * x[i] + x0l * x0[i];

        /* b. RMS norm x → x_norm */
        for (size_t i = 0; i < B * S; i++)
            rms_norm(x_norm + i * E, x + i * E, E);

        /* c. Q, K, V = x_norm @ W_q, W_k, W_v (weights stored as out_dim × in_dim) */
        matmul_atb(q, x_norm, g->attn_q_w[L], (int)(B * S), (int)(n_head * head_dim), (int)E);
        matmul_atb(k, x_norm, g->attn_k_w[L], (int)(B * S), (int)(n_kv_head * head_dim), (int)E);
        matmul_atb(v, x_norm, g->attn_v_w[L], (int)(B * S), (int)(n_kv_head * head_dim), (int)E);

        /* d. Apply RoPE to Q, K */
        for (size_t b = 0; b < B; b++) {
            apply_rope(q + b * n_head * S * head_dim,
                       k + b * n_kv_head * S * head_dim,
                       g->rope_cos, g->rope_sin,
                       (int)n_head, (int)n_kv_head, (int)head_dim, (int)S);
        }

        /* e. Scaled dot-product attention with causal mask */
        float scale = 1.0f / sqrtf((float)head_dim);
        memset(attn_out, 0, attn_out_size);
        for (size_t b = 0; b < B; b++) {
            for (size_t h = 0; h < n_head; h++) {
                int kv_h = (int)(h % n_kv_head);
                for (size_t i = 0; i < S; i++) {
                    float *scores = (float *)alloc->alloc(alloc->ctx, S * sizeof(float));
                    if (!scores) {
                        alloc->free(alloc->ctx, x, x_size);
                        alloc->free(alloc->ctx, x0, x0_size);
                        alloc->free(alloc->ctx, x_norm, x_norm_size);
                        alloc->free(alloc->ctx, q, q_size);
                        alloc->free(alloc->ctx, k, k_size);
                        alloc->free(alloc->ctx, v, v_size);
                        alloc->free(alloc->ctx, attn_out, attn_out_size);
                        alloc->free(alloc->ctx, up, up_size);
                        alloc->free(alloc->ctx, mlp_out, mlp_out_size);
                        return HU_ERR_OUT_OF_MEMORY;
                    }
                    for (size_t j = 0; j < S; j++) {
                        float dot = 0.0f;
                        const float *qi = q + (b * n_head + h) * S * head_dim + i * head_dim;
                        const float *kj = k + (b * n_kv_head + kv_h) * S * head_dim + j * head_dim;
                        for (size_t d = 0; d < head_dim; d++)
                            dot += qi[d] * kj[d];
                        scores[j] = (j <= i) ? (dot * scale) : -1e9f;
                    }
                    softmax(scores, (int)S);
                    float *out_h = attn_out + (b * S + i) * E + h * head_dim;
                    for (size_t j = 0; j < S; j++) {
                        const float *vj = v + (b * n_kv_head + kv_h) * S * head_dim + j * head_dim;
                        for (size_t d = 0; d < head_dim; d++)
                            out_h[d] += scores[j] * vj[d];
                    }
                    alloc->free(alloc->ctx, scores, S * sizeof(float));
                }
            }
        }

        /* f. Output projection */
        float *proj = (float *)alloc->alloc(alloc->ctx, B * S * E * sizeof(float));
        if (!proj) {
            alloc->free(alloc->ctx, x, x_size);
            alloc->free(alloc->ctx, x0, x0_size);
            alloc->free(alloc->ctx, x_norm, x_norm_size);
            alloc->free(alloc->ctx, q, q_size);
            alloc->free(alloc->ctx, k, k_size);
            alloc->free(alloc->ctx, v, v_size);
            alloc->free(alloc->ctx, attn_out, attn_out_size);
            alloc->free(alloc->ctx, up, up_size);
            alloc->free(alloc->ctx, mlp_out, mlp_out_size);
            return HU_ERR_OUT_OF_MEMORY;
        }
        matmul_atb(proj, attn_out, g->attn_o_w[L], (int)(B * S), (int)E, (int)(n_head * head_dim));
        memcpy(attn_out, proj, B * S * E * sizeof(float));
        alloc->free(alloc->ctx, proj, B * S * E * sizeof(float));

        /* g. Residual: x = x + attn_out */
        for (size_t i = 0; i < B * S * E; i++)
            x[i] += attn_out[i];

        /* h. RMS norm x → x_norm */
        for (size_t i = 0; i < B * S; i++)
            rms_norm(x_norm + i * E, x + i * E, E);

        /* i. MLP: up = x_norm @ W_up, activation, down = up @ W_down */
        matmul_atb(up, x_norm, g->mlp_up_w[L], (int)(B * S), (int)n_mlp, (int)E);
        for (size_t i = 0; i < B * S * n_mlp; i++)
            up[i] = relu_sq(up[i]);
        matmul_atb(mlp_out, up, g->mlp_down_w[L], (int)(B * S), (int)E, (int)n_mlp);

        /* j. Residual: x = x + mlp_out */
        for (size_t i = 0; i < B * S * E; i++)
            x[i] += mlp_out[i];
    }

    /* 4. RMS norm x */
    for (size_t i = 0; i < B * S; i++)
        rms_norm(x + i * E, x + i * E, E);

    /* 5. Logits = x @ lm_head.T */
    size_t logits_size = B * S * V * sizeof(float);
    float *logits = (float *)alloc->alloc(alloc->ctx, logits_size);
    if (!logits) {
        alloc->free(alloc->ctx, x, x_size);
        alloc->free(alloc->ctx, x0, x0_size);
        alloc->free(alloc->ctx, x_norm, x_norm_size);
        alloc->free(alloc->ctx, q, q_size);
        alloc->free(alloc->ctx, k, k_size);
        alloc->free(alloc->ctx, v, v_size);
        alloc->free(alloc->ctx, attn_out, attn_out_size);
        alloc->free(alloc->ctx, up, up_size);
        alloc->free(alloc->ctx, mlp_out, mlp_out_size);
        return HU_ERR_OUT_OF_MEMORY;
    }
    matmul_atb(logits, x, g->lm_head, (int)(B * S), (int)V, (int)E);

    /* 6. Soft-cap: logits = 15 * tanh(logits / 15) */
    for (size_t i = 0; i < B * S * V; i++)
        logits[i] = 15.0f * tanhf(logits[i] / 15.0f);

    /* Fill output tensor */
    output->data = logits;
    output->shape[0] = batch_size;
    output->shape[1] = seq_len;
    output->shape[2] = vocab_size;
    output->ndim = 3;
    output->dtype = HU_ML_DTYPE_F32;
    output->size_bytes = logits_size;

    alloc->free(alloc->ctx, x, x_size);
    alloc->free(alloc->ctx, x0, x0_size);
    alloc->free(alloc->ctx, x_norm, x_norm_size);
    alloc->free(alloc->ctx, q, q_size);
    alloc->free(alloc->ctx, k, k_size);
    alloc->free(alloc->ctx, v, v_size);
    alloc->free(alloc->ctx, attn_out, attn_out_size);
    alloc->free(alloc->ctx, up, up_size);
    alloc->free(alloc->ctx, mlp_out, mlp_out_size);

    return HU_OK;
}

/* Caller must free output->data when done (allocated by forward). */
static hu_error_t gpt_backward(void *ctx, const hu_ml_tensor_t *grad_output)
{
    (void)ctx;
    (void)grad_output;
    return HU_ERR_NOT_SUPPORTED;
}

static hu_error_t gpt_get_params(void *ctx, hu_ml_tensor_t **params, size_t *count)
{
    (void)ctx;
    (void)params;
    (void)count;
    return HU_ERR_NOT_SUPPORTED;
}

static size_t gpt_num_params(void *ctx)
{
    return ((hu_gpt_t *)ctx)->total_params;
}

static void gpt_deinit(void *ctx, hu_allocator_t *alloc)
{
    hu_gpt_t *g = (hu_gpt_t *)ctx;
    if (!g)
        return;

    if (g->wte)
        alloc->free(alloc->ctx, g->wte, g->config.vocab_size * g->config.n_embd * sizeof(float));
    if (g->lm_head)
        alloc->free(alloc->ctx, g->lm_head, g->config.vocab_size * g->config.n_embd * sizeof(float));

    for (size_t i = 0; i < g->config.n_layer; i++) {
        size_t q_sz = g->config.n_head * g->config.head_dim * g->config.n_embd * sizeof(float);
        size_t kv_sz = g->config.n_kv_head * g->config.head_dim * g->config.n_embd * sizeof(float);
        size_t o_sz = g->config.n_embd * g->config.n_head * g->config.head_dim * sizeof(float);
        size_t up_sz = 4 * g->config.n_embd * g->config.n_embd * sizeof(float);
        size_t down_sz = g->config.n_embd * 4 * g->config.n_embd * sizeof(float);
        if (g->attn_q_w[i]) alloc->free(alloc->ctx, g->attn_q_w[i], q_sz);
        if (g->attn_k_w[i]) alloc->free(alloc->ctx, g->attn_k_w[i], kv_sz);
        if (g->attn_v_w[i]) alloc->free(alloc->ctx, g->attn_v_w[i], kv_sz);
        if (g->attn_o_w[i]) alloc->free(alloc->ctx, g->attn_o_w[i], o_sz);
        if (g->mlp_up_w[i]) alloc->free(alloc->ctx, g->mlp_up_w[i], up_sz);
        if (g->mlp_down_w[i]) alloc->free(alloc->ctx, g->mlp_down_w[i], down_sz);
    }
    if (g->attn_q_w) alloc->free(alloc->ctx, g->attn_q_w, g->config.n_layer * sizeof(float *));
    if (g->attn_k_w) alloc->free(alloc->ctx, g->attn_k_w, g->config.n_layer * sizeof(float *));
    if (g->attn_v_w) alloc->free(alloc->ctx, g->attn_v_w, g->config.n_layer * sizeof(float *));
    if (g->attn_o_w) alloc->free(alloc->ctx, g->attn_o_w, g->config.n_layer * sizeof(float *));
    if (g->mlp_up_w) alloc->free(alloc->ctx, g->mlp_up_w, g->config.n_layer * sizeof(float *));
    if (g->mlp_down_w) alloc->free(alloc->ctx, g->mlp_down_w, g->config.n_layer * sizeof(float *));

    if (g->resid_lambdas) alloc->free(alloc->ctx, g->resid_lambdas, g->config.n_layer * sizeof(float));
    if (g->x0_lambdas) alloc->free(alloc->ctx, g->x0_lambdas, g->config.n_layer * sizeof(float));
    if (g->rope_cos) alloc->free(alloc->ctx, g->rope_cos, g->config.sequence_len * (g->config.head_dim / 2) * sizeof(float));
    if (g->rope_sin) alloc->free(alloc->ctx, g->rope_sin, g->config.sequence_len * (g->config.head_dim / 2) * sizeof(float));
}

/* ─── RoPE precompute ───────────────────────────────────────────────────── */

static void precompute_rope(float *cos, float *sin, size_t seq_len, size_t head_dim)
{
    int half = (int)(head_dim / 2);
    for (size_t pos = 0; pos < seq_len; pos++) {
        for (int d = 0; d < half; d++) {
            float theta = (float)pos * powf(10000.0f, -2.0f * (float)d / (float)head_dim);
            cos[pos * half + d] = cosf(theta);
            sin[pos * half + d] = sinf(theta);
        }
    }
}

/* ─── Public API ────────────────────────────────────────────────────────── */

static const hu_model_vtable_t gpt_vtable = {
    .forward = gpt_forward,
    .backward = gpt_backward,
    .get_params = gpt_get_params,
    .num_params = gpt_num_params,
    .deinit = gpt_deinit,
};

hu_error_t hu_gpt_create(hu_allocator_t *alloc, const hu_gpt_config_t *config,
                        hu_model_t *out)
{
    if (!alloc || !config || !out)
        return HU_ERR_INVALID_ARGUMENT;

    if (config->n_embd == 0 || config->vocab_size == 0 || config->n_layer == 0 ||
        config->n_head == 0 || config->head_dim == 0)
        return HU_ERR_INVALID_ARGUMENT;

    if (config->n_embd != config->n_head * config->head_dim)
        return HU_ERR_INVALID_ARGUMENT;

    hu_gpt_t *g = (hu_gpt_t *)alloc->alloc(alloc->ctx, sizeof(hu_gpt_t));
    if (!g)
        return HU_ERR_OUT_OF_MEMORY;

    memset(g, 0, sizeof(hu_gpt_t));
    g->alloc = alloc;
    g->config = *config;

    size_t V = config->vocab_size;
    size_t E = config->n_embd;
    size_t L = config->n_layer;
    size_t n_head = config->n_head;
    size_t n_kv_head = config->n_kv_head;
    size_t head_dim = config->head_dim;
    size_t seq_len = config->sequence_len;
    size_t n_mlp = 4 * E;

    uint64_t seed = 42;
    float init_scale = 0.02f;

    g->wte = (float *)alloc->alloc(alloc->ctx, V * E * sizeof(float));
    if (!g->wte) goto fail;
    for (size_t i = 0; i < V * E; i++)
        g->wte[i] = prng_next(&seed) * init_scale;

    g->lm_head = (float *)alloc->alloc(alloc->ctx, V * E * sizeof(float));
    if (!g->lm_head) goto fail;
    for (size_t i = 0; i < V * E; i++)
        g->lm_head[i] = prng_next(&seed) * init_scale;

    g->attn_q_w = (float **)alloc->alloc(alloc->ctx, L * sizeof(float *));
    g->attn_k_w = (float **)alloc->alloc(alloc->ctx, L * sizeof(float *));
    g->attn_v_w = (float **)alloc->alloc(alloc->ctx, L * sizeof(float *));
    g->attn_o_w = (float **)alloc->alloc(alloc->ctx, L * sizeof(float *));
    g->mlp_up_w = (float **)alloc->alloc(alloc->ctx, L * sizeof(float *));
    g->mlp_down_w = (float **)alloc->alloc(alloc->ctx, L * sizeof(float *));
    if (!g->attn_q_w || !g->attn_k_w || !g->attn_v_w || !g->attn_o_w || !g->mlp_up_w || !g->mlp_down_w)
        goto fail;

    for (size_t i = 0; i < L; i++) {
        g->attn_q_w[i] = (float *)alloc->alloc(alloc->ctx, n_head * head_dim * E * sizeof(float));
        g->attn_k_w[i] = (float *)alloc->alloc(alloc->ctx, n_kv_head * head_dim * E * sizeof(float));
        g->attn_v_w[i] = (float *)alloc->alloc(alloc->ctx, n_kv_head * head_dim * E * sizeof(float));
        g->attn_o_w[i] = (float *)alloc->alloc(alloc->ctx, E * n_head * head_dim * sizeof(float));
        g->mlp_up_w[i] = (float *)alloc->alloc(alloc->ctx, n_mlp * E * sizeof(float));
        g->mlp_down_w[i] = (float *)alloc->alloc(alloc->ctx, E * n_mlp * sizeof(float));
        if (!g->attn_q_w[i] || !g->attn_k_w[i] || !g->attn_v_w[i] || !g->attn_o_w[i] ||
            !g->mlp_up_w[i] || !g->mlp_down_w[i])
            goto fail;
        for (size_t j = 0; j < n_head * head_dim * E; j++) g->attn_q_w[i][j] = prng_next(&seed) * init_scale;
        for (size_t j = 0; j < n_kv_head * head_dim * E; j++) g->attn_k_w[i][j] = prng_next(&seed) * init_scale;
        for (size_t j = 0; j < n_kv_head * head_dim * E; j++) g->attn_v_w[i][j] = prng_next(&seed) * init_scale;
        for (size_t j = 0; j < E * n_head * head_dim; j++) g->attn_o_w[i][j] = prng_next(&seed) * init_scale;
        for (size_t j = 0; j < n_mlp * E; j++) g->mlp_up_w[i][j] = prng_next(&seed) * init_scale;
        for (size_t j = 0; j < E * n_mlp; j++) g->mlp_down_w[i][j] = prng_next(&seed) * init_scale;
    }

    g->resid_lambdas = (float *)alloc->alloc(alloc->ctx, L * sizeof(float));
    g->x0_lambdas = (float *)alloc->alloc(alloc->ctx, L * sizeof(float));
    if (!g->resid_lambdas || !g->x0_lambdas) goto fail;
    for (size_t i = 0; i < L; i++) {
        g->resid_lambdas[i] = 0.9f + prng_next(&seed) * 0.1f;
        g->x0_lambdas[i] = 0.1f + prng_next(&seed) * 0.05f;
    }

    size_t rope_len = seq_len * (head_dim / 2);
    g->rope_cos = (float *)alloc->alloc(alloc->ctx, rope_len * sizeof(float));
    g->rope_sin = (float *)alloc->alloc(alloc->ctx, rope_len * sizeof(float));
    if (!g->rope_cos || !g->rope_sin) goto fail;
    precompute_rope(g->rope_cos, g->rope_sin, seq_len, head_dim);

    /* Param count */
    g->total_params = V * E * 2;  /* wte + lm_head */
    g->total_params += L * (n_head * head_dim * E + n_kv_head * head_dim * E * 2 + E * n_head * head_dim);
    g->total_params += L * (n_mlp * E + E * n_mlp);
    g->total_params += L * 2;  /* resid_lambdas, x0_lambdas */

    out->ctx = g;
    out->vtable = &gpt_vtable;
    return HU_OK;

fail:
    gpt_deinit(g, alloc);
    alloc->free(alloc->ctx, g, sizeof(hu_gpt_t));
    return HU_ERR_OUT_OF_MEMORY;
}
