/* src/ml/reward_model_train.c — Bradley-Terry training loop
 *
 * SGD training of the value head using Bradley-Terry preference loss.
 * The backbone is frozen per Christiano 2017 §2.2.
 *
 * Gradients are ANALYTICAL: the value head is linear (score = W·h + b), so
 *   dL/dW = dL/dscore * h,  dL/db = dL/dscore
 * computed via hu_value_head_backward on the last-position hidden vector h
 * returned by reward_model_huml_score_hidden. (An earlier version used
 * O(hidden_dim) finite-difference forward passes per step — correct but
 * quadratic and untestable; the analytical path is checked against finite
 * differences in tests/test_reward_model_huml.c, AC-101.4.)
 */
#include "human/ml/reward_model.h"
#include "reward_model_priv.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * Bradley-Terry:
 *   delta = r_w - r_l
 *   loss  = -log(sigmoid(delta)) = log(1 + exp(-delta))
 *   dL/dr_w = sigmoid(delta) - 1.0
 *   dL/dr_l = 1.0 - sigmoid(delta)
 */
static double sigmoid(double x) {
    if (x > 20.0)
        return 1.0;
    if (x < -20.0)
        return 0.0;
    return 1.0 / (1.0 + exp(-x));
}

static double bradley_terry_loss(double r_w, double r_l) {
    double delta = r_w - r_l;
    if (delta > 20.0)
        return 0.0;
    if (delta < -20.0)
        return -delta;
    return log(1.0 + exp(-delta));
}

/* Accumulate the analytical gradient of the SUMMED Bradley-Terry loss over all
 * valid (two-sided) pairs into out_dW[hidden_dim] / *out_db, and return the
 * number of valid pairs in *out_valid. Scratch buffers h_w/h_l/dW_w/dW_l are
 * caller-provided (all float[hidden_dim]). Caller divides by *out_valid to get
 * the mean-loss gradient. */
static hu_error_t accumulate_bt_grad(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                     const hu_preference_pair_t *pairs, size_t n, size_t hidden_dim,
                                     float *h_w, float *h_l, float *dW_w, float *dW_l,
                                     double *out_dW, double *out_db, size_t *out_valid) {
    huml_rm_ctx_t *ctx = (huml_rm_ctx_t *)rm->ctx;
    for (size_t j = 0; j < hidden_dim; j++)
        out_dW[j] = 0.0;
    *out_db = 0.0;
    size_t valid = 0;

    for (size_t i = 0; i < n; i++) {
        if (pairs[i].chosen_len == 0 || pairs[i].rejected_len == 0)
            continue;

        double r_w = 0.0, r_l = 0.0;
        hu_error_t err =
            reward_model_huml_score_hidden(rm, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                           pairs[i].chosen, pairs[i].chosen_len, &r_w, h_w);
        if (err != HU_OK)
            return err;
        err = reward_model_huml_score_hidden(rm, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                             pairs[i].rejected, pairs[i].rejected_len, &r_l, h_l);
        if (err != HU_OK)
            return err;

        double sig = sigmoid(r_w - r_l);
        double dL_dr_w = sig - 1.0;
        double dL_dr_l = 1.0 - sig;

        float db_w = 0.0f, db_l = 0.0f;
        err = hu_value_head_backward(&ctx->value_head, h_w, dL_dr_w, dW_w, &db_w, NULL);
        if (err != HU_OK)
            return err;
        err = hu_value_head_backward(&ctx->value_head, h_l, dL_dr_l, dW_l, &db_l, NULL);
        if (err != HU_OK)
            return err;

        for (size_t j = 0; j < hidden_dim; j++)
            out_dW[j] += (double)dW_w[j] + (double)dW_l[j];
        *out_db += (double)db_w + (double)db_l;
        valid++;
    }

    *out_valid = valid;
    return HU_OK;
}

/* Mean Bradley-Terry loss over valid pairs (forward only). */
static hu_error_t mean_bt_loss(hu_reward_model_t *rm, hu_allocator_t *alloc,
                               const hu_preference_pair_t *pairs, size_t n, double *out_loss,
                               size_t *out_valid) {
    double sum = 0.0;
    size_t valid = 0;
    for (size_t i = 0; i < n; i++) {
        if (pairs[i].chosen_len == 0 || pairs[i].rejected_len == 0)
            continue;
        double r_w = 0.0, r_l = 0.0;
        hu_error_t err = rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                           pairs[i].chosen, pairs[i].chosen_len, &r_w);
        if (err != HU_OK)
            return err;
        err = rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                pairs[i].rejected, pairs[i].rejected_len, &r_l);
        if (err != HU_OK)
            return err;
        sum += bradley_terry_loss(r_w, r_l);
        valid++;
    }
    *out_valid = valid;
    *out_loss = (valid > 0) ? sum / (double)valid : 0.0;
    return HU_OK;
}

hu_error_t hu_reward_model_train(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                 const hu_preference_pair_t *pairs, size_t n,
                                 const hu_reward_model_train_config_t *config,
                                 hu_reward_model_train_metrics_t *out_metrics) {
    if (!rm || !alloc || !pairs || n == 0 || !config || !out_metrics)
        return HU_ERR_INVALID_ARGUMENT;
    if (config->max_iters == 0 || config->learning_rate <= 0.0)
        return HU_ERR_INVALID_ARGUMENT;
    if (strcmp(rm->vtable->name(rm->ctx), "reward_model_huml") != 0)
        return HU_ERR_NOT_SUPPORTED;

    huml_rm_ctx_t *ctx = (huml_rm_ctx_t *)rm->ctx;
    size_t V = ctx->value_head.hidden_dim;

    size_t skipped_count = 0;
    for (size_t i = 0; i < n; i++)
        if (pairs[i].chosen_len == 0 || pairs[i].rejected_len == 0)
            skipped_count++;

    double initial_loss = 0.0;
    size_t valid_pairs = 0;
    hu_error_t err = mean_bt_loss(rm, alloc, pairs, n, &initial_loss, &valid_pairs);
    if (err != HU_OK)
        return err;

    if (valid_pairs == 0) {
        memset(out_metrics, 0, sizeof(*out_metrics));
        out_metrics->skipped_count = skipped_count;
        if (skipped_count > 0)
            fprintf(stderr, "WARNING: skipped %zu one-sided KTO pairs during training\n",
                    skipped_count);
        return HU_OK;
    }

    /* Scratch buffers sized to the actual hidden_dim (no fixed [256] cap). */
    float *h_w = (float *)alloc->alloc(alloc->ctx, V * sizeof(float));
    float *h_l = (float *)alloc->alloc(alloc->ctx, V * sizeof(float));
    float *dW_w = (float *)alloc->alloc(alloc->ctx, V * sizeof(float));
    float *dW_l = (float *)alloc->alloc(alloc->ctx, V * sizeof(float));
    double *total_dW = (double *)alloc->alloc(alloc->ctx, V * sizeof(double));
    if (!h_w || !h_l || !dW_w || !dW_l || !total_dW) {
        alloc->free(alloc->ctx, h_w, V * sizeof(float));
        alloc->free(alloc->ctx, h_l, V * sizeof(float));
        alloc->free(alloc->ctx, dW_w, V * sizeof(float));
        alloc->free(alloc->ctx, dW_l, V * sizeof(float));
        alloc->free(alloc->ctx, total_dW, V * sizeof(double));
        return HU_ERR_OUT_OF_MEMORY;
    }

    double lr = config->learning_rate;
    for (size_t iter = 0; iter < config->max_iters; iter++) {
        double total_db = 0.0;
        size_t valid = 0;
        err = accumulate_bt_grad(rm, alloc, pairs, n, V, h_w, h_l, dW_w, dW_l, total_dW, &total_db,
                                 &valid);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, h_w, V * sizeof(float));
            alloc->free(alloc->ctx, h_l, V * sizeof(float));
            alloc->free(alloc->ctx, dW_w, V * sizeof(float));
            alloc->free(alloc->ctx, dW_l, V * sizeof(float));
            alloc->free(alloc->ctx, total_dW, V * sizeof(double));
            return err;
        }
        if (valid > 0) {
            for (size_t j = 0; j < V; j++)
                ctx->value_head.W[j] -= (float)(lr * total_dW[j] / (double)valid);
            ctx->value_head.b -= (float)(lr * total_db / (double)valid);
        }
    }

    alloc->free(alloc->ctx, h_w, V * sizeof(float));
    alloc->free(alloc->ctx, h_l, V * sizeof(float));
    alloc->free(alloc->ctx, dW_w, V * sizeof(float));
    alloc->free(alloc->ctx, dW_l, V * sizeof(float));
    alloc->free(alloc->ctx, total_dW, V * sizeof(double));

    double final_loss = 0.0;
    size_t fv = 0;
    err = mean_bt_loss(rm, alloc, pairs, n, &final_loss, &fv);
    if (err != HU_OK)
        return err;

    out_metrics->initial_loss = initial_loss;
    out_metrics->final_loss = final_loss;
    out_metrics->iters_completed = config->max_iters;
    out_metrics->skipped_count = skipped_count;

    if (skipped_count > 0)
        fprintf(stderr, "WARNING: skipped %zu one-sided KTO pairs during training\n",
                skipped_count);

    return HU_OK;
}

/* Test helper: mean Bradley-Terry loss without training. */
hu_error_t reward_model_compute_bt_loss_only_for_test(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                                      const hu_preference_pair_t *pairs, size_t n,
                                                      double *out_loss) {
    if (!rm || !alloc || !pairs || n == 0 || !out_loss)
        return HU_ERR_INVALID_ARGUMENT;
    size_t valid = 0;
    return mean_bt_loss(rm, alloc, pairs, n, out_loss, &valid);
}

/* Test helper: analytical mean-loss gradient w.r.t. the value-head weights,
 * averaged over valid pairs (matches reward_model_compute_bt_loss_only_for_test
 * so the AC-101.4 gradient check can compare against central finite differences
 * of that same mean loss). out_dW is caller-allocated float[hidden_dim]. */
hu_error_t reward_model_compute_bt_grad_for_test(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                                 const hu_preference_pair_t *pairs, size_t n,
                                                 float *out_dW, double *out_db) {
    if (!rm || !alloc || !pairs || n == 0 || !out_dW || !out_db)
        return HU_ERR_INVALID_ARGUMENT;
    if (strcmp(rm->vtable->name(rm->ctx), "reward_model_huml") != 0)
        return HU_ERR_NOT_SUPPORTED;

    huml_rm_ctx_t *ctx = (huml_rm_ctx_t *)rm->ctx;
    size_t V = ctx->value_head.hidden_dim;

    float *h_w = (float *)alloc->alloc(alloc->ctx, V * sizeof(float));
    float *h_l = (float *)alloc->alloc(alloc->ctx, V * sizeof(float));
    float *dW_w = (float *)alloc->alloc(alloc->ctx, V * sizeof(float));
    float *dW_l = (float *)alloc->alloc(alloc->ctx, V * sizeof(float));
    double *acc = (double *)alloc->alloc(alloc->ctx, V * sizeof(double));
    if (!h_w || !h_l || !dW_w || !dW_l || !acc) {
        alloc->free(alloc->ctx, h_w, V * sizeof(float));
        alloc->free(alloc->ctx, h_l, V * sizeof(float));
        alloc->free(alloc->ctx, dW_w, V * sizeof(float));
        alloc->free(alloc->ctx, dW_l, V * sizeof(float));
        alloc->free(alloc->ctx, acc, V * sizeof(double));
        return HU_ERR_OUT_OF_MEMORY;
    }

    double db = 0.0;
    size_t valid = 0;
    hu_error_t err =
        accumulate_bt_grad(rm, alloc, pairs, n, V, h_w, h_l, dW_w, dW_l, acc, &db, &valid);
    if (err == HU_OK) {
        double inv = (valid > 0) ? 1.0 / (double)valid : 0.0;
        for (size_t j = 0; j < V; j++)
            out_dW[j] = (float)(acc[j] * inv);
        *out_db = db * inv;
    }

    alloc->free(alloc->ctx, h_w, V * sizeof(float));
    alloc->free(alloc->ctx, h_l, V * sizeof(float));
    alloc->free(alloc->ctx, dW_w, V * sizeof(float));
    alloc->free(alloc->ctx, dW_l, V * sizeof(float));
    alloc->free(alloc->ctx, acc, V * sizeof(double));
    return err;
}

/* Test helper: expose value head W for finite-difference gradient checks. */
float *reward_model_huml_value_head_W_for_test(hu_reward_model_t *rm) {
    if (!rm || !rm->vtable || strcmp(rm->vtable->name(rm->ctx), "reward_model_huml") != 0)
        return NULL;
    huml_rm_ctx_t *ctx = (huml_rm_ctx_t *)rm->ctx;
    return ctx->value_head.W;
}
