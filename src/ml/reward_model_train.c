/* src/ml/reward_model_train.c — Bradley-Terry training loop
 *
 * SGD training of the value head using Bradley-Terry preference loss.
 * The backbone is frozen per Christiano 2017 §2.2.
 */
#include "human/ml/reward_model.h"
#include "reward_model_priv.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Bradley-Terry loss computation:
 *   r_w = score(prompt, chosen)
 *   r_l = score(prompt, rejected)
 *   delta = r_w - r_l
 *   sigmoid(x) = 1.0 / (1.0 + exp(-x))
 *   loss = -log(sigmoid(delta))
 *        = log(1.0 + exp(-delta))
 *
 * Gradients:
 *   dL/dr_w = sigmoid(delta) - 1.0
 *   dL/dr_l = 1.0 - sigmoid(delta)
 */

static double sigmoid(double x) {
    /* Numerically stable sigmoid */
    if (x > 20.0)
        return 1.0;
    if (x < -20.0)
        return 0.0;
    return 1.0 / (1.0 + exp(-x));
}

static double bradley_terry_loss(double r_w, double r_l) {
    double delta = r_w - r_l;
    /* log(1 + exp(-delta)) is numerically stable */
    if (delta > 20.0)
        return 0.0;
    if (delta < -20.0)
        return -delta;
    return log(1.0 + exp(-delta));
}

hu_error_t hu_reward_model_train(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                 const hu_preference_pair_t *pairs, size_t n,
                                 const hu_reward_model_train_config_t *config,
                                 hu_reward_model_train_metrics_t *out_metrics) {
    if (!rm || !alloc || !pairs || n == 0 || !config || !out_metrics)
        return HU_ERR_INVALID_ARGUMENT;

    if (config->max_iters == 0 || config->learning_rate <= 0.0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Check backend is HUML */
    if (strcmp(rm->vtable->name(rm->ctx), "reward_model_huml") != 0)
        return HU_ERR_NOT_SUPPORTED;

    huml_rm_ctx_t *ctx = (huml_rm_ctx_t *)rm->ctx;

    /* Compute initial loss (forward pass only, no updates) */
    double initial_loss = 0.0;
    size_t skipped_count = 0;

    for (size_t i = 0; i < n; i++) {
        /* Skip one-sided pairs */
        if (pairs[i].chosen_len == 0 || pairs[i].rejected_len == 0) {
            skipped_count++;
            continue;
        }

        double r_w, r_l;
        hu_error_t err = rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                           pairs[i].chosen, pairs[i].chosen_len, &r_w);
        if (err != HU_OK)
            return err;

        err = rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                pairs[i].rejected, pairs[i].rejected_len, &r_l);
        if (err != HU_OK)
            return err;

        double loss = bradley_terry_loss(r_w, r_l);
        initial_loss += loss;
    }

    size_t valid_pairs = n - skipped_count;
    if (valid_pairs == 0) {
        /* All pairs were skipped */
        memset(out_metrics, 0, sizeof(*out_metrics));
        out_metrics->skipped_count = skipped_count;
        if (skipped_count > 0)
            fprintf(stderr, "WARNING: skipped %zu one-sided KTO pairs during training\n",
                    skipped_count);
        return HU_OK;
    }

    initial_loss /= (double)valid_pairs;

    /* SGD training loop: numerical gradient on value head weights */
    for (size_t iter = 0; iter < config->max_iters; iter++) {
        double total_dW_sum[256] = {0};
        double total_db_sum = 0.0;
        size_t updated_pairs = 0;

        /* For each weight, compute numerical gradient via finite difference */
        const double eps = 1e-4;

        /* Compute current loss (baseline) */
        double baseline_loss = 0.0;
        for (size_t i = 0; i < n; i++) {
            if (pairs[i].chosen_len == 0 || pairs[i].rejected_len == 0)
                continue;

            double r_w, r_l;
            hu_error_t err = rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                               pairs[i].chosen, pairs[i].chosen_len, &r_w);
            if (err != HU_OK)
                return err;

            err = rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                    pairs[i].rejected, pairs[i].rejected_len, &r_l);
            if (err != HU_OK)
                return err;

            double loss = bradley_terry_loss(r_w, r_l);
            baseline_loss += loss;
            updated_pairs++;
        }

        if (updated_pairs > 0)
            baseline_loss /= (double)updated_pairs;

        /* Gradient for each weight W[j] using numerical approximation */
        for (size_t j = 0; j < ctx->value_head.hidden_dim; j++) {
            /* Perturb W[j] by +eps */
            float saved_W = ctx->value_head.W[j];
            ctx->value_head.W[j] = saved_W + (float)eps;

            double perturbed_loss = 0.0;
            size_t valid_count = 0;
            for (size_t i = 0; i < n; i++) {
                if (pairs[i].chosen_len == 0 || pairs[i].rejected_len == 0)
                    continue;

                double r_w, r_l;
                hu_error_t err =
                    rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                      pairs[i].chosen, pairs[i].chosen_len, &r_w);
                if (err != HU_OK) {
                    ctx->value_head.W[j] = saved_W;
                    return err;
                }

                err = rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                        pairs[i].rejected, pairs[i].rejected_len, &r_l);
                if (err != HU_OK) {
                    ctx->value_head.W[j] = saved_W;
                    return err;
                }

                double loss = bradley_terry_loss(r_w, r_l);
                perturbed_loss += loss;
                valid_count++;
            }

            if (valid_count > 0)
                perturbed_loss /= (double)valid_count;

            /* Numerical gradient: dL/dW[j] ≈ (L(W[j] + eps) - L(W[j])) / eps */
            double grad_W_j = (perturbed_loss - baseline_loss) / eps;
            total_dW_sum[j] += grad_W_j;

            /* Restore weight */
            ctx->value_head.W[j] = saved_W;
        }

        /* Gradient for bias b using numerical approximation */
        float saved_b = ctx->value_head.b;
        ctx->value_head.b = saved_b + (float)eps;

        double perturbed_loss_b = 0.0;
        updated_pairs = 0;
        for (size_t i = 0; i < n; i++) {
            if (pairs[i].chosen_len == 0 || pairs[i].rejected_len == 0)
                continue;

            double r_w, r_l;
            hu_error_t err = rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                               pairs[i].chosen, pairs[i].chosen_len, &r_w);
            if (err != HU_OK) {
                ctx->value_head.b = saved_b;
                return err;
            }

            err = rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                    pairs[i].rejected, pairs[i].rejected_len, &r_l);
            if (err != HU_OK) {
                ctx->value_head.b = saved_b;
                return err;
            }

            double loss = bradley_terry_loss(r_w, r_l);
            perturbed_loss_b += loss;
            updated_pairs++;
        }

        if (updated_pairs > 0)
            perturbed_loss_b /= (double)updated_pairs;

        double grad_b = (perturbed_loss_b - baseline_loss) / eps;
        total_db_sum += grad_b;

        ctx->value_head.b = saved_b;

        /* Apply SGD update */
        if (updated_pairs > 0) {
            double lr = config->learning_rate;
            for (size_t j = 0; j < ctx->value_head.hidden_dim; j++) {
                ctx->value_head.W[j] -= (float)(lr * total_dW_sum[j]);
            }
            ctx->value_head.b -= (float)(lr * total_db_sum);
        }
    }

    /* Compute final loss */
    double final_loss = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (pairs[i].chosen_len == 0 || pairs[i].rejected_len == 0)
            continue;

        double r_w, r_l;
        hu_error_t err = rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                           pairs[i].chosen, pairs[i].chosen_len, &r_w);
        if (err != HU_OK)
            return err;

        err = rm->vtable->score(rm->ctx, alloc, pairs[i].prompt, pairs[i].prompt_len,
                                pairs[i].rejected, pairs[i].rejected_len, &r_l);
        if (err != HU_OK)
            return err;

        double loss = bradley_terry_loss(r_w, r_l);
        final_loss += loss;
    }

    final_loss /= (double)valid_pairs;

    /* Populate metrics */
    out_metrics->initial_loss = initial_loss;
    out_metrics->final_loss = final_loss;
    out_metrics->iters_completed = config->max_iters;
    out_metrics->skipped_count = skipped_count;

    if (skipped_count > 0)
        fprintf(stderr, "WARNING: skipped %zu one-sided KTO pairs during training\n",
                skipped_count);

    return HU_OK;
}

/* Test helper: compute Bradley-Terry loss without training. */
hu_error_t reward_model_compute_bt_loss_only_for_test(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                                      const hu_preference_pair_t *pairs, size_t n,
                                                      double *out_loss) {
    if (!rm || !alloc || !pairs || n == 0 || !out_loss)
        return HU_ERR_INVALID_ARGUMENT;

    double total_loss = 0.0;
    size_t valid_pairs = 0;

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

        double loss = bradley_terry_loss(r_w, r_l);
        total_loss += loss;
        valid_pairs++;
    }

    if (valid_pairs == 0)
        *out_loss = 0.0;
    else
        *out_loss = total_loss / (double)valid_pairs;

    return HU_OK;
}

/* Test helper: expose value head W for finite-difference gradient checks. */
float *reward_model_huml_value_head_W_for_test(hu_reward_model_t *rm) {
    if (!rm || !rm->vtable || strcmp(rm->vtable->name(rm->ctx), "reward_model_huml") != 0)
        return NULL;
    huml_rm_ctx_t *ctx = (huml_rm_ctx_t *)rm->ctx;
    return ctx->value_head.W;
}
