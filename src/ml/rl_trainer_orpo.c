/* US-11.5 — ORPO loss head (finishes Sprint 7 US-7.10 stub).
 *
 * Canonical ORPO loss (Hong et al. arXiv:2403.07691 Eq. 7; EMNLP 2024 /
 * ICLR 2025):
 *
 *   L_ORPO = L_SFT + λ · L_OR
 *   L_SFT  = -(1/|y_w|) · logπ(y_w | x)                       (NLL on chosen)
 *   L_OR   = -log σ( log_odds(y_w|x) - log_odds(y_l|x) )
 *   log_odds(y|x) = logπ(y|x) - log(1 - π(y|x))
 *
 * Key properties:
 *   • Single-stage SFT + preference: NLL term directly anchors the
 *     chosen completion, eliminating DCR-style drift seen on Sprint 8.
 *   • NO reference model required (one of ORPO's main advantages over
 *     DPO); we keep a `model *` pointer only for parity with SimPO so
 *     a future `train_step` body has somewhere to land.
 *
 * Numerical hazards & mitigations:
 *   • `log(1 - exp(logp))` underflows when `logp → 0`. Use `log1p(-exp(x))`
 *     (precise for very-negative `x`) and clamp `logp ≤ -1e-12` so
 *     `1 - exp(logp) > 0`. See HU_ORPO_LOG1MEXP_FLOOR.
 *   • Outer `-log σ(...)` overflows when σ → 0 (large negative logits).
 *     Mirror the SimPO floor at `src/ml/rl_trainer_simpo.c:33`.
 *
 * Mirror layout / contract from `src/ml/rl_trainer_simpo.c`. Same
 * vtable signature, same factory shape; the only behavioral
 * difference is the loss math and the absence of a reference model.
 *
 * Design doc: `sprints/sprint-11/designs/US-11.5.md`.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dpo.h"
#include "human/ml/rl_trainer.h"

#include <math.h>
#include <stddef.h>

/* Lower bound on σ(...) before taking -log. Mirrors HU_SIMPO_SIGMOID_FLOOR
 * (src/ml/rl_trainer_simpo.c:33). */
#define HU_ORPO_SIGMOID_FLOOR 1e-10

/* Upper-bound clamp on logp before computing log(1 - exp(logp)). When
 * logp is too close to 0 the term `1 - exp(logp)` underflows; clamping
 * keeps the inner argument strictly positive. 1e-12 chosen to stay well
 * inside double precision (eps ≈ 2.22e-16). */
#define HU_ORPO_LOG1MEXP_FLOOR 1e-12

typedef struct hu_orpo_ctx {
    hu_allocator_t *alloc;
    float lambda;
    hu_model_t *model; /* nullable; set only when caller intends train_step. */
} hu_orpo_ctx_t;

/* Numerically stable sigmoid (no overflow on large negative x). */
static double orpo_sigmoid(double x) {
    if (x >= 0.0) {
        double e = exp(-x);
        return 1.0 / (1.0 + e);
    }
    double e = exp(x);
    return e / (1.0 + e);
}

/* log(1 - exp(logp)) with precision-preserving `log1p` and a clamp to
 * keep the inner argument strictly positive. Caller guarantees
 * logp <= 0 (it is a log-probability). */
static double orpo_log1mexp(double logp) {
    /* Clamp so that 1 - exp(logp) is strictly > 0. Without this, a
     * model that is *exactly* certain on the chosen token gives logp=0
     * and the log-odds diverges. */
    if (logp > -HU_ORPO_LOG1MEXP_FLOOR)
        logp = -HU_ORPO_LOG1MEXP_FLOOR;
    /* log1p(-exp(logp)) is exact for very negative logp. */
    return log1p(-exp(logp));
}

static hu_error_t orpo_compute_loss(void *ctx, const hu_pref_pair_logprobs_t *lp,
                                    double *out_loss) {
    if (!ctx || !lp || !out_loss)
        return HU_ERR_INVALID_ARGUMENT;
    if (lp->chosen_token_count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_orpo_ctx_t *self = (hu_orpo_ctx_t *)ctx;

    /* SFT term: length-normalized NLL on chosen. */
    double avg_chosen = lp->chosen_logprob_sum / (double)lp->chosen_token_count;
    double nll = -avg_chosen;

    /* OR penalty: if no rejected sequence supplied, degrade to pure SFT
     * (single-stage SFT-only mode; see US-11.5 design §9 OQ-3 — the
     * cheaper interpretation of AC-11.5.2 that reads naturally as
     * "loss = NLL + λ·OR_penalty, where OR_penalty = 0 when no
     * rejected"). */
    double or_penalty = 0.0;
    if (lp->rejected_token_count > 0) {
        double avg_rejected = lp->rejected_logprob_sum / (double)lp->rejected_token_count;

        double log_odds_chosen = avg_chosen - orpo_log1mexp(avg_chosen);
        double log_odds_rejected = avg_rejected - orpo_log1mexp(avg_rejected);

        double or_logits = log_odds_chosen - log_odds_rejected;
        double sig = orpo_sigmoid(or_logits);
        if (sig < HU_ORPO_SIGMOID_FLOOR)
            sig = HU_ORPO_SIGMOID_FLOOR;
        or_penalty = -log(sig);
    }

    *out_loss = nll + (double)self->lambda * or_penalty;
    return HU_OK;
}

static hu_error_t orpo_train_step(void *ctx, const hu_preference_pair_t *pair, double *out_loss) {
    if (!ctx || !pair || !out_loss)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    /* CLI test path (AC-11.5.1): model may be NULL. Emit a deterministic
     * mock loss so the CLI router test exercises the full create →
     * train_step → deinit lifecycle without booting a real GPT.
     * Mirrors `simpo_train_step` HU_IS_TEST branch. */
    (void)ctx;
    *out_loss = 0.7; /* fixed, distinct from SimPO mock (0.5) so log lines disambiguate. */
    return HU_OK;
#else
    hu_orpo_ctx_t *self = (hu_orpo_ctx_t *)ctx;
    if (!self->model || !self->model->vtable || !self->model->vtable->forward) {
        /* No model bound — caller must use compute_loss with injected
         * logprobs instead. See FU-11.5.a (production forward-pass gap,
         * symmetric to FU-7.10.a for SimPO). */
        return HU_ERR_NOT_SUPPORTED;
    }
    /* TODO(FU-11.5.a): implement full forward-pass training step.
     * v1 vtable lands the surface + loss head; the forward-pass +
     * backward-pass + tokenizer wiring is the next story. */
    (void)pair;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static void orpo_deinit(void *ctx) {
    if (!ctx)
        return;
    hu_orpo_ctx_t *self = (hu_orpo_ctx_t *)ctx;
    hu_allocator_t *alloc = self->alloc;
    if (alloc && alloc->free)
        alloc->free(alloc->ctx, self, sizeof(*self));
}

static const hu_rl_trainer_vtable_t HU_ORPO_VTABLE = {
    .compute_loss = orpo_compute_loss,
    .train_step = orpo_train_step,
    .deinit = orpo_deinit,
};

hu_error_t hu_rl_trainer_orpo_create(hu_allocator_t *alloc, const hu_orpo_config_t *cfg,
                                     hu_rl_trainer_t *out) {
    if (!alloc || !alloc->alloc || !cfg || !out)
        return HU_ERR_INVALID_ARGUMENT;
    /* λ must be strictly positive — λ=0 reduces ORPO to plain SFT, which
     * the caller can already do via the existing trainer path. Reject
     * negative as nonsensical. */
    if (!(cfg->lambda > 0.0f))
        return HU_ERR_INVALID_ARGUMENT;

    hu_orpo_ctx_t *self = (hu_orpo_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*self));
    if (!self)
        return HU_ERR_OUT_OF_MEMORY;

    self->alloc = alloc;
    self->lambda = cfg->lambda;
    self->model = cfg->model;

    out->ctx = self;
    out->vtable = &HU_ORPO_VTABLE;
    out->type = HU_RL_TRAINER_ORPO;
    return HU_OK;
}
