/* MuonAdamW optimizer — AdamW for scalar/embedding, Muon for 2D matrices.
 * Ported from autoresearch train.py. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

#define ADAM_EPS 1e-8f
#define MUON_NESTEROV_BETA 0.95f

/* ─── Internal structs ────────────────────────────────────────────────────── */

typedef struct hu_param_group {
    hu_param_kind_t kind;
    float *param;
    float *grad;
    size_t rows;
    size_t cols;
    size_t numel;
    float *exp_avg;
    float *exp_avg_sq;
    int step_count;
    float *momentum_buf;
} hu_param_group_t;

typedef struct hu_muon_adamw {
    hu_allocator_t *alloc;
    hu_optimizer_config_t config;
    hu_param_group_t *groups;
    size_t group_count;
    size_t group_capacity;
    float lr_multiplier;
} hu_muon_adamw_t;

/* ─── LR schedule ───────────────────────────────────────────────────────── */

float hu_ml_lr_schedule(float progress, float warmup_ratio, float warmdown_ratio,
                        float final_lr_frac)
{
    if (progress <= 0.0f)
        return 0.0f;
    if (progress >= 1.0f)
        return final_lr_frac;

    if (progress < warmup_ratio) {
        return progress / warmup_ratio;
    }
    if (progress > (1.0f - warmdown_ratio)) {
        float warmdown_progress = (progress - (1.0f - warmdown_ratio)) / warmdown_ratio;
        return 1.0f - warmdown_progress * (1.0f - final_lr_frac);
    }
    return 1.0f;
}

/* ─── AdamW step (embedding, unembedding, scalar) ─────────────────────────── */

static void step_adamw(hu_param_group_t *g, const hu_optimizer_config_t *cfg,
                       float lr, float wd)
{
    const float beta1 = cfg->adam_beta1;
    const float beta2 = cfg->adam_beta2;
    const float eps = ADAM_EPS;
    const size_t n = g->numel;

    g->step_count++;
    const int t = g->step_count;
    const float bias1 = 1.0f - powf(beta1, (float)t);
    const float bias2 = 1.0f - powf(beta2, (float)t);

    for (size_t i = 0; i < n; i++) {
        float grad_val = g->grad[i];
        g->exp_avg[i] = beta1 * g->exp_avg[i] + (1.0f - beta1) * grad_val;
        g->exp_avg_sq[i] = beta2 * g->exp_avg_sq[i] + (1.0f - beta2) * grad_val * grad_val;

        float denom = sqrtf(g->exp_avg_sq[i] / bias2) + eps;
        float step_size = lr / bias1;
        g->param[i] -= step_size * g->exp_avg[i] / denom;
    }

    /* Weight decay for non-embedding params (scalar only; embedding/unembedding skip) */
    if (g->kind == HU_PARAM_SCALAR && wd > 0.0f) {
        for (size_t i = 0; i < n; i++)
            g->param[i] *= (1.0f - lr * wd);
    }
}

/* ─── Muon step (2D matrices) ──────────────────────────────────────────────── */

static hu_error_t step_muon(hu_param_group_t *g, const hu_optimizer_config_t *cfg,
                            float lr, float wd, hu_allocator_t *alloc)
{
    const float beta = MUON_NESTEROV_BETA;
    const size_t rows = g->rows;
    const size_t cols = g->cols;
    const size_t n = g->numel;

    (void)cfg;

    for (size_t i = 0; i < n; i++) {
        float grad_val = g->grad[i];
        g->momentum_buf[i] = beta * g->momentum_buf[i] + (1.0f - beta) * grad_val;
    }

    float *g_buf = (float *)alloc->alloc(alloc->ctx, n * sizeof(float));
    if (!g_buf)
        return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < n; i++)
        g_buf[i] = g->grad[i] + beta * g->momentum_buf[i];

    /* Row-normalize g (simplified orthogonalization) */
    for (size_t r = 0; r < rows; r++) {
        float *row = g_buf + r * cols;
        float norm_sq = 0.0f;
        for (size_t c = 0; c < cols; c++)
            norm_sq += row[c] * row[c];
        float norm = sqrtf(norm_sq) + 1e-12f;
        for (size_t c = 0; c < cols; c++)
            row[c] /= norm;
    }

    /* param -= lr * g */
    for (size_t i = 0; i < n; i++)
        g->param[i] -= lr * g_buf[i];

    /* Cautious weight decay: mask = (g * param >= 0); param -= lr * wd * param * mask */
    if (wd > 0.0f) {
        for (size_t i = 0; i < n; i++) {
            float mask = (g_buf[i] * g->param[i] >= 0.0f) ? 1.0f : 0.0f;
            g->param[i] -= lr * wd * g->param[i] * mask;
        }
    }

    alloc->free(alloc->ctx, g_buf, n * sizeof(float));
    return HU_OK;
}

/* ─── Vtable implementations ─────────────────────────────────────────────── */

static hu_error_t muon_adamw_step(void *ctx, hu_ml_tensor_t *params,
                                  const hu_ml_tensor_t *grads, size_t count)
{
    (void)params;
    (void)grads;
    (void)count;

    hu_muon_adamw_t *m = (hu_muon_adamw_t *)ctx;
    const hu_optimizer_config_t *cfg = &m->config;
    float mult = m->lr_multiplier;

    for (size_t i = 0; i < m->group_count; i++) {
        hu_param_group_t *g = &m->groups[i];
        float lr;
        float wd = cfg->weight_decay * mult;

        switch (g->kind) {
        case HU_PARAM_EMBEDDING:
            lr = cfg->embedding_lr * mult;
            step_adamw(g, cfg, lr, 0.0f);
            break;
        case HU_PARAM_UNEMBEDDING:
            lr = cfg->unembedding_lr * mult;
            step_adamw(g, cfg, lr, 0.0f);
            break;
        case HU_PARAM_MATRIX:
            lr = cfg->matrix_lr * mult;
            {
                hu_error_t err = step_muon(g, cfg, lr, wd, m->alloc);
                if (err != HU_OK) return err;
            }
            break;
        case HU_PARAM_SCALAR:
            lr = cfg->scalar_lr * mult;
            step_adamw(g, cfg, lr, wd);
            break;
        }
    }
    return HU_OK;
}

static void muon_adamw_zero_grad(void *ctx)
{
    hu_muon_adamw_t *m = (hu_muon_adamw_t *)ctx;
    for (size_t i = 0; i < m->group_count; i++) {
        hu_param_group_t *g = &m->groups[i];
        memset(g->grad, 0, g->numel * sizeof(float));
    }
}

static void muon_adamw_set_lr_multiplier(void *ctx, float multiplier)
{
    ((hu_muon_adamw_t *)ctx)->lr_multiplier = multiplier;
}

static void muon_adamw_deinit(void *ctx, hu_allocator_t *alloc)
{
    hu_muon_adamw_t *m = (hu_muon_adamw_t *)ctx;
    if (!m)
        return;

    for (size_t i = 0; i < m->group_count; i++) {
        hu_param_group_t *g = &m->groups[i];
        if (g->exp_avg)
            alloc->free(alloc->ctx, g->exp_avg, g->numel * sizeof(float));
        if (g->exp_avg_sq)
            alloc->free(alloc->ctx, g->exp_avg_sq, g->numel * sizeof(float));
        if (g->momentum_buf)
            alloc->free(alloc->ctx, g->momentum_buf, g->numel * sizeof(float));
    }
    if (m->groups)
        alloc->free(alloc->ctx, m->groups, m->group_capacity * sizeof(hu_param_group_t));
}

static const hu_ml_optimizer_vtable_t muon_adamw_vtable = {
    .step = muon_adamw_step,
    .zero_grad = muon_adamw_zero_grad,
    .set_lr_multiplier = muon_adamw_set_lr_multiplier,
    .deinit = muon_adamw_deinit,
};

/* ─── Public API ─────────────────────────────────────────────────────────── */

hu_error_t hu_muon_adamw_create(hu_allocator_t *alloc,
                                const hu_optimizer_config_t *config,
                                hu_ml_optimizer_t *out)
{
    if (!alloc || !config || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_muon_adamw_t *m = (hu_muon_adamw_t *)alloc->alloc(alloc->ctx, sizeof(hu_muon_adamw_t));
    if (!m)
        return HU_ERR_OUT_OF_MEMORY;

    m->alloc = alloc;
    m->config = *config;
    m->groups = NULL;
    m->group_count = 0;
    m->group_capacity = 0;
    m->lr_multiplier = 1.0f;

    out->ctx = m;
    out->vtable = &muon_adamw_vtable;
    return HU_OK;
}

hu_error_t hu_muon_adamw_add_param(hu_ml_optimizer_t *opt, float *param,
                                   float *grad, size_t rows, size_t cols,
                                   hu_param_kind_t kind)
{
    if (!opt || !opt->ctx || !opt->vtable || !param || !grad)
        return HU_ERR_INVALID_ARGUMENT;

    hu_muon_adamw_t *m = (hu_muon_adamw_t *)opt->ctx;
    if (opt->vtable != &muon_adamw_vtable)
        return HU_ERR_INVALID_ARGUMENT;

    size_t numel = rows * cols;
    if (numel == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Grow groups array if needed */
    if (m->group_count >= m->group_capacity) {
        size_t new_cap = (m->group_capacity == 0) ? 8 : m->group_capacity * 2;
        hu_param_group_t *new_groups = (hu_param_group_t *)m->alloc->realloc(
            m->alloc->ctx, m->groups,
            m->group_capacity * sizeof(hu_param_group_t),
            new_cap * sizeof(hu_param_group_t));
        if (!new_groups)
            return HU_ERR_OUT_OF_MEMORY;
        m->groups = new_groups;
        m->group_capacity = new_cap;
    }

    hu_param_group_t *g = &m->groups[m->group_count];
    g->kind = kind;
    g->param = param;
    g->grad = grad;
    g->rows = rows;
    g->cols = cols;
    g->numel = numel;
    g->step_count = 0;

    g->exp_avg = (float *)m->alloc->alloc(m->alloc->ctx, numel * sizeof(float));
    g->exp_avg_sq = (float *)m->alloc->alloc(m->alloc->ctx, numel * sizeof(float));
    if (!g->exp_avg || !g->exp_avg_sq) {
        if (g->exp_avg)
            m->alloc->free(m->alloc->ctx, g->exp_avg, numel * sizeof(float));
        if (g->exp_avg_sq)
            m->alloc->free(m->alloc->ctx, g->exp_avg_sq, numel * sizeof(float));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(g->exp_avg, 0, numel * sizeof(float));
    memset(g->exp_avg_sq, 0, numel * sizeof(float));

    g->momentum_buf = NULL;
    if (kind == HU_PARAM_MATRIX) {
        g->momentum_buf = (float *)m->alloc->alloc(m->alloc->ctx, numel * sizeof(float));
        if (!g->momentum_buf) {
            m->alloc->free(m->alloc->ctx, g->exp_avg, numel * sizeof(float));
            m->alloc->free(m->alloc->ctx, g->exp_avg_sq, numel * sizeof(float));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(g->momentum_buf, 0, numel * sizeof(float));
    }

    m->group_count++;
    return HU_OK;
}
