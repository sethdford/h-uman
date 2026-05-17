/* src/ml/reward_model_train.c — Phase 3 Task 3
 *
 * Bradley-Terry reward-model training loop. Frozen backbone (toy GPT
 * from Task 2's huml_rm_ctx_t.backbone), trainable value head (Task 1's
 * hu_value_head_t W and b). Per-iter mini-batch SGD, no momentum.
 *
 * Loss (per two-sided pair, Christiano 2017 §2.2 / Ouyang 2022 §3.5):
 *   r_w = score(prompt, chosen)
 *   r_l = score(prompt, rejected)
 *   L_i = -log σ(r_w - r_l)
 *
 * Backward (chained through hu_value_head_backward — see
 * include/human/ml/value_head.h):
 *   dL/dr_w = σ(r_w - r_l) - 1
 *   dL/dr_l = 1 - σ(r_w - r_l)
 *   For each side: dW[i] += h[i] * dL/dr,  db += dL/dr
 *
 * The dh output of hu_value_head_backward is intentionally DISCARDED:
 * the backbone is frozen, so the gradient w.r.t. the hidden state is
 * not propagated.
 *
 * Style mirrors src/ml/reward_model.c and src/ml/dpo_real_huml.c:
 *   - hu_allocator_t with the 3-arg free (ctx, ptr, size); every
 *     allocation tracks its capacity so the test allocator's leak
 *     ledger balances (AGENTS.md §2 — ASan).
 *   - HU_IS_TEST style is `#if HU_IS_TEST` (numeric, NOT #ifdef) per
 *     the Phase 2 audit follow-through (commit b6a71f81).
 *
 * Why duplicate parse_id_string + forward-and-extract from
 * src/ml/reward_model.c instead of refactoring an exported helper: the
 * forward in this file MUST capture the last-position hidden state h
 * for the value-head backward (dW[i] = h[i] * dL/dr). Going through
 * the public score() vtable would cost two forwards per pair (once for
 * the score, once for h) AND lose the gradient-numerical-equivalence
 * guarantee that the FD grad check below relies on. The duplication is
 * ~30 lines and is the SAME translation-unit-level pattern
 * dpo_real_huml.c already uses (which also duplicates parse_id_string
 * from reward_model.c). Rule of Three on parse_id_string is now
 * satisfied with three callers (reward_model.c, dpo_real_huml.c, this
 * file); extraction to src/ml/ml_tokens.c is a follow-up refactor
 * tracked outside Task 3 scope.
 *
 * AC-6 (FD grad check on every loss): both the analytical SGD step and
 * the FD test perturb the SAME hu_value_head_t.W storage and route
 * through the SAME huml_rm_forward_capture_h helper, so the two
 * gradients are guaranteed to be on the same tensor — there is no
 * "test path uses different forward than production path" gap.
 */

#include "human/ml/reward_model.h"
#include "reward_model_priv.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dpo.h"
#include "human/ml/ml.h"
#include "human/ml/model.h"
#include "human/ml/value_head.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tokenize a space-separated int-id string into an int32_t array. Out-of-
 * range tokens are clamped to vocab_size - 1 (the toy GPT crashes on
 * >= V). Returns the buffer capacity via *out_cap so callers can free
 * with `cap * sizeof(int32_t)` per the 3-arg allocator contract.
 *
 * Structurally identical to parse_id_string in src/ml/reward_model.c
 * and src/ml/dpo_real_huml.c — see file-header note on Rule of Three. */
static hu_error_t parse_id_string(hu_allocator_t *alloc, const char *s,
                                  size_t vocab_size, int32_t **out, size_t *out_n,
                                  size_t *out_cap) {
    if (!alloc || !alloc->alloc || !s || !out || !out_n) {
        return HU_ERR_INVALID_ARGUMENT;
    }
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
        if (v < 0) v = 0;
        if (vocab_size > 0 && (size_t)v >= vocab_size) v = (long)(vocab_size - 1);
        buf[n++] = (int32_t)v;
        p = endp;
        while (*p == ' ' || *p == '\t') p++;
    }
    *out = buf;
    *out_n = n;
    if (out_cap) *out_cap = cap;
    return HU_OK;
}

/* Run the toy GPT backbone forward on `prompt response` (space-joined),
 * extract the last-position [vocab_size]-vector of logits as the
 * "hidden state" h (same shape contract as huml_rm_score in
 * reward_model.c — see plan §D3 / R4), and project through the value
 * head to produce the scalar score.
 *
 * `out_h` may be NULL to skip the hidden-state copy (used for
 * forward-only loss computation; the SGD step needs h, the
 * initial/final-loss measurements do not). When non-NULL, it must
 * point to at least c->value_head.hidden_dim float slots, which the
 * training loop allocates once per session and reuses across pairs. */
static hu_error_t huml_rm_forward_capture_h(huml_rm_ctx_t *c, hu_allocator_t *alloc,
                                             const char *prompt, size_t prompt_len,
                                             const char *response, size_t response_len,
                                             double *out_score, float *out_h) {
    if (!c || !alloc || !alloc->alloc || !alloc->free
        || !prompt || prompt_len == 0
        || !response || response_len == 0
        || !out_score) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!c->backbone.vtable || !c->backbone.vtable->forward) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    int32_t *prompt_ids = NULL, *response_ids = NULL;
    size_t pl = 0, rl = 0;
    size_t pcap = 0, rcap = 0;
    hu_error_t err = parse_id_string(alloc, prompt, c->vocab_size,
                                      &prompt_ids, &pl, &pcap);
    if (err != HU_OK) return err;
    err = parse_id_string(alloc, response, c->vocab_size,
                          &response_ids, &rl, &rcap);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, prompt_ids, pcap * sizeof(int32_t));
        return err;
    }
    if (pl == 0 || rl == 0) {
        alloc->free(alloc->ctx, prompt_ids, pcap * sizeof(int32_t));
        alloc->free(alloc->ctx, response_ids, rcap * sizeof(int32_t));
        return HU_ERR_INVALID_ARGUMENT;
    }
    /* Mirror reward_model.c's sequence_len clamp: drop response tokens
     * first, prompt tokens second; refuse the pair entirely if even the
     * minimal prompt would still overflow. */
    if (pl + rl > c->gpt_cfg.sequence_len) {
        size_t over = (pl + rl) - c->gpt_cfg.sequence_len;
        if (over >= rl) {
            if (over >= pl) {
                alloc->free(alloc->ctx, prompt_ids, pcap * sizeof(int32_t));
                alloc->free(alloc->ctx, response_ids, rcap * sizeof(int32_t));
                return HU_ERR_INVALID_ARGUMENT;
            }
            pl -= over;
        } else {
            rl -= over;
        }
    }

    size_t total = pl + rl;
    int32_t *ids = (int32_t *)alloc->alloc(alloc->ctx, total * sizeof(int32_t));
    if (!ids) {
        alloc->free(alloc->ctx, prompt_ids, pcap * sizeof(int32_t));
        alloc->free(alloc->ctx, response_ids, rcap * sizeof(int32_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(ids, prompt_ids, pl * sizeof(int32_t));
    memcpy(ids + pl, response_ids, rl * sizeof(int32_t));
    alloc->free(alloc->ctx, prompt_ids, pcap * sizeof(int32_t));
    alloc->free(alloc->ctx, response_ids, rcap * sizeof(int32_t));

    hu_ml_tensor_t input = {
        .data = ids,
        .shape = {1, total, 0, 0},
        .ndim = 2,
        .dtype = HU_ML_DTYPE_I32,
        .size_bytes = total * sizeof(int32_t),
    };
    hu_ml_tensor_t output = {0};
    err = c->backbone.vtable->forward(c->backbone.ctx, &input, &output);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, ids, total * sizeof(int32_t));
        return err;
    }
    /* output.data: float logits, shape [1, total, V] (src/ml/gpt.c:534). */
    float *logits = (float *)output.data;
    size_t V = output.shape[2];
    if (V != c->value_head.hidden_dim) {
        alloc->free(alloc->ctx, output.data, output.size_bytes);
        alloc->free(alloc->ctx, ids, total * sizeof(int32_t));
        return HU_ERR_INVALID_ARGUMENT;
    }
    const float *h = logits + (total - 1) * V;
    if (out_h) {
        memcpy(out_h, h, V * sizeof(float));
    }
    double score = 0.0;
    err = hu_value_head_forward(&c->value_head, h, &score);

    alloc->free(alloc->ctx, output.data, output.size_bytes);
    alloc->free(alloc->ctx, ids, total * sizeof(int32_t));
    if (err != HU_OK) return err;
    *out_score = score;
    return HU_OK;
}

typedef struct {
    double sum_loss;  /* sum of -log σ(r_w - r_l) over USED pairs */
    size_t used;      /* pairs that contributed (both sides scored cleanly) */
    size_t skipped;   /* pairs skipped (one-sided KTO OR scoring failure) */
} bt_loss_summary_t;

/* Forward-only Bradley-Terry loss across the batch. Skips one-sided
 * KTO pairs (chosen_len == 0 OR rejected_len == 0) and pairs whose
 * scoring fails (e.g. degenerate token strings). The skipped count is
 * propagated to the caller so the training entry-point can report it. */
static hu_error_t compute_bt_loss(huml_rm_ctx_t *c, hu_allocator_t *alloc,
                                   const hu_preference_pair_t *pairs, size_t n,
                                   bt_loss_summary_t *out) {
    if (!c || !alloc || !pairs || n == 0 || !out) return HU_ERR_INVALID_ARGUMENT;
    out->sum_loss = 0.0;
    out->used = 0;
    out->skipped = 0;
    for (size_t i = 0; i < n; i++) {
        const hu_preference_pair_t *p = &pairs[i];
        if (p->prompt_len == 0 || p->chosen_len == 0 || p->rejected_len == 0) {
            out->skipped++;
            continue;
        }
        double r_w = 0.0, r_l = 0.0;
        hu_error_t err = huml_rm_forward_capture_h(c, alloc, p->prompt, p->prompt_len,
                                                    p->chosen, p->chosen_len,
                                                    &r_w, NULL);
        if (err != HU_OK) {
            out->skipped++;
            continue;
        }
        err = huml_rm_forward_capture_h(c, alloc, p->prompt, p->prompt_len,
                                         p->rejected, p->rejected_len,
                                         &r_l, NULL);
        if (err != HU_OK) {
            out->skipped++;
            continue;
        }
        /* Numerically-stable -log σ(r_w - r_l) via log1p(exp(-x)) for
         * x >= 0 and (-x + log1p(exp(x))) for x < 0; the closed form
         * `-log(σ(x))` overflows for large negative x. Same trick as
         * the DPO loss in src/ml/dpo_real_huml.c lines 145-148 with
         * the 1e-12 σ floor; log1p variant is preferred here because
         * the FD grad check sweeps small perturbations and the σ floor
         * would clip the analytical gradient below the FD step size
         * for some inits. */
        double x = r_w - r_l;
        double pair_loss;
        if (x >= 0) {
            pair_loss = log1p(exp(-x));
        } else {
            pair_loss = -x + log1p(exp(x));
        }
        out->sum_loss += pair_loss;
        out->used++;
    }
    return HU_OK;
}

/* Forward + backward + per-pair gradient accumulation. Returns the
 * iteration's mean loss (over USED pairs) plus the accumulated dW/db
 * for the SGD step that the caller applies after the loop.
 *
 * dW_acc must be a caller-provided float[hidden_dim] zero-initialized
 * each iter. db_acc is double for headroom against summation drift on
 * large batches. */
static hu_error_t bt_iter_forward_backward(huml_rm_ctx_t *c, hu_allocator_t *alloc,
                                            const hu_preference_pair_t *pairs, size_t n,
                                            float *dW_acc, double *db_acc,
                                            float *h_w_buf, float *h_l_buf,
                                            float *dW_tmp,
                                            bt_loss_summary_t *out) {
    if (!c || !alloc || !pairs || n == 0
        || !dW_acc || !db_acc || !h_w_buf || !h_l_buf || !dW_tmp || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    out->sum_loss = 0.0;
    out->used = 0;
    out->skipped = 0;
    *db_acc = 0.0;
    memset(dW_acc, 0, c->value_head.hidden_dim * sizeof(float));

    for (size_t i = 0; i < n; i++) {
        const hu_preference_pair_t *p = &pairs[i];
        if (p->prompt_len == 0 || p->chosen_len == 0 || p->rejected_len == 0) {
            out->skipped++;
            continue;
        }
        double r_w = 0.0, r_l = 0.0;
        hu_error_t err = huml_rm_forward_capture_h(c, alloc, p->prompt, p->prompt_len,
                                                    p->chosen, p->chosen_len,
                                                    &r_w, h_w_buf);
        if (err != HU_OK) { out->skipped++; continue; }
        err = huml_rm_forward_capture_h(c, alloc, p->prompt, p->prompt_len,
                                         p->rejected, p->rejected_len,
                                         &r_l, h_l_buf);
        if (err != HU_OK) { out->skipped++; continue; }

        double x = r_w - r_l;
        double sigma_x = 1.0 / (1.0 + exp(-x));   /* σ(r_w - r_l) */
        double pair_loss;
        if (x >= 0) {
            pair_loss = log1p(exp(-x));
        } else {
            pair_loss = -x + log1p(exp(x));
        }
        out->sum_loss += pair_loss;
        out->used++;

        /* Bradley-Terry gradients on the scalar scores, plan §A4:
         *   dL/dr_w = σ(r_w - r_l) - 1   (always negative for σ < 1)
         *   dL/dr_l = 1 - σ(r_w - r_l)   (always positive for σ < 1) */
        double dL_dr_w = sigma_x - 1.0;
        double dL_dr_l = 1.0 - sigma_x;

        float db_w = 0.0f, db_l = 0.0f;
        /* Backward through value_head for chosen side; dh discarded
         * (frozen backbone — Christiano 2017 §2.2). */
        err = hu_value_head_backward(&c->value_head, h_w_buf, dL_dr_w,
                                      dW_tmp, &db_w, NULL);
        if (err != HU_OK) { out->skipped++; out->used--; continue; }
        for (size_t k = 0; k < c->value_head.hidden_dim; k++) {
            dW_acc[k] += dW_tmp[k];
        }
        *db_acc += (double)db_w;

        err = hu_value_head_backward(&c->value_head, h_l_buf, dL_dr_l,
                                      dW_tmp, &db_l, NULL);
        if (err != HU_OK) { out->skipped++; out->used--; continue; }
        for (size_t k = 0; k < c->value_head.hidden_dim; k++) {
            dW_acc[k] += dW_tmp[k];
        }
        *db_acc += (double)db_l;
    }
    return HU_OK;
}

hu_error_t hu_reward_model_train(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                  const hu_preference_pair_t *pairs, size_t n,
                                  const hu_reward_model_train_config_t *config,
                                  hu_reward_model_train_metrics_t *out_metrics) {
    if (!rm || !alloc || !alloc->alloc || !alloc->free || !pairs || n == 0
        || !config || !out_metrics) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (config->max_iters == 0 || !(config->learning_rate > 0.0)) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    huml_rm_ctx_t *c = hu_reward_model_huml_ctx_or_null(rm);
    if (!c) {
        /* MLX path lives in scripts/rm_mlx_train.py (Task 8). */
        return HU_ERR_NOT_SUPPORTED;
    }
    if (c->value_head.hidden_dim == 0 || !c->value_head.W) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    memset(out_metrics, 0, sizeof(*out_metrics));
    const size_t H = c->value_head.hidden_dim;

    /* Per-iter scratch: dW accumulator, hidden-state buffers for the
     * chosen and rejected sides, and a single per-side dW tmp slot.
     * Allocated once for the full training run, reused across iters
     * AND across pairs within an iter. */
    float *dW_acc = (float *)alloc->alloc(alloc->ctx, H * sizeof(float));
    float *dW_tmp = (float *)alloc->alloc(alloc->ctx, H * sizeof(float));
    float *h_w_buf = (float *)alloc->alloc(alloc->ctx, H * sizeof(float));
    float *h_l_buf = (float *)alloc->alloc(alloc->ctx, H * sizeof(float));
    if (!dW_acc || !dW_tmp || !h_w_buf || !h_l_buf) {
        if (dW_acc)  alloc->free(alloc->ctx, dW_acc,  H * sizeof(float));
        if (dW_tmp)  alloc->free(alloc->ctx, dW_tmp,  H * sizeof(float));
        if (h_w_buf) alloc->free(alloc->ctx, h_w_buf, H * sizeof(float));
        if (h_l_buf) alloc->free(alloc->ctx, h_l_buf, H * sizeof(float));
        return HU_ERR_OUT_OF_MEMORY;
    }

    /* Initial loss (before iter 0) — pure forward pass. Also captures
     * the skipped-pair count, which is constant across iters (skipping
     * is determined by the input shape, not by training state). */
    bt_loss_summary_t init_summary = {0};
    hu_error_t err = compute_bt_loss(c, alloc, pairs, n, &init_summary);
    if (err != HU_OK) {
        alloc->free(alloc->ctx, dW_acc,  H * sizeof(float));
        alloc->free(alloc->ctx, dW_tmp,  H * sizeof(float));
        alloc->free(alloc->ctx, h_w_buf, H * sizeof(float));
        alloc->free(alloc->ctx, h_l_buf, H * sizeof(float));
        return err;
    }
    out_metrics->initial_loss = (init_summary.used > 0)
        ? init_summary.sum_loss / (double)init_summary.used
        : 0.0;
    out_metrics->skipped_count = init_summary.skipped;

    if (init_summary.skipped > 0) {
        /* One log per training run (NOT per skipped pair) — Phase 2
         * audit b6a71f81 standardized this. */
        fprintf(stderr,
                "hu_reward_model_train: skipping %zu of %zu one-sided KTO pair(s); "
                "Bradley-Terry needs both chosen and rejected sides. "
                "KTO unpaired loss lands in Task 4.\n",
                init_summary.skipped, n);
    }

    /* If no usable pair, there's nothing to train on — final == initial,
     * iters_completed = 0. Caller still gets a clean return. */
    if (init_summary.used == 0) {
        out_metrics->final_loss = out_metrics->initial_loss;
        out_metrics->iters_completed = 0;
        alloc->free(alloc->ctx, dW_acc,  H * sizeof(float));
        alloc->free(alloc->ctx, dW_tmp,  H * sizeof(float));
        alloc->free(alloc->ctx, h_w_buf, H * sizeof(float));
        alloc->free(alloc->ctx, h_l_buf, H * sizeof(float));
        return HU_OK;
    }

    /* Mini-batch SGD. Plan §A4 wording: "accumulate dW/db across all
     * pairs in the batch, apply the SGD update at the end of the
     * iteration." We normalize the accumulated gradient by the number
     * of USED pairs (mean-reduction) so config->learning_rate has the
     * same per-example interpretation regardless of batch size — this
     * matches the PyTorch default `loss.backward()` reduction and the
     * mean-loss metrics this function reports. */
    for (size_t iter = 0; iter < config->max_iters; iter++) {
        bt_loss_summary_t iter_summary = {0};
        double db_acc = 0.0;
        err = bt_iter_forward_backward(c, alloc, pairs, n,
                                        dW_acc, &db_acc,
                                        h_w_buf, h_l_buf, dW_tmp,
                                        &iter_summary);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, dW_acc,  H * sizeof(float));
            alloc->free(alloc->ctx, dW_tmp,  H * sizeof(float));
            alloc->free(alloc->ctx, h_w_buf, H * sizeof(float));
            alloc->free(alloc->ctx, h_l_buf, H * sizeof(float));
            return err;
        }
        if (iter_summary.used == 0) {
            /* Defensive: shouldn't happen if init_summary.used > 0
             * since skipping is data-determined, not state-determined.
             * If it does, stop training cleanly. */
            break;
        }

        const double inv_n = 1.0 / (double)iter_summary.used;
        const float lr_inv_n = (float)(config->learning_rate * inv_n);
        for (size_t k = 0; k < H; k++) {
            c->value_head.W[k] -= lr_inv_n * dW_acc[k];
        }
        c->value_head.b -= (float)(config->learning_rate * inv_n * db_acc);
        out_metrics->iters_completed++;

        if (config->log_every > 0 && (iter + 1) % config->log_every == 0) {
            fprintf(stderr, "hu_reward_model_train iter=%zu/%zu loss=%.6f used=%zu\n",
                    iter + 1, config->max_iters,
                    iter_summary.sum_loss * inv_n, iter_summary.used);
        }
    }

    /* Final loss (after the last SGD step) — pure forward. */
    bt_loss_summary_t final_summary = {0};
    err = compute_bt_loss(c, alloc, pairs, n, &final_summary);
    alloc->free(alloc->ctx, dW_acc,  H * sizeof(float));
    alloc->free(alloc->ctx, dW_tmp,  H * sizeof(float));
    alloc->free(alloc->ctx, h_w_buf, H * sizeof(float));
    alloc->free(alloc->ctx, h_l_buf, H * sizeof(float));
    if (err != HU_OK) return err;
    out_metrics->final_loss = (final_summary.used > 0)
        ? final_summary.sum_loss / (double)final_summary.used
        : 0.0;
    return HU_OK;
}

#if HU_IS_TEST
/* Test seam #1 (AC-6 finite-difference grad check):
 * compute the same Bradley-Terry mean loss the SGD step uses for its
 * forward, with NO backward and NO weight update. Lets the FD test
 * perturb W[0] by ±eps and read back two losses on the SAME forward
 * implementation as the training-time gradient. Returning the mean
 * over USED pairs (not the sum) matches metrics.initial_loss /
 * metrics.final_loss so the FD check answers "is the analytical
 * gradient consistent with the loss the training metrics report?". */
hu_error_t reward_model_compute_bt_loss_only_for_test(hu_reward_model_t *rm,
                                                       hu_allocator_t *alloc,
                                                       const hu_preference_pair_t *pairs,
                                                       size_t n,
                                                       double *out_loss) {
    if (!rm || !alloc || !pairs || n == 0 || !out_loss) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    huml_rm_ctx_t *c = hu_reward_model_huml_ctx_or_null(rm);
    if (!c) return HU_ERR_NOT_SUPPORTED;
    bt_loss_summary_t s = {0};
    hu_error_t err = compute_bt_loss(c, alloc, pairs, n, &s);
    if (err != HU_OK) return err;
    *out_loss = (s.used > 0) ? s.sum_loss / (double)s.used : 0.0;
    return HU_OK;
}

/* Test seam #2 (AC-6 finite-difference grad check):
 * direct mutable pointer to value_head.W[0..hidden_dim-1] for the FD
 * test to perturb a single weight. Returns NULL for non-HUML rms (MLX
 * keeps weights in a Python subprocess, Task 8). The pointer is
 * stable for the lifetime of `rm` and aliases the same storage the
 * SGD step writes — perturbations applied via this pointer are
 * visible to the next forward pass without any reseat/refresh. */
float *reward_model_huml_value_head_W_for_test(hu_reward_model_t *rm) {
    if (!rm) return NULL;
    huml_rm_ctx_t *c = hu_reward_model_huml_ctx_or_null(rm);
    if (!c) return NULL;
    return c->value_head.W;
}
#endif /* HU_IS_TEST */
