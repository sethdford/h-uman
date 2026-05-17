#ifndef HU_ML_DP_SGD_H
#define HU_ML_DP_SGD_H

/* US-8.1 — Real DP-SGD with RDP Accounting.
 *
 * Side-by-side module that delivers the *correct* primitives for per-sample
 * gradient clipping + Gaussian noise + Rényi Differential Privacy (RDP)
 * moments-accountant tracking, as a standalone unit. Sprint-9 composes this
 * into `src/ml/learner_cpu.c`; for now, the existing learner stays as-is.
 *
 * Two of the four public functions are *pure predicates* (no I/O, no
 * allocation, no global state) so the security-relevant decisions (σ
 * calibration, ε budget query) are testable directly without spinning up a
 * training loop. See `.claude/rules/security-predicate-extraction.md`.
 *
 * References:
 *   - Abadi et al., CCS 2016. "Deep Learning with Differential Privacy."
 *     §3.3 (Moments Accountant), Algorithm 1 (DP-SGD).
 *   - Mironov, CSF 2017. "Rényi Differential Privacy." §4 (composition),
 *     §5 (sub-sampled Gaussian → RDP closed form).
 *   - Mironov, Talwar, Zhang, arXiv:1908.10530. Theorem 4 (RDP closed form
 *     for the sampled Gaussian mechanism).
 *
 * Naming collision note: `learner.h` already defines an additive
 * `hu_dp_accountant_t`. This file introduces a *distinct* `hu_dp_rdp_accountant_t`
 * (RDP moments accountant) and `hu_dp_accountant_rdp_*` functions, so the two
 * coexist; sprint-9 deletes the additive one.
 *
 * Determinism contract: `hu_dp_sgd_step` is byte-for-byte reproducible for a
 * given (inputs, seed). It uses an embedded splitmix64 PRNG; no `rand()`,
 * no `/dev/urandom`, no `time(NULL)`. */

#include <stddef.h>
#include <stdint.h>

#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of RDP events tracked per accountant.
 * 100k events at ~16B each = ~1.6 MB; bounded to keep allocation deterministic.
 * NOTE: the running RDP_α counters are accumulated incrementally so the cost
 * of `hu_dp_accountant_rdp_epsilon` is O(|α grid|), not O(events × |α grid|). */
#define HU_DP_RDP_MAX_EVENTS 100000

/* Moments-accountant α grid. Opacus uses this same integer grid; outside it
 * we have observed loose / NaN behavior at extreme σ. Range chosen per
 * Mironov 2017 §5. */
#define HU_DP_RDP_ALPHA_MIN   2
#define HU_DP_RDP_ALPHA_MAX   64
#define HU_DP_RDP_ALPHA_COUNT (HU_DP_RDP_ALPHA_MAX - HU_DP_RDP_ALPHA_MIN + 1)

/* One sub-sampled-Gaussian event in the privacy ledger. */
typedef struct hu_dp_rdp_event {
    double sigma;       /* noise multiplier; must be > 0 */
    double sample_rate; /* q in [0, 1] */
} hu_dp_rdp_event_t;

/* RDP moments accountant. Fixed capacity, no allocation after init. */
typedef struct hu_dp_rdp_accountant {
    hu_dp_rdp_event_t events[HU_DP_RDP_MAX_EVENTS];
    size_t event_count;
    /* Running RDP_α for each α in the grid; reset to 0 at init. */
    double rdp_alpha[HU_DP_RDP_ALPHA_COUNT];
} hu_dp_rdp_accountant_t;

/* --- Pure predicates (no I/O, no allocation, no global state) --- */

/* Calibrate Gaussian noise σ to hit `target_epsilon` at `target_delta` after
 * `steps` sub-sampled Gaussian queries at sampling rate `sample_rate`.
 * `dataset_size` is informational (caller passes it for symmetry with Opacus;
 * `sample_rate = batch_size / dataset_size` should hold in the caller). The
 * per-sample sensitivity is fixed at `clip_norm` (after which Gaussian noise
 * σ_noise = sigma * clip_norm is added).
 *
 * Returns σ (> 0) on success, or NaN if no σ in the search range satisfies
 * the budget. Caller MUST check via `isnan()` before using the result; see
 * the determinism / safety note in §"R7" of the design doc.
 *
 * Implementation: binary search on σ in [0.1, 100.0], inner loop calls
 * `hu_dp_rdp_epsilon_from_sigma`. Converges to ~1e-4 relative tolerance. */
double hu_dp_sgd_noise_sigma(double clip_norm, double target_epsilon, double target_delta,
                             size_t steps, size_t dataset_size, double sample_rate);

/* Inverse: given a completed run of `steps` queries at noise multiplier σ
 * and sampling rate q, return the ε actually consumed at the given δ.
 *
 * Returns ε (>= 0) on success, or NaN on invalid input (σ <= 0, q ∉ [0, 1],
 * δ ∉ (0, 1)). */
double hu_dp_rdp_epsilon_from_sigma(double sigma, double sample_rate, size_t steps, double delta);

/* --- DP-SGD step (impure: writes output, consumes PRNG) --- */

/* Per-sample clip + aggregate + Gaussian noise (Abadi et al. 2016 Alg. 1).
 *
 *   per_sample_grads : flattened row-major matrix [batch_size × num_params].
 *                      Row i is sample i's gradient vector.
 *   batch_size       : number of rows; MUST be >= 1.
 *   num_params       : number of columns; MUST be >= 1.
 *   clip_norm        : per-row L2 clip threshold (e.g. 1.0).
 *   sigma            : Gaussian noise multiplier; noise σ = sigma * clip_norm.
 *                      sigma == 0.0 disables noise (test-only path).
 *   seed             : PRNG seed; the determinism contract is keyed on this.
 *   out_aggregate    : caller-allocated [num_params] output vector.
 *
 * Returns:
 *   HU_OK                       on success.
 *   HU_ERR_INVALID_ARGUMENT     if batch_size == 0, num_params == 0,
 *                               clip_norm <= 0, sigma < 0, or any pointer
 *                               is NULL.
 *
 * Determinism: byte-for-byte identical output for identical (inputs, seed). */
hu_error_t hu_dp_sgd_step(const double *per_sample_grads, size_t batch_size, size_t num_params,
                          double clip_norm, double sigma, uint64_t seed, double *out_aggregate);

/* --- RDP accountant lifecycle --- */

/* Zero-init. No allocation. */
void hu_dp_accountant_rdp_init(hu_dp_rdp_accountant_t *acct);

/* Record one sub-sampled Gaussian event. Updates rdp_alpha[] incrementally.
 *
 * Returns:
 *   HU_OK                       on success.
 *   HU_ERR_INVALID_ARGUMENT     if acct == NULL, sigma <= 0, sample_rate
 *                               outside [0, 1].
 *   HU_ERR_LIMIT_REACHED        if event_count == HU_DP_RDP_MAX_EVENTS. */
hu_error_t hu_dp_accountant_rdp_record(hu_dp_rdp_accountant_t *acct, double sigma,
                                       double sample_rate);

/* Compute current ε for the given δ via min_α RDP→(ε, δ) conversion
 * (Mironov 2017 Proposition 3).
 *
 * Returns ε on success, NaN on invalid input (acct == NULL, δ ∉ (0, 1)). */
double hu_dp_accountant_rdp_epsilon(const hu_dp_rdp_accountant_t *acct, double delta);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_DP_SGD_H */
