/* src/ml/kto.c — Phase 3 Task 4
 *
 * KTO (Kahneman-Tversky Optimization) — Ethayarajh et al. 2024.
 * https://arxiv.org/abs/2402.01306
 *
 * Single-signal preference optimization. Unlike DPO, KTO does not
 * require paired (chosen, rejected) — one-sided signals are valid.
 *
 * Loss (simplified, z_ref = 0):
 *   Desirable:   L_D = lambda_D * (1 - sigma(beta * (logpi_theta - logpi_ref)))
 *   Undesirable: L_U = lambda_U * (1 - sigma(beta * (logpi_ref - logpi_theta)))
 *
 * Data shape via hu_preference_pair_t one-sided convention:
 *   desirable:   chosen_len > 0 && rejected_len == 0
 *   undesirable: chosen_len == 0 && rejected_len > 0
 *   two-sided:   both populated — silently skipped (route to DPO)
 *   neither:     HU_ERR_INVALID_ARGUMENT
 *
 * Pattern mirrors src/ml/dpo_real_huml.c — toy GPT backbone, same
 * allocator discipline (3-arg free with exact size tracking).
 */
#include "human/ml/kto.h"
#include "human/ml/policy_logprobs.h"
#include "human/ml/reference_model.h"
#include "human/ml/model.h"
#include "human/ml/ml.h"
#include "human/core/error.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    hu_model_t policy;
    hu_model_t reference;
    hu_gpt_config_t gpt_cfg;
    double beta;
    double learning_rate;
    double lambda_d;
    double lambda_u;
    int initialized;
} kto_huml_ctx_t;

/* Tokenize space-separated int-id string. Same pattern as dpo_real_huml.c.
 * Returns allocated cap via *out_cap for size-aware free. */
static hu_error_t parse_id_string(hu_allocator_t *alloc, const char *s,
                                  int32_t **out, size_t *out_n, size_t *out_cap) {
    if (!s) return HU_ERR_INVALID_ARGUMENT;
    size_t cap = 16, n = 0;
    int32_t *buf = (int32_t *)alloc->alloc(alloc->ctx, cap * sizeof(int32_t));
    if (!buf) return HU_ERR_OUT_OF_MEMORY;
    const char *p = s;
    while (*p) {
        char *endp = NULL;
        long v = strtol(p, &endp, 10);
        if (endp == p) break;
        if (n == cap) {
            size_t old_cap = cap;
            cap *= 2;
            int32_t *nb = (int32_t *)alloc->alloc(alloc->ctx, cap * sizeof(int32_t));
            if (!nb) {
                alloc->free(alloc->ctx, buf, old_cap * sizeof(int32_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(nb, buf, n * sizeof(int32_t));
            alloc->free(alloc->ctx, buf, old_cap * sizeof(int32_t));
            buf = nb;
        }
        buf[n++] = (int32_t)v;
        p = endp;
        while (*p == ' ' || *p == '\t') p++;
    }
    *out = buf;
    *out_n = n;
    if (out_cap) *out_cap = cap;
    return HU_OK;
}

static hu_error_t kto_huml_step(void *vctx, hu_allocator_t *alloc,
                                 const hu_preference_pair_t *pairs, size_t n_pairs,
                                 hu_rl_trainer_metrics_t *out) {
    if (!vctx || !alloc || !pairs || n_pairs == 0 || !out) return HU_ERR_INVALID_ARGUMENT;
    kto_huml_ctx_t *c = (kto_huml_ctx_t *)vctx;
    double total_loss = 0.0;
    double chosen_delta = 0.0, rejected_delta = 0.0;
    size_t valid_count = 0;

    for (size_t i = 0; i < n_pairs; i++) {
        int is_desirable   = (pairs[i].chosen_len > 0 && pairs[i].rejected_len == 0);
        int is_undesirable = (pairs[i].chosen_len == 0 && pairs[i].rejected_len > 0);
        int is_two_sided   = (pairs[i].chosen_len > 0 && pairs[i].rejected_len > 0);

        if (is_two_sided) continue;
        /* Phase 3 audit fold-in (critic MEDIUM-3): null-pair (neither
         * chosen nor rejected populated) used to mid-batch `return
         * HU_ERR_INVALID_ARGUMENT`, which would leave any pairs
         * already processed before this index with mutated lm_head
         * weights but no metrics in `out`. Skip silently — same
         * convention as is_two_sided above. Caller-side validation
         * happens at `human ml kto-train` JSONL load time. */
        if (!is_desirable && !is_undesirable) continue;

        int32_t *prompt = NULL, *response = NULL;
        size_t pl = 0, rl = 0;
        size_t pcap = 0, rcap = 0;

        if (parse_id_string(alloc, pairs[i].prompt, &prompt, &pl, &pcap) != HU_OK) continue;

        const char *resp_str = is_desirable ? pairs[i].chosen : pairs[i].rejected;
        if (parse_id_string(alloc, resp_str, &response, &rl, &rcap) != HU_OK) {
            alloc->free(alloc->ctx, prompt, pcap * sizeof(int32_t));
            continue;
        }
        if (pl == 0 || rl == 0) goto cleanup_pair;

        double lp_pol = 0, lp_ref = 0;
        hu_policy_logprobs(alloc, &c->policy, prompt, pl, response, rl, &lp_pol);
        hu_policy_logprobs(alloc, &c->reference, prompt, pl, response, rl, &lp_ref);

        double lambda, pair_loss, grad_scalar;

        if (is_desirable) {
            lambda = c->lambda_d;
            double diff = lp_pol - lp_ref;
            double sig = 1.0 / (1.0 + exp(-c->beta * diff));
            pair_loss = lambda * (1.0 - sig);
            /* dL/d(logpi_theta) = -lambda * beta * sig * (1 - sig) */
            grad_scalar = -lambda * c->beta * sig * (1.0 - sig);
            chosen_delta += lp_pol - lp_ref;
        } else {
            lambda = c->lambda_u;
            double diff = lp_ref - lp_pol;
            double sig = 1.0 / (1.0 + exp(-c->beta * diff));
            pair_loss = lambda * (1.0 - sig);
            /* dL/d(logpi_theta) = +lambda * beta * sig * (1 - sig) */
            grad_scalar = +lambda * c->beta * sig * (1.0 - sig);
            rejected_delta += lp_pol - lp_ref;
        }

        total_loss += pair_loss;
        valid_count++;

        /* Backward: sign-based finite-diff step on lm_head rows for
         * response tokens. Same structure as dpo_real_huml.c. */
        if (c->learning_rate > 0 && fabs(grad_scalar) > 1e-20) {
            hu_ml_tensor_t *params = NULL;
            size_t n_params = 0;
            hu_error_t pe = c->policy.vtable->get_params(c->policy.ctx, &params, &n_params);
            if (pe != HU_OK) goto cleanup_pair;
            if (n_params >= 2 && params[1].dtype == HU_ML_DTYPE_F32) {
                size_t V = c->gpt_cfg.vocab_size;
                size_t E = c->gpt_cfg.n_embd;
                size_t lm_head_elems = params[1].size_bytes / sizeof(float);
                if (lm_head_elems == V * E && V > 0 && E > 0) {
                    float *lm_head = (float *)params[1].data;
                    /* SGD: W -= lr * grad. grad_scalar < 0 for desirable
                     * → W increases → logpi increases. grad_scalar > 0
                     * for undesirable → W decreases → logpi decreases. */
                    float step = (float)(c->learning_rate * grad_scalar * 0.1);

                    for (size_t k = 0; k < rl; k++) {
                        int32_t t = response[k];
                        if (t < 0 || (size_t)t >= V) continue;
                        float *cell = lm_head + (size_t)t * E;
                        float saved = *cell;
                        /* Try stepping in the negative-gradient direction */
                        *cell = saved - step;
                        double lp_new = 0;
                        hu_policy_logprobs(alloc, &c->policy, prompt, pl, response, rl, &lp_new);
                        if (is_desirable) {
                            if (lp_new <= lp_pol) {
                                *cell = saved + step;
                                hu_policy_logprobs(alloc, &c->policy, prompt, pl, response, rl, &lp_new);
                                if (lp_new <= lp_pol) {
                                    *cell = saved;
                                }
                            }
                        } else {
                            if (lp_new >= lp_pol) {
                                *cell = saved + step;
                                hu_policy_logprobs(alloc, &c->policy, prompt, pl, response, rl, &lp_new);
                                if (lp_new >= lp_pol) {
                                    *cell = saved;
                                }
                            }
                        }
                    }
                }
            }
        }

cleanup_pair:
        if (prompt)   alloc->free(alloc->ctx, prompt,   pcap * sizeof(int32_t));
        if (response) alloc->free(alloc->ctx, response, rcap * sizeof(int32_t));
    }

    double denom = valid_count > 0 ? (double)valid_count : 1.0;
    out->final_loss = total_loss / denom;
    out->iters_completed = 1;
    out->chosen_logprob_delta = chosen_delta / denom;
    out->rejected_logprob_delta = rejected_delta / denom;
    out->adapter_path[0] = '\0';
    return HU_OK;
}

static hu_error_t kto_huml_save(void *vctx, hu_allocator_t *alloc, const char *path) {
    if (!vctx || !alloc || !path) return HU_ERR_INVALID_ARGUMENT;
    kto_huml_ctx_t *c = (kto_huml_ctx_t *)vctx;
    if (!c->initialized) return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "wb");
    if (!f) return HU_ERR_IO;

    hu_ml_tensor_t *params = NULL;
    size_t n_params = 0;
    hu_error_t err = c->policy.vtable->get_params(c->policy.ctx, &params, &n_params);
    if (err != HU_OK || n_params < 2) { fclose(f); return err == HU_OK ? HU_ERR_INVALID_ARGUMENT : err; }

    fwrite(params[1].data, 1, params[1].size_bytes, f);
    fclose(f);
    return HU_OK;
}

static const char *kto_huml_name(void *vctx) { (void)vctx; return "kto_huml"; }

static void kto_huml_deinit(void *vctx, hu_allocator_t *alloc) {
    if (!vctx) return;
    kto_huml_ctx_t *c = (kto_huml_ctx_t *)vctx;
    if (c->initialized) {
        c->policy.vtable->deinit(c->policy.ctx, alloc);
        c->reference.vtable->deinit(c->reference.ctx, alloc);
    }
    alloc->free(alloc->ctx, c, sizeof(kto_huml_ctx_t));
}

static const hu_rl_trainer_vtable_t kto_huml_vtable = {
    .step = kto_huml_step,
    .save_adapter = kto_huml_save,
    .name = kto_huml_name,
    .deinit = kto_huml_deinit,
};

hu_error_t hu_kto_huml_create(hu_allocator_t *alloc,
                               const hu_rl_trainer_config_t *config,
                               hu_rl_trainer_t *out) {
    if (!alloc || !config || !out) return HU_ERR_INVALID_ARGUMENT;
    kto_huml_ctx_t *c = (kto_huml_ctx_t *)alloc->alloc(alloc->ctx, sizeof(kto_huml_ctx_t));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->beta = config->beta > 0 ? config->beta : 0.1;
    c->learning_rate = config->learning_rate > 0 ? config->learning_rate : 1e-5;
    c->lambda_d = (config->lambda_d == 0.0) ? 1.0 : config->lambda_d;
    c->lambda_u = (config->lambda_u == 0.0) ? 1.0 : config->lambda_u;
    c->gpt_cfg = (hu_gpt_config_t){
        .vocab_size = 32,
        .n_layer = 1,
        .n_head = 1,
        .n_kv_head = 1,
        .n_embd = 16,
        .head_dim = 16,
        .sequence_len = 64,
    };
    if (hu_gpt_create(alloc, &c->gpt_cfg, &c->policy) != HU_OK) {
        alloc->free(alloc->ctx, c, sizeof(kto_huml_ctx_t));
        return HU_ERR_PROVIDER_RESPONSE;
    }
    if (hu_reference_model_create_from(alloc, &c->policy, &c->gpt_cfg, &c->reference) != HU_OK) {
        c->policy.vtable->deinit(c->policy.ctx, alloc);
        alloc->free(alloc->ctx, c, sizeof(kto_huml_ctx_t));
        return HU_ERR_PROVIDER_RESPONSE;
    }
    c->initialized = 1;
    out->ctx = c;
    out->vtable = &kto_huml_vtable;
    return HU_OK;
}

/* MLX backend: real implementation in src/ml/kto_mlx.c (Phase 3 Task 7). */

#if HU_IS_TEST
hu_error_t kto_compute_loss_only_for_test(void *vctx,
                                           const hu_preference_pair_t *p,
                                           double *out_loss) {
    if (!vctx || !p || !out_loss) return HU_ERR_INVALID_ARGUMENT;
    kto_huml_ctx_t *c = (kto_huml_ctx_t *)vctx;
    hu_allocator_t alloc = hu_system_allocator();

    int is_desirable   = (p->chosen_len > 0 && p->rejected_len == 0);
    int is_undesirable = (p->chosen_len == 0 && p->rejected_len > 0);
    if (!is_desirable && !is_undesirable) return HU_ERR_INVALID_ARGUMENT;

    int32_t *prompt = NULL, *response = NULL;
    size_t pl = 0, rl = 0, pcap = 0, rcap = 0;

    if (parse_id_string(&alloc, p->prompt, &prompt, &pl, &pcap) != HU_OK)
        return HU_ERR_INVALID_ARGUMENT;

    const char *resp_str = is_desirable ? p->chosen : p->rejected;
    if (parse_id_string(&alloc, resp_str, &response, &rl, &rcap) != HU_OK) {
        alloc.free(alloc.ctx, prompt, pcap * sizeof(int32_t));
        return HU_ERR_INVALID_ARGUMENT;
    }

    double lp_pol = 0, lp_ref = 0;
    hu_policy_logprobs(&alloc, &c->policy, prompt, pl, response, rl, &lp_pol);
    hu_policy_logprobs(&alloc, &c->reference, prompt, pl, response, rl, &lp_ref);

    if (is_desirable) {
        double diff = lp_pol - lp_ref;
        double sig = 1.0 / (1.0 + exp(-c->beta * diff));
        *out_loss = c->lambda_d * (1.0 - sig);
    } else {
        double diff = lp_ref - lp_pol;
        double sig = 1.0 / (1.0 + exp(-c->beta * diff));
        *out_loss = c->lambda_u * (1.0 - sig);
    }

    alloc.free(alloc.ctx, prompt, pcap * sizeof(int32_t));
    alloc.free(alloc.ctx, response, rcap * sizeof(int32_t));
    return HU_OK;
}

hu_error_t kto_get_huml_lm_head_param_for_test(void *vctx,
                                                 size_t row, size_t col,
                                                 float **out_param_ptr) {
    if (!vctx || !out_param_ptr) return HU_ERR_INVALID_ARGUMENT;
    kto_huml_ctx_t *c = (kto_huml_ctx_t *)vctx;

    hu_ml_tensor_t *params = NULL;
    size_t n_params = 0;
    hu_error_t e = c->policy.vtable->get_params(c->policy.ctx, &params, &n_params);
    if (e != HU_OK) return e;
    if (n_params < 2 || params[1].dtype != HU_ML_DTYPE_F32)
        return HU_ERR_INVALID_ARGUMENT;

    size_t V = c->gpt_cfg.vocab_size;
    size_t E = c->gpt_cfg.n_embd;
    if (row >= V || col >= E) return HU_ERR_INVALID_ARGUMENT;

    float *lm_head = (float *)params[1].data;
    *out_param_ptr = lm_head + row * E + col;
    return HU_OK;
}
#endif /* HU_IS_TEST */
