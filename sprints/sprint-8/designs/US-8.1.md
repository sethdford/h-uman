# Design for US-8.1: Real DP-SGD with RDP Accounting

**Priority:** P0
**Risk tier:** HIGH (DP math is security-sensitive; wrong σ silently invalidates every privacy claim the product makes)
**Estimate:** M
**Branch:** `sprint-8-verifiable-privacy`
**Worktree:** `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-8-privacy`

---

## 1. Approach

The current `learner.c` accountant does naive (additive) ε composition over a "per-sample" clip that actually clips the whole batch as one vector. Both flaws are silently lossy: callers get an ε number that is neither tight (naive composition is strictly looser than RDP) nor sound (no per-sample clip ⇒ no per-sample sensitivity bound ⇒ Abadi 2016 Theorem 1 does not apply at all).

The fix is **a new, side-by-side translation unit** that exposes correct primitives without touching the existing learner. Per the sprint-7 no-touch list and the story's out-of-scope notes, we deliver the correct module here; sprint-9 will compose it into the actual training loop. This keeps the blast radius tiny (4 new files, 0 modified) and lets us pin correctness with an oracle fixture before risking any wiring.

The design choice that earns its keep: extract the **calibration decision** (`hu_dp_sgd_noise_sigma`) and the **budget query** (`hu_dp_accountant_rdp_epsilon`) as **pure predicates** with no allocation, no I/O, and no global state. Per `.claude/rules/security-predicate-extraction.md`, security decisions buried inside training loops cannot be tested without spawning the training loop; extracted pure predicates are unit-testable against an oracle JSON in microseconds. The noise-addition step (`hu_dp_sgd_step`) is impure (PRNG, output buffer) but separable: the predicate decides σ, the step consumes σ.

We deliberately avoid the trap of a "step accountant that knows about training": the accountant tracks a sequence of `(sigma, sample_rate)` events and the ε query is a separate function. That mirrors Opacus's `RDPAccountant.get_epsilon(delta)` and lets the test suite replay an event log without ever calling `hu_dp_sgd_step`.

**Reference papers (must read before implementing):**
- Abadi, Chu, Goodfellow, McMahan, Mironov, Talwar, Zhang (CCS 2016). *Deep Learning with Differential Privacy.* §3.3 "The Moments Accountant" and Algorithm 1 (DP-SGD with per-example clipping). The clipping rule (clip each per-sample gradient to L2 norm `C` *before* aggregation) and the moments-accountant calibration are both there.
- Mironov (CSF 2017). *Rényi Differential Privacy.* §4 (composition) and §5 (sub-sampled Gaussian mechanism → RDP closed form). This is the actual accounting math; Abadi 2016's "moments accountant" is RDP with α as the moment order.
- Mironov, Talwar, Zhang (2019). *R\'enyi Differential Privacy of the Sampled Gaussian Mechanism* (arXiv:1908.10530). Use Theorem 4 closed form for `RDP_α(σ, q)` where `q = sample_rate`. This is what Opacus implements.

---

## 2. Files to modify

| File | Status | Change | Est. LOC |
|---|---|---|---|
| `include/human/ml/dp_sgd.h` | NEW | Public API: types + 5 function prototypes | +90 |
| `src/ml/dp_sgd.c` | NEW | Implementation; gated by `#ifdef HU_ENABLE_ML` | +320 |
| `tests/test_dp_sgd.c` | NEW | AC-coverage tests (8 functions) | +280 |
| `tests/fixtures/dp_accountant_oracle.json` | NEW | Reference (σ, q, steps, δ) → ε values from Opacus | +60 |
| `CMakeLists.txt` | MODIFY (1-line) | Add `src/ml/dp_sgd.c` to `human_ml` target sources | +1 |
| `tests/CMakeLists.txt` | MODIFY (1-line) | Add `tests/test_dp_sgd.c` to `human_tests` sources | +1 |

**Explicitly NOT touching** (per sprint-7 no-touch list and story §"Out of scope"):
- `src/ml/learner_cpu.c` — defective clip stays as-is; sprint-9 composes the fix.
- `src/ml/learner.c` — existing `hu_dp_accountant_t` (additive) stays as-is.
- `include/human/ml/learner.h` — existing `hu_dp_accountant_*` symbols stay as-is.
- `src/ml/dpo*`, `src/ml/cli.c`, `scripts/finetune-gemma.py`.

**Naming collision note.** `learner.h` already has `hu_dp_accountant_t` and `hu_dp_accountant_init/record_query/total_epsilon`. We MUST NOT shadow those. The new type is `hu_dp_rdp_accountant_t` and the new functions are `hu_dp_accountant_rdp_record` / `hu_dp_accountant_rdp_epsilon` (the prefix is `hu_dp_accountant_rdp_*`, not `hu_dp_rdp_accountant_*`, to match the story's spelling). The old additive type and the new RDP type coexist; sprint-9 deletes the additive one when composition lands.

---

## 3. Public types and function signatures

```c
/* include/human/ml/dp_sgd.h */
#ifndef HU_ML_DP_SGD_H
#define HU_ML_DP_SGD_H

#include <stddef.h>
#include <stdint.h>
#include "human/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of RDP events tracked per accountant.
 * 100k events at ~17B each = ~1.6 MB; bounded to keep allocation deterministic. */
#define HU_DP_RDP_MAX_EVENTS 100000

/* Moments-accountant α grid. Opacus uses this same grid; outside it we have
 * observed loose / NaN behavior on extreme σ. Range chosen per Mironov 2017 §5. */
#define HU_DP_RDP_ALPHA_MIN 2
#define HU_DP_RDP_ALPHA_MAX 64

/* One sub-sampled-Gaussian event in the privacy ledger. */
typedef struct hu_dp_rdp_event {
    double sigma;        /* noise multiplier; must be > 0 */
    double sample_rate;  /* q in [0, 1] */
} hu_dp_rdp_event_t;

/* RDP moments accountant. Fixed capacity, no allocation after init. */
typedef struct hu_dp_rdp_accountant {
    hu_dp_rdp_event_t events[HU_DP_RDP_MAX_EVENTS];
    size_t event_count;
    /* Running RDP_α for each α in the grid; reset to 0 at init.
     * Stored incrementally so hu_dp_accountant_rdp_epsilon is O(α_count), not O(events*α_count). */
    double rdp_alpha[HU_DP_RDP_ALPHA_MAX - HU_DP_RDP_ALPHA_MIN + 1];
} hu_dp_rdp_accountant_t;

/* --- Pure predicates (no I/O, no allocation, no global state) --- */

/* Calibrate Gaussian noise σ to hit target_epsilon at target_delta after
 * `steps` sub-sampled Gaussian queries at sampling rate `sample_rate` over a
 * dataset of `dataset_size` samples. clip_norm fixes per-sample L2 sensitivity.
 *
 * Returns σ (>0) on success, or NaN if no σ in the search range satisfies the
 * budget (caller MUST check via isnan()).
 *
 * Implementation: binary search on σ in [0.1, 100.0], inner loop calls
 * hu_dp_rdp_epsilon_from_sigma. ~50 iterations to converge to 1e-4 relative
 * tolerance. */
double hu_dp_sgd_noise_sigma(double clip_norm,
                             double target_epsilon,
                             double target_delta,
                             size_t steps,
                             size_t dataset_size,
                             double sample_rate);

/* Inverse: given a completed run of `steps` queries at noise multiplier σ
 * and sampling rate q, return the (ε, δ) ε actually consumed.
 *
 * Returns ε (>=0) on success, or NaN on invalid input (σ<=0, q∉[0,1], etc.). */
double hu_dp_rdp_epsilon_from_sigma(double sigma,
                                    double sample_rate,
                                    size_t steps,
                                    double delta);

/* --- DP-SGD step (impure: writes output, consumes PRNG) --- */

/* Per-sample clip + aggregate + Gaussian noise.
 *
 * Inputs:
 *   per_sample_grads : flattened row-major matrix [batch_size × num_params].
 *                      Row i is sample i's gradient vector.
 *   batch_size       : number of rows; MUST be >= 1.
 *   num_params       : number of columns; MUST be >= 1.
 *   clip_norm        : per-row L2 clip threshold (e.g. 1.0).
 *   sigma            : Gaussian noise multiplier; noise σ = sigma * clip_norm.
 *                      sigma == 0.0 disables noise (test-only path).
 *   seed             : PRNG seed; determinism contract is keyed on this.
 *   out_aggregate    : caller-allocated [num_params] output vector.
 *
 * Returns:
 *   HU_OK                       on success.
 *   HU_ERR_INVALID_ARGUMENT     if batch_size==0, num_params==0, clip_norm<=0,
 *                               sigma<0, or any pointer is NULL.
 *
 * Determinism: byte-for-byte identical output for identical (inputs, seed). */
hu_error_t hu_dp_sgd_step(const double *per_sample_grads,
                          size_t batch_size,
                          size_t num_params,
                          double clip_norm,
                          double sigma,
                          uint64_t seed,
                          double *out_aggregate);

/* --- RDP accountant lifecycle --- */

/* Zero-init. No allocation. */
void hu_dp_accountant_rdp_init(hu_dp_rdp_accountant_t *acct);

/* Record one sub-sampled Gaussian event. Updates rdp_alpha[] incrementally.
 *
 * Returns:
 *   HU_OK                       on success.
 *   HU_ERR_INVALID_ARGUMENT     if acct==NULL, sigma<=0, sample_rate∉[0,1].
 *   HU_ERR_OUT_OF_RESOURCES     if event_count == HU_DP_RDP_MAX_EVENTS. */
hu_error_t hu_dp_accountant_rdp_record(hu_dp_rdp_accountant_t *acct,
                                       double sigma,
                                       double sample_rate);

/* Compute current ε for the given δ via min_α RDP→(ε,δ) conversion
 * (Mironov 2017 Proposition 3).
 *
 * Returns ε on success, NaN on invalid input (delta∉(0,1), acct==NULL). */
double hu_dp_accountant_rdp_epsilon(const hu_dp_rdp_accountant_t *acct,
                                    double delta);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_DP_SGD_H */
```

**Internal helpers (static in `dp_sgd.c`, not exported):**

```c
/* Sampled Gaussian mechanism RDP: Mironov–Talwar–Zhang 2019 Theorem 4.
 * For sample_rate q ≈ 0 we use the small-q approximation; else numerical
 * stable log-sum-exp form. */
static double rdp_sampled_gaussian(double alpha, double sigma, double q);

/* Convert RDP_α at order α to (ε, δ): ε = RDP_α + log(1/δ) / (α-1).
 * (Mironov 2017 Proposition 3, restated by Canonne et al. 2020.) */
static double rdp_to_dp_epsilon(double rdp_alpha_value, double alpha, double delta);

/* L2 norm of a row. */
static double l2_norm(const double *v, size_t n);

/* Box-Muller standard normal from a 64-bit splitmix64-seeded PRNG.
 * Determinism: same seed ⇒ same sequence on any platform. Do NOT use rand(). */
static double prng_standard_normal(uint64_t *state);
```

---

## 4. Test seam plan

**Oracle fixture** `tests/fixtures/dp_accountant_oracle.json` — generated once from Opacus 1.5.x's `RDPAccountant.get_epsilon()`, committed as a fixed reference. Shape:

```json
{
  "_provenance": {
    "tool": "opacus.accountants.RDPAccountant",
    "version": "1.5.x",
    "generated": "2026-05-17",
    "script": "scripts/gen_dp_oracle.py"
  },
  "cases": [
    {
      "name": "abadi_table1_workload",
      "sigma": 1.1, "sample_rate": 0.01, "steps": 1000, "delta": 1e-5,
      "expected_epsilon": 2.95, "tolerance_abs": 0.05
    },
    {
      "name": "tight_budget",
      "sigma": 4.0, "sample_rate": 0.001, "steps": 10000, "delta": 1e-5,
      "expected_epsilon": 0.45, "tolerance_abs": 0.02
    },
    {
      "name": "high_q_short_run",
      "sigma": 1.0, "sample_rate": 0.1, "steps": 100, "delta": 1e-5,
      "expected_epsilon": 1.85, "tolerance_abs": 0.05
    },
    {
      "name": "sanity_lower_bound",
      "sigma": 10.0, "sample_rate": 0.01, "steps": 100, "delta": 1e-5,
      "expected_epsilon": 0.06, "tolerance_abs": 0.01
    }
  ]
}
```

Tolerance is absolute (not relative) so the test is robust to log-scale ε near 0.

**Test function names → AC mapping:**

| AC | Test function | What it asserts |
|---|---|---|
| AC-8.1.1 | `test_dp_sgd_noise_sigma_calibrates_to_abadi_table1_bounds` | `0.5 <= σ <= 5.0` for the stated (ε=8, δ=1e-5, 1000 steps, q=0.01, C=1); documented expectation σ ≈ 1.1. Bonus: `test_dp_sgd_noise_sigma_returns_nan_on_unsatisfiable_budget` (ε=0.001, q=0.5, 100k steps → NaN). |
| AC-8.1.2 | `test_dp_sgd_step_clips_oversized_per_sample_to_clip_norm` | 4×N matrix, row 0 norm 3.0, rows 1–3 norm 0.5. With σ=0, aggregate L2 ≤ 4·C = 4.0; row 0 post-clip norm = 1.0 ± 1e-5. **Adversarial pin: row 0 MUST be scaled down. Assertion is `HU_ASSERT_LE(post_clip_l2, 1.0 + 1e-5)`, not `HU_ASSERT_GE`**. Per `.claude/rules/tests-that-pin-bugs.md`, the dangerous case (unclipped row 0 contributes norm 3.0) MUST fail the test. |
| AC-8.1.3 | `test_dp_sgd_accountant_rdp_epsilon_tighter_than_naive` | Record 100 events at (σ=1.1, q=0.01, δ=1e-5); assert ε < 8.0. Companion `test_dp_sgd_accountant_matches_opacus_oracle` iterates `dp_accountant_oracle.json` cases. |
| AC-8.1.4 | `test_dp_sgd_step_determinism_same_seed_same_output` | Two calls, same inputs + seed=42 → `memcmp(out_a, out_b, n*sizeof(double)) == 0`. Companion `test_dp_sgd_step_different_seed_different_output` proves PRNG actually consumes the seed. |
| AC-8.1.5 | `test_dp_sgd_step_rejects_zero_batch_size` | `batch_size=0` ⇒ returns `HU_ERR_INVALID_ARGUMENT`; output buffer untouched (assert canary bytes intact). **Adversarial: dangerous case is "step proceeds with zero-batch, divides by zero, NaN noise added". Assertion blocks that path.** Per same rule, name accurately describes the rejection contract. |

**Bonus (non-AC but per DoD)** — `test_dp_sgd_references_production_symbols`: a no-op test whose presence makes `scripts/check-test-references.sh` pass (satisfies `.claude/rules/test-references-production-symbol.md`). The other tests already call `hu_dp_sgd_step`, `hu_dp_sgd_noise_sigma`, `hu_dp_accountant_rdp_epsilon` — so this is automatic; no `// @covers-none` escape hatch needed.

**PRNG seeding strategy.** `prng_standard_normal` uses splitmix64 over a `uint64_t` state seeded from the `seed` parameter. Determinism contract is: same `seed` ⇒ same byte sequence ⇒ same Box-Muller pairs ⇒ same noise ⇒ same `out_aggregate`. The PRNG state lives on the stack in `hu_dp_sgd_step`; no global RNG, no thread-local, no `srand()`. This is the only path the determinism test (AC-8.1.4) can rely on.

---

## 5. Risk assessment

### R1: σ-calibration off by factor 2 — HIGH probability, LARGE impact
**Failure mode.** If `hu_dp_sgd_noise_sigma` returns σ/2 instead of σ (e.g., dropping a factor of 2 in the Gaussian mechanism sensitivity from `2·C` to `C`, or applying clip_norm twice), the noise added is half what it should be. The reported ε in `human doctor --privacy` would be exactly half the true ε; users get an unsound bound while the system claims compliance with their chosen budget. This is **strictly worse than no DP** because users would skip other protections trusting the (false) guarantee.
**Mitigation.**
1. Oracle JSON fixture (`dp_accountant_oracle.json`) cross-checked against Opacus 1.5.x — a third-party implementation. If our σ differs by >5% from Opacus on the Abadi 2016 Table 1 workload, the test fails immediately. **A factor-of-2 error would shift ε by ~4× and trip the 5% tolerance on every oracle case.**
2. The story-specified AC-8.1.1 bound `0.5 ≤ σ ≤ 5.0` catches gross calibration errors (off by >2× on either side).
3. We compute `RDP_α(σ, q)` directly (Mironov–Talwar–Zhang Theorem 4) rather than indirectly through Abadi's privacy amplification lemma; closed-form is harder to misimplement than a bound.
4. The internal `rdp_sampled_gaussian` helper is independently tested at fixed α against a hand-computed value (added as a 6th oracle case at α=4, σ=1.0, q=0.0 — degenerate case where `RDP_α = α/(2σ²) = 2.0`).

### R2: α-range too narrow — MEDIUM probability, MEDIUM impact
**Failure mode.** Mironov 2017 Proposition 3 says ε = min_α [RDP_α + log(1/δ)/(α-1)]. The min is taken over the α grid. If our `[2, 64]` grid excludes the optimal α* for the given (σ, q, steps, δ), we get a **loose but sound** upper bound (this is safe — never too small). However, the calibration in `hu_dp_sgd_noise_sigma` would then pick a larger σ than needed (over-noising). Concretely, very small σ + large steps can push α* > 64.
**Mitigation.**
1. Use Opacus's grid `[2, 3, 4, ..., 64]` as a deliberate choice — Opacus is empirically robust across the (σ, q) regimes documented in their paper and the workloads sprint-9 will hit.
2. Internal assertion: if the argmin α lands at the **boundary** (α==2 or α==64), log a debug warning (under `HU_IS_TEST`) so we know to widen the grid. Production behavior is unchanged (still sound, just possibly loose).
3. Add `test_dp_sgd_alpha_range_does_not_saturate_for_typical_workloads`: iterates 4 representative (σ, q, steps) and asserts the argmin α is **strictly interior** (3 ≤ α* ≤ 63).
4. Sound-direction reminder: a too-narrow α range over-reports ε, which over-noises — not a safety violation, just a utility regression. The opposite (under-reporting ε) would require a math bug, not a grid bug.

### R3: PRNG non-determinism — LOW probability, MEDIUM impact
**Failure mode.** If `prng_standard_normal` consults global state or platform-specific `rand()`, AC-8.1.4 fails on parallel test runs or on a different OS. The implication is bigger than test flake: training-time reproducibility (essential for debugging privacy regressions) breaks.
**Mitigation.** splitmix64 is fully on-stack, deterministic, and platform-portable. Test on both macOS and Linux in CI (`ci.yml` already does this). No `time(NULL)`, no `getrandom()`, no `/dev/urandom`.

### R4: Floating-point platform divergence — LOW probability, SMALL impact
**Failure mode.** `exp`, `log`, `log1p` may differ in last bits between glibc and Apple's libm. AC-8.1.4 (byte-for-byte determinism) could fail across platforms.
**Mitigation.** AC-8.1.4 is **same-machine** determinism (two consecutive calls), not cross-platform. The oracle test (AC-8.1.3) uses absolute tolerance `0.05` on ε, well above any libm last-bit drift. Document this explicitly in the test file header.

### R5: Per-sample clip via flat-buffer indexing bug — MEDIUM probability, LARGE impact
**Failure mode.** `per_sample_grads` is `[batch × params]` row-major. An off-by-one or wrong-stride bug could clip *columns* (per-parameter) instead of *rows* (per-sample), invalidating the sensitivity argument entirely.
**Mitigation.** AC-8.1.2 is specifically designed to catch this: row 0 has L2 norm 3.0 (concentrated norm in one row), rows 1–3 have norm 0.5 each. A per-column clip would not reduce row 0's contribution to exactly norm 1.0; the assertion `post_clip_row0_norm == 1.0 ± 1e-5` would fail loudly. We add `test_dp_sgd_step_clips_per_row_not_per_column` as an explicit dual: a matrix where the *column* norms are large but row norms are small — clip should NOT trigger.

### R6: Backward compat with existing `hu_dp_accountant_t` — LOW probability, SMALL impact
**Failure mode.** Linker collision if `learner.h`'s `hu_dp_accountant_t` and `dp_sgd.h`'s new type accidentally share a name.
**Mitigation.** New type is `hu_dp_rdp_accountant_t` (distinct typedef name). New function names use the `hu_dp_accountant_rdp_*` namespace; old ones stay `hu_dp_accountant_*` (no `rdp` infix). Compiler will fail loudly if names collide.

### R7: Observability — MEDIUM probability, SMALL impact
**Failure mode.** If σ-calibration silently returns NaN (unsatisfiable budget) and a caller forgets the `isnan()` check, training proceeds with NaN noise — every gradient becomes NaN, training is bricked, no privacy bound issued.
**Mitigation.** Document the NaN contract loudly in the header. The first caller (sprint-9 wiring) MUST `HU_ASSERT(!isnan(sigma))` before using it. Add `test_dp_sgd_noise_sigma_returns_nan_on_unsatisfiable_budget` to make the contract explicit.

---

## 6. Implementation sequencing (commit order for implementer)

Each step ends with the relevant tests green; full suite green before moving on.

1. **Commit 1: Oracle fixture + scaffolding.**
   - Add `tests/fixtures/dp_accountant_oracle.json` (4 cases from Opacus).
   - Add `tests/fixtures/README.md` documenting provenance.
   - Add `scripts/gen_dp_oracle.py` (one-shot Python; not run by CI, but committed for reproducibility).
   - No C code yet. Fixture is the contract; subsequent commits prove the code meets it.

2. **Commit 2: Pure predicates (`rdp_sampled_gaussian`, `rdp_to_dp_epsilon`, `hu_dp_rdp_epsilon_from_sigma`).**
   - `include/human/ml/dp_sgd.h` with prototypes for the pure-function subset.
   - `src/ml/dp_sgd.c` with just these helpers + the public `hu_dp_rdp_epsilon_from_sigma`.
   - `tests/test_dp_sgd.c` with `test_dp_sgd_accountant_matches_opacus_oracle` and `test_dp_sgd_alpha_range_does_not_saturate_for_typical_workloads`.
   - Wire into CMake.

3. **Commit 3: Accountant lifecycle (`hu_dp_accountant_rdp_init/record/epsilon`).**
   - Adds the accumulator struct + 3 functions.
   - `test_dp_sgd_accountant_rdp_epsilon_tighter_than_naive` (AC-8.1.3).
   - Reuses oracle fixture by recording events one-by-one and comparing to the closed-form `hu_dp_rdp_epsilon_from_sigma` (cross-check: incremental record should equal one-shot computation).

4. **Commit 4: σ calibration (`hu_dp_sgd_noise_sigma`).**
   - Binary search over σ.
   - `test_dp_sgd_noise_sigma_calibrates_to_abadi_table1_bounds` (AC-8.1.1).
   - `test_dp_sgd_noise_sigma_returns_nan_on_unsatisfiable_budget` (R7 mitigation).

5. **Commit 5: Step function (`hu_dp_sgd_step` + PRNG).**
   - Per-row clip + sum + Gaussian noise.
   - `test_dp_sgd_step_clips_oversized_per_sample_to_clip_norm` (AC-8.1.2).
   - `test_dp_sgd_step_clips_per_row_not_per_column` (R5 dual).
   - `test_dp_sgd_step_determinism_same_seed_same_output` + `test_dp_sgd_step_different_seed_different_output` (AC-8.1.4).
   - `test_dp_sgd_step_rejects_zero_batch_size` + null-pointer + zero-params + negative-sigma rejection tests (AC-8.1.5).

6. **Commit 6: Final sweep.**
   - Full suite green (10,000+ tests, 0 ASan errors).
   - `/verify` PASS.
   - `scripts/check-test-references.sh` green.
   - Update `sprints/sprint-8/evidence/US-8.1-evidence.md` with test counts + oracle case results.

---

## 7. Anti-patterns to avoid (cited)

### A1: Adversarial tests that lock the bug
From `.claude/rules/tests-that-pin-bugs.md`:
> "Adversarial tests should assert the dangerous case is BLOCKED, not accepted."

AC-8.1.2 and AC-8.1.5 are adversarial. They MUST be phrased as:
- `HU_ASSERT_LE(post_clip_norm, 1.0 + 1e-5)` — **dangerous case (norm 3.0 unscaled) MUST fail** the assertion.
- `HU_ASSERT_EQ(hu_dp_sgd_step(...batch=0...), HU_ERR_INVALID_ARGUMENT)` — **dangerous case (returns HU_OK with NaN output) MUST fail.**

NOT as:
- ~~`HU_ASSERT_TRUE(post_clip_norm > 0)`~~ — passes for the buggy unscaled case.
- ~~`HU_ASSERT_TRUE(result != HU_OK || true)`~~ — passes for everything.

The verifier checks test names against assertions: if the implementer writes `test_dp_sgd_step_rejects_zero_batch_size` and the assertion accepts the dangerous case, the audit explicitly calls it out as a Sprint-8 regression of the Sprint-7 audit pattern.

### A2: Security-decision predicate buried in the impure step
From `.claude/rules/security-predicate-extraction.md`:
> "For any security decision that lives inside a hard-to-test boundary, extract the decision into a pure predicate function."

The σ-calibration decision lives in `hu_dp_sgd_noise_sigma` — a pure predicate, no allocation, no I/O, no global state. Testable directly against the oracle fixture without ever invoking the impure step. Likewise `hu_dp_rdp_epsilon_from_sigma`. The two impure boundary functions (`hu_dp_sgd_step`, `hu_dp_accountant_rdp_record`) only **act** on the decisions; they do not **make** privacy-relevant decisions.

**Specifically:** the budget-exhaustion question ("did this run actually fit in target_epsilon?") is answered by calling the pure predicate `hu_dp_accountant_rdp_epsilon(acct, delta) <= target_epsilon`. We do NOT bake a `budget_exhausted` boolean into the accountant struct or compute it inside `hu_dp_accountant_rdp_record`. That keeps the decision testable and re-evaluable at any δ.

### A3: Test inlines production code
From `.claude/rules/test-references-production-symbol.md`:
The test file must reference `hu_dp_sgd_*` production symbols, not re-implement the math. Specifically forbidden: local `static double local_rdp(...)` in `test_dp_sgd.c`. All math in the tests goes through public production symbols.

### A4: "Looks correct" without running
From `~/.claude/rules/quality-gates.md`:
The implementer MUST run `/verify` and capture evidence. The oracle fixture is the evidence; "I implemented the formula from the paper" is not.

---

## 8. Acceptance criteria mapping

| AC | Test function | Tolerance | Adversarial? |
|---|---|---|---|
| AC-8.1.1 | `test_dp_sgd_noise_sigma_calibrates_to_abadi_table1_bounds` | `0.5 ≤ σ ≤ 5.0` (hard); expected ≈ 1.1 (informational) | No |
| AC-8.1.2 | `test_dp_sgd_step_clips_oversized_per_sample_to_clip_norm` | aggregate L2 ≤ 4.0; row-0 norm = 1.0 ± 1e-5 | **Yes** |
| AC-8.1.3 | `test_dp_sgd_accountant_rdp_epsilon_tighter_than_naive` | ε < 8.0 (strict); cross-checked against oracle | No |
| AC-8.1.4 | `test_dp_sgd_step_determinism_same_seed_same_output` | byte-for-byte `memcmp == 0` | No |
| AC-8.1.5 | `test_dp_sgd_step_rejects_zero_batch_size` | returns `HU_ERR_INVALID_ARGUMENT`; output buffer canaries intact | **Yes** |

All five ACs are testable without invoking any existing learner code. The story can ship and be verified end-to-end inside `tests/test_dp_sgd.c`.

---

## 9. Definition of Done (from story §DoD, restated for the implementer)

- [ ] Full suite 10,000+ tests green, 0 ASan errors.
- [ ] `/verify` PASS.
- [ ] `/aspect-panel` CLEAN.
- [ ] `tests/test_dp_sgd.c` references `hu_dp_sgd_step`, `hu_dp_sgd_noise_sigma`, `hu_dp_accountant_rdp_epsilon` (real production symbols).
- [ ] Adversarial ACs (8.1.2, 8.1.5) assert the dangerous case is BLOCKED.
- [ ] Oracle fixture cross-check passes for all 4 cases within stated tolerance.
- [ ] No modification to `src/ml/learner_cpu.c`, `src/ml/learner.c`, or `include/human/ml/learner.h`.
- [ ] Evidence document at `sprints/sprint-8/evidence/US-8.1-evidence.md` capturing oracle results and final ε numbers.

---

## 10. Open questions for implementer (none blocking)

- The Mironov–Talwar–Zhang 2019 closed form has a small-q approximation regime. The implementer should pick the threshold `q < q_threshold ⇒ use approximation` empirically by checking which form Opacus uses at q=0.01 — both should agree on the oracle cases. If they don't, fall back to the numerically stable log-sum-exp form everywhere.
- The α grid uses integers `[2..64]`. RDP is defined for real α > 1; integer-α is what Opacus uses and what Abadi 2016 recommends for computational stability. Stick with integers unless a future story requires sub-integer resolution.

RESULT_tech-lead=DESIGN_READY
