/* GPT model with full backward pass — CPU reference implementation.
 * Supports activation caching, gradient computation, and parameter registration.
 * Layout: Q/K/V in [B*S × n_head*hd] (row-major per token). */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ─── Activation cache ───────────────────────────────────────────────────── */

typedef struct {
    float *x_pre, *x_norm1, *q, *k, *v, *attn_w, *attn_out;
    float *x_post_attn, *x_norm2, *up_pre;
} gpt_lcache_t;

typedef struct {
    int32_t *ids;
    float *x_embed, *x0, *x_pre_final, *x_final, *logits;
    gpt_lcache_t *layers;
    size_t B, S;
    int valid;
} gpt_cache_t;

/* ─── Internal struct ────────────────────────────────────────────────────── */

typedef struct hu_gpt {
    hu_allocator_t *alloc;
    hu_gpt_config_t config;
    float *wte, *lm_head;
    float **aqw, **akw, **avw, **aow, **muw, **mdw;
    float *rl, *x0l, *rcos, *rsin;
    float *grad_wte, *grad_lm_head;
    float **grad_aqw, **grad_akw, **grad_avw, **grad_aow, **grad_muw, **grad_mdw;
    float *grad_rl, *grad_x0l;
    gpt_cache_t cache;
    size_t total_params;
    hu_ml_tensor_t *param_descs;
    size_t param_desc_count;
} hu_gpt_t;

/* ─── PRNG ───────────────────────────────────────────────────────────────── */

static float prng_next(uint64_t *seed)
{
    *seed = *seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((float)(*seed >> 33) / (float)(1ULL << 31)) - 0.5f;
}

/* ─── Forward helpers ────────────────────────────────────────────────────── */

static float relu_sq(float x) { float r = (x > 0.0f) ? x : 0.0f; return r * r; }

static void softmax_vec(float *x, int n)
{
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - mx); sum += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= sum;
}

/* out[m×n] = A[m×k] @ B[n×k]^T */
static void matmul_atb(float *out, const float *a, const float *b, int m, int n, int k)
{
    for (int i = 0; i < m; i++)
        for (int j = 0; j < n; j++) {
            float s = 0.0f;
            for (int t = 0; t < k; t++) s += a[i * k + t] * b[j * k + t];
            out[i * n + j] = s;
        }
}

#define RMS_EPS 1e-6f

static void rms_norm(float *out, const float *x, size_t n)
{
    float sq = 0.0f;
    for (size_t i = 0; i < n; i++) sq += x[i] * x[i];
    float rms = sqrtf(sq / (float)n + RMS_EPS);
    for (size_t i = 0; i < n; i++) out[i] = x[i] / rms;
}

/* Q/K/V layout: [B*S × n_head*hd] — apply RoPE per token position. */
static void apply_rope(float *q, float *k, const float *cs, const float *sn,
                       int B, int S, int nh, int nkv, int hd)
{
    int half = hd / 2;
    for (int b = 0; b < B; b++)
        for (int s = 0; s < S; s++) {
            const float *c = cs + s * half, *si = sn + s * half;
            for (int h = 0; h < nh; h++) {
                float *qh = q + (b * S + s) * (nh * hd) + h * hd;
                for (int d = 0; d < half; d++) {
                    float q0 = qh[d], q1 = qh[d + half];
                    qh[d] = q0 * c[d] - q1 * si[d];
                    qh[d + half] = q0 * si[d] + q1 * c[d];
                }
            }
            for (int h = 0; h < nkv; h++) {
                float *kh = k + (b * S + s) * (nkv * hd) + h * hd;
                for (int d = 0; d < half; d++) {
                    float k0 = kh[d], k1 = kh[d + half];
                    kh[d] = k0 * c[d] - k1 * si[d];
                    kh[d + half] = k0 * si[d] + k1 * c[d];
                }
            }
        }
}

/* ─── Backward helpers ──────────────────────────────────────────────────── */

/* C[m×k] += A[m×n] @ B[n×k] */
static void matmul_add_ab(float *c, const float *a, const float *b, int m, int n, int k)
{
    for (int i = 0; i < m; i++)
        for (int j = 0; j < k; j++) {
            float s = 0.0f;
            for (int t = 0; t < n; t++) s += a[i * n + t] * b[t * k + j];
            c[i * k + j] += s;
        }
}

/* C[n×k] += A[m×n]^T @ B[m×k] */
static void matmul_add_tab(float *c, const float *a, const float *b, int m, int n, int k)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < k; j++) {
            float s = 0.0f;
            for (int t = 0; t < m; t++) s += a[t * n + i] * b[t * k + j];
            c[i * k + j] += s;
        }
}

/* dx[n] += rms_norm_backward(dy, x, y) per vector */
static void rms_norm_bw(float *dx, const float *dy, const float *x,
                         const float *y, size_t n)
{
    float sq = 0.0f;
    for (size_t i = 0; i < n; i++) sq += x[i] * x[i];
    float inv = 1.0f / sqrtf(sq / (float)n + RMS_EPS);
    float dot = 0.0f;
    for (size_t i = 0; i < n; i++) dot += dy[i] * y[i];
    dot /= (float)n;
    for (size_t i = 0; i < n; i++) dx[i] += inv * (dy[i] - y[i] * dot);
}

/* Transpose of RoPE rotation — inverse rotation for gradient propagation. */
static void apply_rope_bw(float *dq, float *dk, const float *cs, const float *sn,
                           int B, int S, int nh, int nkv, int hd)
{
    int half = hd / 2;
    for (int b = 0; b < B; b++)
        for (int s = 0; s < S; s++) {
            const float *c = cs + s * half, *si = sn + s * half;
            for (int h = 0; h < nh; h++) {
                float *dqh = dq + (b * S + s) * (nh * hd) + h * hd;
                for (int d = 0; d < half; d++) {
                    float d0 = dqh[d], d1 = dqh[d + half];
                    dqh[d] = d0 * c[d] + d1 * si[d];
                    dqh[d + half] = -d0 * si[d] + d1 * c[d];
                }
            }
            for (int h = 0; h < nkv; h++) {
                float *dkh = dk + (b * S + s) * (nkv * hd) + h * hd;
                for (int d = 0; d < half; d++) {
                    float d0 = dkh[d], d1 = dkh[d + half];
                    dkh[d] = d0 * c[d] + d1 * si[d];
                    dkh[d + half] = -d0 * si[d] + d1 * c[d];
                }
            }
        }
}

/* ─── Cache management ──────────────────────────────────────────────────── */

static void cache_free(hu_gpt_t *g)
{
    gpt_cache_t *c = &g->cache;
    if (!c->valid) return;
    hu_allocator_t *a = g->alloc;
    size_t B = c->B, S = c->S;
    const hu_gpt_config_t *cfg = &g->config;
    size_t E = cfg->n_embd, V = cfg->vocab_size, L = cfg->n_layer;
    size_t nh = cfg->n_head, nkv = cfg->n_kv_head, hd = cfg->head_dim, nm = 4 * E;
#define CF(p, sz) do { if (p) a->free(a->ctx, p, sz); } while(0)
    CF(c->ids, B*S*sizeof(int32_t));
    CF(c->x_embed, B*S*E*4); CF(c->x0, B*S*E*4);
    CF(c->x_pre_final, B*S*E*4); CF(c->x_final, B*S*E*4);
    CF(c->logits, B*S*V*4);
    for (size_t i = 0; i < L && c->layers; i++) {
        gpt_lcache_t *lc = &c->layers[i];
        CF(lc->x_pre, B*S*E*4); CF(lc->x_norm1, B*S*E*4);
        CF(lc->q, B*nh*S*hd*4); CF(lc->k, B*nkv*S*hd*4); CF(lc->v, B*nkv*S*hd*4);
        CF(lc->attn_w, B*nh*S*S*4); CF(lc->attn_out, B*S*E*4);
        CF(lc->x_post_attn, B*S*E*4); CF(lc->x_norm2, B*S*E*4);
        CF(lc->up_pre, B*S*nm*4);
    }
    CF(c->layers, L * sizeof(gpt_lcache_t));
#undef CF
    memset(c, 0, sizeof(gpt_cache_t));
}

#define CA(p, sz) do { p = a->alloc(a->ctx, sz); if (!(p)) { cache_free(g); return HU_ERR_OUT_OF_MEMORY; } } while(0)

static hu_error_t cache_alloc(hu_gpt_t *g, size_t B, size_t S)
{
    cache_free(g);
    gpt_cache_t *c = &g->cache;
    hu_allocator_t *a = g->alloc;
    const hu_gpt_config_t *cfg = &g->config;
    size_t E = cfg->n_embd, V = cfg->vocab_size, L = cfg->n_layer;
    size_t nh = cfg->n_head, nkv = cfg->n_kv_head, hd = cfg->head_dim, nm = 4 * E;
    c->B = B; c->S = S;
    CA(c->ids, B*S*sizeof(int32_t));
    CA(c->x_embed, B*S*E*4); CA(c->x0, B*S*E*4);
    CA(c->x_pre_final, B*S*E*4); CA(c->x_final, B*S*E*4);
    CA(c->logits, B*S*V*4);
    CA(c->layers, L * sizeof(gpt_lcache_t));
    memset(c->layers, 0, L * sizeof(gpt_lcache_t));
    for (size_t i = 0; i < L; i++) {
        gpt_lcache_t *lc = &c->layers[i];
        CA(lc->x_pre, B*S*E*4); CA(lc->x_norm1, B*S*E*4);
        CA(lc->q, B*nh*S*hd*4); CA(lc->k, B*nkv*S*hd*4); CA(lc->v, B*nkv*S*hd*4);
        CA(lc->attn_w, B*nh*S*S*4); CA(lc->attn_out, B*S*E*4);
        CA(lc->x_post_attn, B*S*E*4); CA(lc->x_norm2, B*S*E*4);
        CA(lc->up_pre, B*S*nm*4);
    }
    c->valid = 1;
    return HU_OK;
}

#undef CA

/* ─── Forward pass (with activation caching) ─────────────────────────────── */

static hu_error_t gpt_forward(void *ctx, const hu_ml_tensor_t *input,
                               hu_ml_tensor_t *output)
{
    hu_gpt_t *g = (hu_gpt_t *)ctx;
    const hu_gpt_config_t *cfg = &g->config;
    if (!input || !output || input->ndim != 2 || input->dtype != HU_ML_DTYPE_I32)
        return HU_ERR_INVALID_ARGUMENT;

    size_t B = input->shape[0], S = input->shape[1];
    if (S > cfg->sequence_len) return HU_ERR_INVALID_ARGUMENT;

    const int32_t *ids = (const int32_t *)input->data;
    size_t E = cfg->n_embd, V = cfg->vocab_size, L = cfg->n_layer;
    size_t nh = cfg->n_head, nkv = cfg->n_kv_head, hd = cfg->head_dim, nm = 4 * E;
    hu_allocator_t *a = g->alloc;

    hu_error_t cerr = cache_alloc(g, B, S);
    if (cerr != HU_OK) return cerr;
    gpt_cache_t *c = &g->cache;
    memcpy(c->ids, ids, B * S * sizeof(int32_t));

    size_t xsz = B * S * E * sizeof(float);
    float *x = (float *)a->alloc(a->ctx, xsz);
    float *x0 = (float *)a->alloc(a->ctx, xsz);
    float *xn = (float *)a->alloc(a->ctx, xsz);
    float *q_buf = (float *)a->alloc(a->ctx, B * nh * S * hd * sizeof(float));
    float *k_buf = (float *)a->alloc(a->ctx, B * nkv * S * hd * sizeof(float));
    float *v_buf = (float *)a->alloc(a->ctx, B * nkv * S * hd * sizeof(float));
    float *ao = (float *)a->alloc(a->ctx, xsz);
    float *up_buf = (float *)a->alloc(a->ctx, B * S * nm * sizeof(float));
    float *mo = (float *)a->alloc(a->ctx, xsz);
    if (!x || !x0 || !xn || !q_buf || !k_buf || !v_buf || !ao || !up_buf || !mo) {
        if (x) a->free(a->ctx, x, xsz);
        if (x0) a->free(a->ctx, x0, xsz);
        if (xn) a->free(a->ctx, xn, xsz);
        if (q_buf) a->free(a->ctx, q_buf, B*nh*S*hd*4);
        if (k_buf) a->free(a->ctx, k_buf, B*nkv*S*hd*4);
        if (v_buf) a->free(a->ctx, v_buf, B*nkv*S*hd*4);
        if (ao) a->free(a->ctx, ao, xsz);
        if (up_buf) a->free(a->ctx, up_buf, B*S*nm*4);
        if (mo) a->free(a->ctx, mo, xsz);
        cache_free(g);
        return HU_ERR_OUT_OF_MEMORY;
    }

    /* 1. Token embedding */
    for (size_t b = 0; b < B; b++)
        for (size_t s = 0; s < S; s++) {
            int32_t tid = ids[b * S + s];
            if (tid < 0 || (size_t)tid >= V) tid = 0;
            memcpy(x + (b * S + s) * E, g->wte + (size_t)tid * E, E * sizeof(float));
        }
    memcpy(c->x_embed, x, xsz);

    /* 2. Initial RMS norm */
    for (size_t i = 0; i < B * S; i++) rms_norm(x0 + i * E, x + i * E, E);
    memcpy(c->x0, x0, xsz);

    /* 3. Transformer layers */
    for (size_t li = 0; li < L; li++) {
        gpt_lcache_t *lc = &c->layers[li];
        memcpy(lc->x_pre, x, xsz);

        /* a. Residual mixing */
        float rlv = g->rl[li], x0v = g->x0l[li];
        for (size_t i = 0; i < B * S * E; i++) x[i] = rlv * x[i] + x0v * x0[i];

        /* b. First RMS norm */
        for (size_t i = 0; i < B * S; i++) rms_norm(xn + i * E, x + i * E, E);
        memcpy(lc->x_norm1, xn, xsz);

        /* c. Q, K, V projections (output in [B*S × n_head*hd]) */
        matmul_atb(q_buf, xn, g->aqw[li], (int)(B * S), (int)(nh * hd), (int)E);
        matmul_atb(k_buf, xn, g->akw[li], (int)(B * S), (int)(nkv * hd), (int)E);
        matmul_atb(v_buf, xn, g->avw[li], (int)(B * S), (int)(nkv * hd), (int)E);

        /* d. RoPE */
        apply_rope(q_buf, k_buf, g->rcos, g->rsin, (int)B, (int)S, (int)nh, (int)nkv, (int)hd);
        memcpy(lc->q, q_buf, B * nh * S * hd * sizeof(float));
        memcpy(lc->k, k_buf, B * nkv * S * hd * sizeof(float));
        memcpy(lc->v, v_buf, B * nkv * S * hd * sizeof(float));

        /* e. Causal self-attention — scores written directly to cache */
        float scale = 1.0f / sqrtf((float)hd);
        memset(ao, 0, xsz);
        for (size_t b = 0; b < B; b++)
            for (size_t h = 0; h < nh; h++) {
                size_t kvh = h % nkv;
                for (size_t i = 0; i < S; i++) {
                    float *w = lc->attn_w + (b * nh + h) * S * S + i * S;
                    const float *qi = q_buf + (b * S + i) * (nh * hd) + h * hd;
                    for (size_t j = 0; j < S; j++) {
                        if (j <= i) {
                            const float *kj = k_buf + (b * S + j) * (nkv * hd) + kvh * hd;
                            float dot = 0.0f;
                            for (size_t d = 0; d < hd; d++) dot += qi[d] * kj[d];
                            w[j] = dot * scale;
                        } else {
                            w[j] = -1e9f;
                        }
                    }
                    softmax_vec(w, (int)S);
                    float *oh = ao + (b * S + i) * E + h * hd;
                    for (size_t j = 0; j < S; j++) {
                        const float *vj = v_buf + (b * S + j) * (nkv * hd) + kvh * hd;
                        for (size_t d = 0; d < hd; d++) oh[d] += w[j] * vj[d];
                    }
                }
            }
        memcpy(lc->attn_out, ao, xsz);

        /* f. Output projection: proj = ao @ W_o^T */
        float *proj = (float *)a->alloc(a->ctx, xsz);
        if (!proj) {
            a->free(a->ctx, x, xsz); a->free(a->ctx, x0, xsz); a->free(a->ctx, xn, xsz);
            a->free(a->ctx, q_buf, B*nh*S*hd*4); a->free(a->ctx, k_buf, B*nkv*S*hd*4);
            a->free(a->ctx, v_buf, B*nkv*S*hd*4); a->free(a->ctx, ao, xsz);
            a->free(a->ctx, up_buf, B*S*nm*4); a->free(a->ctx, mo, xsz);
            cache_free(g); return HU_ERR_OUT_OF_MEMORY;
        }
        matmul_atb(proj, ao, g->aow[li], (int)(B * S), (int)E, (int)(nh * hd));

        /* g. Attention residual */
        for (size_t i = 0; i < B * S * E; i++) x[i] += proj[i];
        a->free(a->ctx, proj, xsz);
        memcpy(lc->x_post_attn, x, xsz);

        /* h. Second RMS norm */
        for (size_t i = 0; i < B * S; i++) rms_norm(xn + i * E, x + i * E, E);
        memcpy(lc->x_norm2, xn, xsz);

        /* i. MLP: up, activation, down */
        matmul_atb(up_buf, xn, g->muw[li], (int)(B * S), (int)nm, (int)E);
        memcpy(lc->up_pre, up_buf, B * S * nm * sizeof(float));
        for (size_t i = 0; i < B * S * nm; i++) up_buf[i] = relu_sq(up_buf[i]);
        matmul_atb(mo, up_buf, g->mdw[li], (int)(B * S), (int)E, (int)nm);

        /* j. MLP residual */
        for (size_t i = 0; i < B * S * E; i++) x[i] += mo[i];
    }

    /* 4. Final RMS norm (in-place) */
    memcpy(c->x_pre_final, x, xsz);
    for (size_t i = 0; i < B * S; i++) rms_norm(x + i * E, x + i * E, E);
    memcpy(c->x_final, x, xsz);

    /* 5. Logits */
    size_t lsz = B * S * V * sizeof(float);
    float *logits = (float *)a->alloc(a->ctx, lsz);
    if (!logits) {
        a->free(a->ctx, x, xsz); a->free(a->ctx, x0, xsz); a->free(a->ctx, xn, xsz);
        a->free(a->ctx, q_buf, B*nh*S*hd*4); a->free(a->ctx, k_buf, B*nkv*S*hd*4);
        a->free(a->ctx, v_buf, B*nkv*S*hd*4); a->free(a->ctx, ao, xsz);
        a->free(a->ctx, up_buf, B*S*nm*4); a->free(a->ctx, mo, xsz);
        cache_free(g); return HU_ERR_OUT_OF_MEMORY;
    }
    matmul_atb(logits, x, g->lm_head, (int)(B * S), (int)V, (int)E);

    /* 6. Soft-cap */
    for (size_t i = 0; i < B * S * V; i++) logits[i] = 15.0f * tanhf(logits[i] / 15.0f);
    memcpy(c->logits, logits, lsz);

    output->data = logits;
    output->shape[0] = B; output->shape[1] = S; output->shape[2] = V;
    output->ndim = 3; output->dtype = HU_ML_DTYPE_F32; output->size_bytes = lsz;

    a->free(a->ctx, x, xsz); a->free(a->ctx, x0, xsz); a->free(a->ctx, xn, xsz);
    a->free(a->ctx, q_buf, B*nh*S*hd*4); a->free(a->ctx, k_buf, B*nkv*S*hd*4);
    a->free(a->ctx, v_buf, B*nkv*S*hd*4); a->free(a->ctx, ao, xsz);
    a->free(a->ctx, up_buf, B*S*nm*4); a->free(a->ctx, mo, xsz);
    return HU_OK;
}

/* ─── Backward pass ──────────────────────────────────────────────────────── */

static hu_error_t gpt_backward(void *ctx, const hu_ml_tensor_t *grad_output)
{
    hu_gpt_t *g = (hu_gpt_t *)ctx;
    gpt_cache_t *c = &g->cache;
    if (!c->valid || !grad_output || !grad_output->data)
        return HU_ERR_INVALID_ARGUMENT;

    const hu_gpt_config_t *cfg = &g->config;
    size_t B = c->B, S = c->S;
    size_t E = cfg->n_embd, V = cfg->vocab_size, L = cfg->n_layer;
    size_t nh = cfg->n_head, nkv = cfg->n_kv_head, hd = cfg->head_dim, nm = 4 * E;
    hu_allocator_t *a = g->alloc;
    size_t BS = B * S, xsz = BS * E * sizeof(float);
    float scale = 1.0f / sqrtf((float)hd);

    const float *d_capped = (const float *)grad_output->data;

    /* Allocate temporary gradient buffers */
    float *d_raw = (float *)a->alloc(a->ctx, BS * V * 4);
    float *dx = (float *)a->alloc(a->ctx, xsz);
    float *d_xn = (float *)a->alloc(a->ctx, xsz);
    float *d_x0 = (float *)a->alloc(a->ctx, xsz);
    float *d_q = (float *)a->alloc(a->ctx, B * nh * S * hd * 4);
    float *d_k = (float *)a->alloc(a->ctx, B * nkv * S * hd * 4);
    float *d_v = (float *)a->alloc(a->ctx, B * nkv * S * hd * 4);
    float *d_ao = (float *)a->alloc(a->ctx, xsz);
    float *d_up = (float *)a->alloc(a->ctx, BS * nm * 4);
    float *d_mlp = (float *)a->alloc(a->ctx, xsz);
    float *x_mixed = (float *)a->alloc(a->ctx, xsz);
    float *up_post = (float *)a->alloc(a->ctx, BS * nm * 4);
    float *d_wbuf = (float *)a->alloc(a->ctx, S * sizeof(float));
    float *d_sbuf = (float *)a->alloc(a->ctx, S * sizeof(float));

    if (!d_raw || !dx || !d_xn || !d_x0 || !d_q || !d_k || !d_v ||
        !d_ao || !d_up || !d_mlp || !x_mixed || !up_post || !d_wbuf || !d_sbuf) {
        /* cleanup all */
        if (d_raw) a->free(a->ctx, d_raw, BS*V*4);
        if (dx) a->free(a->ctx, dx, xsz);
        if (d_xn) a->free(a->ctx, d_xn, xsz);
        if (d_x0) a->free(a->ctx, d_x0, xsz);
        if (d_q) a->free(a->ctx, d_q, B*nh*S*hd*4);
        if (d_k) a->free(a->ctx, d_k, B*nkv*S*hd*4);
        if (d_v) a->free(a->ctx, d_v, B*nkv*S*hd*4);
        if (d_ao) a->free(a->ctx, d_ao, xsz);
        if (d_up) a->free(a->ctx, d_up, BS*nm*4);
        if (d_mlp) a->free(a->ctx, d_mlp, xsz);
        if (x_mixed) a->free(a->ctx, x_mixed, xsz);
        if (up_post) a->free(a->ctx, up_post, BS*nm*4);
        if (d_wbuf) a->free(a->ctx, d_wbuf, S*4);
        if (d_sbuf) a->free(a->ctx, d_sbuf, S*4);
        cache_free(g);
        return HU_ERR_OUT_OF_MEMORY;
    }

    memset(dx, 0, xsz);
    memset(d_x0, 0, xsz);

    /* 6'. Soft-cap backward: d_raw = d_capped * (1 - tanh²(raw/15)) */
    for (size_t i = 0; i < BS * V; i++) {
        float t = c->logits[i] / 15.0f;
        d_raw[i] = d_capped[i] * (1.0f - t * t);
    }

    /* 5'. LM head backward */
    /* dx = d_raw @ lm_head  (BS×V @ V×E = BS×E) */
    matmul_add_ab(dx, d_raw, g->lm_head, (int)BS, (int)V, (int)E);
    /* grad_lm_head += d_raw^T @ x_final  (V×BS @ BS×E = V×E) */
    matmul_add_tab(g->grad_lm_head, d_raw, c->x_final, (int)BS, (int)V, (int)E);

    a->free(a->ctx, d_raw, BS * V * 4);

    /* 4'. Final RMS norm backward */
    {
        float *dx_tmp = (float *)a->alloc(a->ctx, xsz);
        if (!dx_tmp) goto cleanup;
        memcpy(dx_tmp, dx, xsz);
        memset(dx, 0, xsz);
        for (size_t i = 0; i < BS; i++)
            rms_norm_bw(dx + i * E, dx_tmp + i * E, c->x_pre_final + i * E,
                        c->x_final + i * E, E);
        a->free(a->ctx, dx_tmp, xsz);
    }

    /* 3'. Transformer layers (reverse) */
    for (size_t li = L; li-- > 0; ) {
        gpt_lcache_t *lc = &c->layers[li];

        /* MLP residual: x = x_post_attn + mlp_out → dx stays, d_mlp = dx */
        memcpy(d_mlp, dx, xsz);

        /* MLP down backward: mlp_out = up_post @ W_down^T */
        /* Recompute up_post from cached up_pre */
        for (size_t i = 0; i < BS * nm; i++)
            up_post[i] = relu_sq(lc->up_pre[i]);

        memset(d_up, 0, BS * nm * 4);
        /* d_up += d_mlp @ W_down  (BS×E @ E×nm = BS×nm) */
        matmul_add_ab(d_up, d_mlp, g->mdw[li], (int)BS, (int)E, (int)nm);
        /* grad_mdw += d_mlp^T @ up_post  (E×BS @ BS×nm = E×nm) */
        matmul_add_tab(g->grad_mdw[li], d_mlp, up_post, (int)BS, (int)E, (int)nm);

        /* relu_sq backward: d_up_pre = d_up * (pre > 0 ? 2*pre : 0) */
        for (size_t i = 0; i < BS * nm; i++)
            d_up[i] = lc->up_pre[i] > 0.0f ? d_up[i] * 2.0f * lc->up_pre[i] : 0.0f;

        /* MLP up backward: up_pre = x_norm2 @ W_up^T */
        memset(d_xn, 0, xsz);
        /* d_xn += d_up @ W_up  (BS×nm @ nm×E = BS×E) */
        matmul_add_ab(d_xn, d_up, g->muw[li], (int)BS, (int)nm, (int)E);
        /* grad_muw += d_up^T @ x_norm2  (nm×BS @ BS×E = nm×E) */
        matmul_add_tab(g->grad_muw[li], d_up, lc->x_norm2, (int)BS, (int)nm, (int)E);

        /* Second RMS norm backward: accumulate into dx */
        for (size_t i = 0; i < BS; i++)
            rms_norm_bw(dx + i * E, d_xn + i * E,
                        lc->x_post_attn + i * E, lc->x_norm2 + i * E, E);

        /* Attention residual: x_post_attn = x_mixed + proj → dx has both */

        /* O projection backward: proj = attn_out @ W_o^T */
        memset(d_ao, 0, xsz);
        /* d_ao += dx @ W_o  (BS×E @ E×(nh*hd) = BS×(nh*hd)) */
        matmul_add_ab(d_ao, dx, g->aow[li], (int)BS, (int)E, (int)(nh * hd));
        /* grad_aow += dx^T @ attn_out  (E×BS @ BS×(nh*hd) = E×(nh*hd)) */
        matmul_add_tab(g->grad_aow[li], dx, lc->attn_out, (int)BS, (int)E, (int)(nh * hd));

        /* Attention backward */
        memset(d_q, 0, B * nh * S * hd * 4);
        memset(d_k, 0, B * nkv * S * hd * 4);
        memset(d_v, 0, B * nkv * S * hd * 4);

        for (size_t b = 0; b < B; b++)
            for (size_t h = 0; h < nh; h++) {
                size_t kvh = h % nkv;
                for (size_t i = 0; i < S; i++) {
                    const float *doi = d_ao + (b * S + i) * E + h * hd;
                    const float *wi = lc->attn_w + (b * nh + h) * S * S + i * S;

                    for (size_t j = 0; j < S; j++) {
                        const float *vj = lc->v + (b * S + j) * (nkv * hd) + kvh * hd;
                        float dw = 0.0f;
                        for (size_t d = 0; d < hd; d++) dw += doi[d] * vj[d];
                        d_wbuf[j] = dw;
                        float *dvj = d_v + (b * S + j) * (nkv * hd) + kvh * hd;
                        for (size_t d = 0; d < hd; d++) dvj[d] += wi[j] * doi[d];
                    }

                    float dot_wdw = 0.0f;
                    for (size_t j = 0; j < S; j++) dot_wdw += wi[j] * d_wbuf[j];
                    for (size_t j = 0; j < S; j++)
                        d_sbuf[j] = scale * wi[j] * (d_wbuf[j] - dot_wdw);

                    float *dqi = d_q + (b * S + i) * (nh * hd) + h * hd;
                    for (size_t j = 0; j < S; j++) {
                        const float *kj = lc->k + (b * S + j) * (nkv * hd) + kvh * hd;
                        for (size_t d = 0; d < hd; d++) dqi[d] += d_sbuf[j] * kj[d];
                        float *dkj = d_k + (b * S + j) * (nkv * hd) + kvh * hd;
                        const float *qic = lc->q + (b * S + i) * (nh * hd) + h * hd;
                        for (size_t d = 0; d < hd; d++) dkj[d] += d_sbuf[j] * qic[d];
                    }
                }
            }

        /* RoPE backward */
        apply_rope_bw(d_q, d_k, g->rcos, g->rsin, (int)B, (int)S, (int)nh, (int)nkv, (int)hd);

        /* QKV projection backward */
        memset(d_xn, 0, xsz);
        /* d_xn += d_q @ W_q  (BS×(nh*hd) @ (nh*hd)×E = BS×E) */
        matmul_add_ab(d_xn, d_q, g->aqw[li], (int)BS, (int)(nh * hd), (int)E);
        matmul_add_tab(g->grad_aqw[li], d_q, lc->x_norm1, (int)BS, (int)(nh * hd), (int)E);

        matmul_add_ab(d_xn, d_k, g->akw[li], (int)BS, (int)(nkv * hd), (int)E);
        matmul_add_tab(g->grad_akw[li], d_k, lc->x_norm1, (int)BS, (int)(nkv * hd), (int)E);

        matmul_add_ab(d_xn, d_v, g->avw[li], (int)BS, (int)(nkv * hd), (int)E);
        matmul_add_tab(g->grad_avw[li], d_v, lc->x_norm1, (int)BS, (int)(nkv * hd), (int)E);

        /* First RMS norm backward: recompute x_mixed = rl*x_pre + x0l*x0 */
        float rlv = g->rl[li], x0v = g->x0l[li];
        for (size_t i = 0; i < BS * E; i++)
            x_mixed[i] = rlv * lc->x_pre[i] + x0v * c->x0[i];
        for (size_t i = 0; i < BS; i++)
            rms_norm_bw(dx + i * E, d_xn + i * E,
                        x_mixed + i * E, lc->x_norm1 + i * E, E);

        /* Resid lambda backward: x_mixed = rl*x_pre + x0l*x0 */
        for (size_t i = 0; i < BS * E; i++) {
            g->grad_rl[li] += dx[i] * lc->x_pre[i];
            g->grad_x0l[li] += dx[i] * c->x0[i];
            d_x0[i] += x0v * dx[i];
            dx[i] *= rlv;
        }
    }

    /* 2'. Initial RMS norm backward */
    for (size_t i = 0; i < BS; i++)
        rms_norm_bw(dx + i * E, d_x0 + i * E,
                    c->x_embed + i * E, c->x0 + i * E, E);

    /* 1'. Embedding backward: scatter-add into grad_wte */
    for (size_t b = 0; b < B; b++)
        for (size_t s = 0; s < S; s++) {
            int32_t tid = c->ids[b * S + s];
            if (tid < 0 || (size_t)tid >= V) tid = 0;
            float *gw = g->grad_wte + (size_t)tid * E;
            const float *dxi = dx + (b * S + s) * E;
            for (size_t d = 0; d < E; d++) gw[d] += dxi[d];
        }

    /* Cleanup temporaries */
    a->free(a->ctx, dx, xsz);
    a->free(a->ctx, d_xn, xsz);
    a->free(a->ctx, d_x0, xsz);
    a->free(a->ctx, d_q, B*nh*S*hd*4);
    a->free(a->ctx, d_k, B*nkv*S*hd*4);
    a->free(a->ctx, d_v, B*nkv*S*hd*4);
    a->free(a->ctx, d_ao, xsz);
    a->free(a->ctx, d_up, BS*nm*4);
    a->free(a->ctx, d_mlp, xsz);
    a->free(a->ctx, x_mixed, xsz);
    a->free(a->ctx, up_post, BS*nm*4);
    a->free(a->ctx, d_wbuf, S*4);
    a->free(a->ctx, d_sbuf, S*4);
    cache_free(g);
    return HU_OK;

cleanup:
    a->free(a->ctx, dx, xsz);
    a->free(a->ctx, d_xn, xsz);
    a->free(a->ctx, d_x0, xsz);
    a->free(a->ctx, d_q, B*nh*S*hd*4);
    a->free(a->ctx, d_k, B*nkv*S*hd*4);
    a->free(a->ctx, d_v, B*nkv*S*hd*4);
    a->free(a->ctx, d_ao, xsz);
    a->free(a->ctx, d_up, BS*nm*4);
    a->free(a->ctx, d_mlp, xsz);
    a->free(a->ctx, x_mixed, xsz);
    a->free(a->ctx, up_post, BS*nm*4);
    a->free(a->ctx, d_wbuf, S*4);
    a->free(a->ctx, d_sbuf, S*4);
    cache_free(g);
    return HU_ERR_OUT_OF_MEMORY;
}

/* ─── Parameter access ───────────────────────────────────────────────────── */

static hu_error_t gpt_get_params(void *ctx, hu_ml_tensor_t **params, size_t *count)
{
    hu_gpt_t *g = (hu_gpt_t *)ctx;
    if (!params || !count) return HU_ERR_INVALID_ARGUMENT;
    *params = g->param_descs;
    *count = g->param_desc_count;
    return HU_OK;
}

static size_t gpt_num_params(void *ctx) { return ((hu_gpt_t *)ctx)->total_params; }

/* ─── Deinit ─────────────────────────────────────────────────────────────── */

static void gpt_deinit(void *ctx, hu_allocator_t *alloc)
{
    hu_gpt_t *g = (hu_gpt_t *)ctx;
    if (!g) return;
    const hu_gpt_config_t *cfg = &g->config;
    size_t E = cfg->n_embd, V = cfg->vocab_size, L = cfg->n_layer;
    size_t nh = cfg->n_head, nkv = cfg->n_kv_head, hd = cfg->head_dim, nm = 4 * E;

    cache_free(g);

#define FP(p, sz) do { if (p) alloc->free(alloc->ctx, p, sz); } while(0)
    FP(g->wte, V*E*4); FP(g->lm_head, V*E*4);
    FP(g->grad_wte, V*E*4); FP(g->grad_lm_head, V*E*4);
    for (size_t i = 0; i < L; i++) {
        size_t qsz = nh*hd*E*4, kvsz = nkv*hd*E*4, osz = E*nh*hd*4;
        size_t usz = nm*E*4, dsz = E*nm*4;
        if (g->aqw) FP(g->aqw[i], qsz); if (g->akw) FP(g->akw[i], kvsz);
        if (g->avw) FP(g->avw[i], kvsz); if (g->aow) FP(g->aow[i], osz);
        if (g->muw) FP(g->muw[i], usz); if (g->mdw) FP(g->mdw[i], dsz);
        if (g->grad_aqw) FP(g->grad_aqw[i], qsz); if (g->grad_akw) FP(g->grad_akw[i], kvsz);
        if (g->grad_avw) FP(g->grad_avw[i], kvsz); if (g->grad_aow) FP(g->grad_aow[i], osz);
        if (g->grad_muw) FP(g->grad_muw[i], usz); if (g->grad_mdw) FP(g->grad_mdw[i], dsz);
    }
    FP(g->aqw, L*sizeof(float*)); FP(g->akw, L*sizeof(float*));
    FP(g->avw, L*sizeof(float*)); FP(g->aow, L*sizeof(float*));
    FP(g->muw, L*sizeof(float*)); FP(g->mdw, L*sizeof(float*));
    FP(g->grad_aqw, L*sizeof(float*)); FP(g->grad_akw, L*sizeof(float*));
    FP(g->grad_avw, L*sizeof(float*)); FP(g->grad_aow, L*sizeof(float*));
    FP(g->grad_muw, L*sizeof(float*)); FP(g->grad_mdw, L*sizeof(float*));
    FP(g->rl, L*4); FP(g->x0l, L*4);
    FP(g->grad_rl, L*4); FP(g->grad_x0l, L*4);
    size_t rope_len = cfg->sequence_len * (hd / 2);
    FP(g->rcos, rope_len*4); FP(g->rsin, rope_len*4);
    if (g->param_descs)
        alloc->free(alloc->ctx, g->param_descs,
                    g->param_desc_count * sizeof(hu_ml_tensor_t));
#undef FP
    alloc->free(alloc->ctx, g, sizeof(hu_gpt_t));
}

/* ─── RoPE precompute ───────────────────────────────────────────────────── */

static void precompute_rope(float *cos_buf, float *sin_buf, size_t seq_len, size_t head_dim)
{
    int half = (int)(head_dim / 2);
    for (size_t pos = 0; pos < seq_len; pos++)
        for (int d = 0; d < half; d++) {
            float theta = (float)pos * powf(10000.0f, -2.0f * (float)d / (float)head_dim);
            cos_buf[pos * half + d] = cosf(theta);
            sin_buf[pos * half + d] = sinf(theta);
        }
}

/* ─── Vtable + Create ───────────────────────────────────────────────────── */

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
    if (!alloc || !config || !out) return HU_ERR_INVALID_ARGUMENT;
    if (config->n_embd == 0 || config->vocab_size == 0 || config->n_layer == 0 ||
        config->n_head == 0 || config->head_dim == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (config->n_embd != config->n_head * config->head_dim)
        return HU_ERR_INVALID_ARGUMENT;

    hu_gpt_t *g = (hu_gpt_t *)alloc->alloc(alloc->ctx, sizeof(hu_gpt_t));
    if (!g) return HU_ERR_OUT_OF_MEMORY;
    memset(g, 0, sizeof(hu_gpt_t));
    g->alloc = alloc;
    g->config = *config;

    size_t V = config->vocab_size, E = config->n_embd, L = config->n_layer;
    size_t nh = config->n_head, nkv = config->n_kv_head, hd = config->head_dim;
    size_t nm = 4 * E;
    uint64_t seed = 42;
    float sc = 0.02f;

#define GA(p, sz) do { (p) = (float *)alloc->alloc(alloc->ctx, (sz)*sizeof(float)); if (!(p)) goto fail; } while(0)
#define GAP(p, n) do { (p) = (float **)alloc->alloc(alloc->ctx, (n)*sizeof(float*)); if (!(p)) goto fail; memset((p), 0, (n)*sizeof(float*)); } while(0)

    GA(g->wte, V*E); GA(g->lm_head, V*E);
    GA(g->grad_wte, V*E); GA(g->grad_lm_head, V*E);
    memset(g->grad_wte, 0, V*E*4); memset(g->grad_lm_head, 0, V*E*4);
    for (size_t i = 0; i < V*E; i++) { g->wte[i] = prng_next(&seed)*sc; g->lm_head[i] = prng_next(&seed)*sc; }

    GAP(g->aqw, L); GAP(g->akw, L); GAP(g->avw, L); GAP(g->aow, L);
    GAP(g->muw, L); GAP(g->mdw, L);
    GAP(g->grad_aqw, L); GAP(g->grad_akw, L); GAP(g->grad_avw, L); GAP(g->grad_aow, L);
    GAP(g->grad_muw, L); GAP(g->grad_mdw, L);

    for (size_t i = 0; i < L; i++) {
        size_t qsz = nh*hd*E, kvsz = nkv*hd*E, osz = E*nh*hd, usz = nm*E, dsz = E*nm;
        GA(g->aqw[i], qsz); GA(g->akw[i], kvsz); GA(g->avw[i], kvsz);
        GA(g->aow[i], osz); GA(g->muw[i], usz); GA(g->mdw[i], dsz);
        GA(g->grad_aqw[i], qsz); GA(g->grad_akw[i], kvsz); GA(g->grad_avw[i], kvsz);
        GA(g->grad_aow[i], osz); GA(g->grad_muw[i], usz); GA(g->grad_mdw[i], dsz);
        memset(g->grad_aqw[i], 0, qsz*4); memset(g->grad_akw[i], 0, kvsz*4);
        memset(g->grad_avw[i], 0, kvsz*4); memset(g->grad_aow[i], 0, osz*4);
        memset(g->grad_muw[i], 0, usz*4); memset(g->grad_mdw[i], 0, dsz*4);
        for (size_t j = 0; j < qsz; j++) g->aqw[i][j] = prng_next(&seed)*sc;
        for (size_t j = 0; j < kvsz; j++) g->akw[i][j] = prng_next(&seed)*sc;
        for (size_t j = 0; j < kvsz; j++) g->avw[i][j] = prng_next(&seed)*sc;
        for (size_t j = 0; j < osz; j++) g->aow[i][j] = prng_next(&seed)*sc;
        for (size_t j = 0; j < usz; j++) g->muw[i][j] = prng_next(&seed)*sc;
        for (size_t j = 0; j < dsz; j++) g->mdw[i][j] = prng_next(&seed)*sc;
    }

    GA(g->rl, L); GA(g->x0l, L);
    GA(g->grad_rl, L); GA(g->grad_x0l, L);
    memset(g->grad_rl, 0, L*4); memset(g->grad_x0l, 0, L*4);
    for (size_t i = 0; i < L; i++) {
        g->rl[i] = 0.9f + prng_next(&seed) * 0.1f;
        g->x0l[i] = 0.1f + prng_next(&seed) * 0.05f;
    }

    size_t rope_len = config->sequence_len * (hd / 2);
    GA(g->rcos, rope_len); GA(g->rsin, rope_len);
    precompute_rope(g->rcos, g->rsin, config->sequence_len, hd);

    g->total_params = V * E * 2;
    g->total_params += L * (nh*hd*E + nkv*hd*E*2 + E*nh*hd);
    g->total_params += L * (nm*E + E*nm);
    g->total_params += L * 2;

    /* Build param descriptor array for get_params / checkpoint */
    g->param_desc_count = 2 + L * 8;
    g->param_descs = (hu_ml_tensor_t *)alloc->alloc(alloc->ctx,
        g->param_desc_count * sizeof(hu_ml_tensor_t));
    if (!g->param_descs) goto fail;
    memset(g->param_descs, 0, g->param_desc_count * sizeof(hu_ml_tensor_t));
    {
        size_t pi = 0;
        g->param_descs[pi++] = (hu_ml_tensor_t){.data = g->wte, .size_bytes = V*E*4,
            .shape = {V, E, 0, 0}, .ndim = 2, .dtype = HU_ML_DTYPE_F32};
        g->param_descs[pi++] = (hu_ml_tensor_t){.data = g->lm_head, .size_bytes = V*E*4,
            .shape = {V, E, 0, 0}, .ndim = 2, .dtype = HU_ML_DTYPE_F32};
        for (size_t i = 0; i < L; i++) {
            g->param_descs[pi++] = (hu_ml_tensor_t){.data = g->aqw[i], .size_bytes = nh*hd*E*4,
                .shape = {nh*hd, E, 0, 0}, .ndim = 2, .dtype = HU_ML_DTYPE_F32};
            g->param_descs[pi++] = (hu_ml_tensor_t){.data = g->akw[i], .size_bytes = nkv*hd*E*4,
                .shape = {nkv*hd, E, 0, 0}, .ndim = 2, .dtype = HU_ML_DTYPE_F32};
            g->param_descs[pi++] = (hu_ml_tensor_t){.data = g->avw[i], .size_bytes = nkv*hd*E*4,
                .shape = {nkv*hd, E, 0, 0}, .ndim = 2, .dtype = HU_ML_DTYPE_F32};
            g->param_descs[pi++] = (hu_ml_tensor_t){.data = g->aow[i], .size_bytes = E*nh*hd*4,
                .shape = {E, nh*hd, 0, 0}, .ndim = 2, .dtype = HU_ML_DTYPE_F32};
            g->param_descs[pi++] = (hu_ml_tensor_t){.data = g->muw[i], .size_bytes = nm*E*4,
                .shape = {nm, E, 0, 0}, .ndim = 2, .dtype = HU_ML_DTYPE_F32};
            g->param_descs[pi++] = (hu_ml_tensor_t){.data = g->mdw[i], .size_bytes = E*nm*4,
                .shape = {E, nm, 0, 0}, .ndim = 2, .dtype = HU_ML_DTYPE_F32};
            g->param_descs[pi++] = (hu_ml_tensor_t){.data = &g->rl[i], .size_bytes = 4,
                .shape = {1, 1, 0, 0}, .ndim = 2, .dtype = HU_ML_DTYPE_F32};
            g->param_descs[pi++] = (hu_ml_tensor_t){.data = &g->x0l[i], .size_bytes = 4,
                .shape = {1, 1, 0, 0}, .ndim = 2, .dtype = HU_ML_DTYPE_F32};
        }
    }

#undef GA
#undef GAP

    out->ctx = g;
    out->vtable = &gpt_vtable;
    return HU_OK;

fail:
    gpt_deinit(g, alloc);
    return HU_ERR_OUT_OF_MEMORY;
}

/* ─── Register parameters with optimizer ─────────────────────────────────── */

hu_error_t hu_gpt_register_params(hu_model_t *model, struct hu_ml_optimizer *opt)
{
    if (!model || !opt || !model->ctx) return HU_ERR_INVALID_ARGUMENT;
    hu_gpt_t *g = (hu_gpt_t *)model->ctx;
    const hu_gpt_config_t *cfg = &g->config;
    size_t E = cfg->n_embd, V = cfg->vocab_size, L = cfg->n_layer;
    size_t nh = cfg->n_head, nkv = cfg->n_kv_head, hd = cfg->head_dim, nm = 4 * E;
    hu_error_t err;

    err = hu_muon_adamw_add_param(opt, g->wte, g->grad_wte, V, E, HU_PARAM_EMBEDDING);
    if (err != HU_OK) return err;
    err = hu_muon_adamw_add_param(opt, g->lm_head, g->grad_lm_head, V, E, HU_PARAM_UNEMBEDDING);
    if (err != HU_OK) return err;

    for (size_t i = 0; i < L; i++) {
        err = hu_muon_adamw_add_param(opt, g->aqw[i], g->grad_aqw[i], nh*hd, E, HU_PARAM_MATRIX);
        if (err != HU_OK) return err;
        err = hu_muon_adamw_add_param(opt, g->akw[i], g->grad_akw[i], nkv*hd, E, HU_PARAM_MATRIX);
        if (err != HU_OK) return err;
        err = hu_muon_adamw_add_param(opt, g->avw[i], g->grad_avw[i], nkv*hd, E, HU_PARAM_MATRIX);
        if (err != HU_OK) return err;
        err = hu_muon_adamw_add_param(opt, g->aow[i], g->grad_aow[i], E, nh*hd, HU_PARAM_MATRIX);
        if (err != HU_OK) return err;
        err = hu_muon_adamw_add_param(opt, g->muw[i], g->grad_muw[i], nm, E, HU_PARAM_MATRIX);
        if (err != HU_OK) return err;
        err = hu_muon_adamw_add_param(opt, g->mdw[i], g->grad_mdw[i], E, nm, HU_PARAM_MATRIX);
        if (err != HU_OK) return err;
        err = hu_muon_adamw_add_param(opt, &g->rl[i], &g->grad_rl[i], 1, 1, HU_PARAM_SCALAR);
        if (err != HU_OK) return err;
        err = hu_muon_adamw_add_param(opt, &g->x0l[i], &g->grad_x0l[i], 1, 1, HU_PARAM_SCALAR);
        if (err != HU_OK) return err;
    }
    return HU_OK;
}
