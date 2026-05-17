# Sprint 12 Backlog — Prove the Metric on Real Data

## Goal

Close the three auditor-mandated entry conditions from Sprint 11 (DPOP numerical correctness,
real MLX YNTP inference, real KL drift gate), run DPOP+DoRA+ORPO on Seth's actual holdout,
and produce a falsifiable, publishable improvement-or-negative result.

---

## Sprint success metric

The sprint closes when the following claim can be made with empirical evidence:

> "On Seth's chat.db n=30 holdout, DPOP+DoRA on Gemma-4-E2B produces
> `delta_nll = X%` vs base+system-prompt persona baseline, with pad-token
> leakage at Y% (expected zero from US-11.1 masking), verified by running
> `scripts/yntp_eval.py` end-to-end against a real MLX Gemma-4 adapter
> on Seth's machine. Golden output committed to
> `sprints/sprint-12/evidence/US-12.4/`."

If X is negative (adapter hurts), this is still a valid publishable result — it
triggers OQ-12.4 and informs re-design. It does NOT trigger sprint failure.

---

## Wave plan

```
Wave 0 (P0; parallel; entry-condition blockers):
  US-12.1 — DPOP numerical golden tests (FU-11.4.a)         [no deps]
  US-12.2 — Real MLX YNTP inference (FU-11.6.b)             [no deps]
  US-12.3 — Real KL drift inference (FU-11.8.f)             [no deps]

Wave 1 (P0; after Wave 0 merges; the publishable run):
  US-12.4 — Real training run + publishable comparison       [US-12.1, US-12.2, US-12.3]

Wave 2 (P1; after Wave 1 is committed):
  US-12.5 — Production ORPO forward-pass wiring (FU-11.5.a) [US-12.1]
  US-12.6 — Adapter-rollback PID lock (FU-11.8.h)           [no deps]

Wave 3 (P2; stretch; only if Waves 0-2 land clean):
  US-12.7 — Stage 2 array-env pad-gate fix + cascade defense-in-depth (FU-11.7.d/e)  [no deps]
  US-12.8 — Status-writer test and AC-11.8.5 correctness (FU-11.8.g)                 [no deps]
```

---

## User Stories (priority order)

---

### US-12.1 (P0): As a developer verifying DPOP loss correctness, I want three numerical golden tests for the DPOP penalty term so that the optimizer cannot silently skip the Degraded-Chosen-Response guard.

**Source:** FU-11.4.a — sprint-auditor mandatory entry condition #1; design §3.2 (line 86, line 114) explicitly mandates these tests; AC-11.4.2/3/4 are NOT_DELIVERED per the auditor's Phase 4 finding.

**Rationale:** Sprint 11 shipped argv-shape tests for DPOP but never verified the loss math numerically. Design §3.2 enumerates three golden tests by name. Without them we are trusting upstream `mlx-lm-lora`'s DPOP implementation blindly — exactly the same corner-cut that produced Sprint 8's pad-leakage failure (sprint-11 retro §"What broke or surprised" #2). The three missing ACs are the Sprint 8 DCR regression guard: if we cannot show numerically that DPOP fires on DCR and stays zero on healthy-chosen, the guard does not exist. This story also lands `tests/fixtures/dpop_golden.json` per design §2 file plan, which serves as the human-readable derivation anchor.

**Acceptance criteria:**

- AC-12.1.1: GIVEN a fixture batch where `log_pi_ref(y_chosen) = -3.0` and `log_pi(y_chosen) = -5.0` (the Degraded Chosen Response condition, `log_pi < log_pi_ref`), WHEN `compute_dpop_loss(batch, dpop_lambda=0.1)` is called, THEN the returned loss is strictly greater than the vanilla DPO loss on the same batch by exactly `0.1 * ((-3.0) - (-5.0)) = 0.2 nats` within 1e-5 tolerance; verified by `tests/test_dpop_loss.py::test_dpop_penalty_fires_on_dcr_condition`.

- AC-12.1.2: GIVEN a fixture batch where `log_pi(y_chosen) = -2.0` and `log_pi_ref(y_chosen) = -3.0` (healthy-chosen; model is ABOVE reference), WHEN `compute_dpop_loss(batch, dpop_lambda=0.1)` is called, THEN the penalty term is exactly 0.0 and the loss equals the vanilla DPO loss within 1e-5; verified by `tests/test_dpop_loss.py::test_dpop_penalty_zero_on_healthy_chosen`.

- AC-12.1.3 (Sprint 8 regression guard): GIVEN a synthetic fixture that replays the Sprint 8 iter-80 gradient direction (chosen_r went to -8.867 under vanilla DPO; `log_pi_ref(y_chosen) = -2.5`, `log_pi(y_chosen) = -11.4`), WHEN the same gradient step is simulated under DPOP with `lambda=0.1`, THEN `train_chosen_r` stays non-negative (the penalty clamps the chosen-log-prob degradation); verified by `tests/test_dpop_loss.py::test_sprint8_iter80_dcr_prevented_by_dpop`. This is the load-bearing regression guard.

- AC-12.1.4: `tests/fixtures/dpop_golden.json` is committed to the repo containing human-readable derivation of the three fixture batch values (the log-probability inputs, expected output, tolerance, and citation to Pal et al. Smaug arXiv 2402.13228 §3); verified by a fixture-validation test `tests/test_dpop_loss.py::test_dpop_golden_json_derivation_loadable`.

- AC-12.1.5: GIVEN the existing `tests/test_dpop.py::test_argparse_exposes_dpop_flags` test (FU-11.4.c brittleness), WHEN Sprint 12 lands, THEN the test is refactored to use direct `argparse.ArgumentParser` reconstruction (no subprocess spawn against the real script) so CI never breaks on import-time side effects; verified by the test passing in a clean venv without mlx installed.

- AC-12.1.6: GIVEN `getattr(args, "dpo_cpo_loss_type", "sigmoid")` at `scripts/finetune-gemma.py:540`, WHEN an unvalidated string is passed, THEN a `ValueError` is raised immediately from a `VALID_LOSS_TYPES` guard before any subprocess is spawned; verified by `tests/test_dpop_loss.py::test_invalid_loss_type_raises_valueerror` (FU-11.4.b).

- AC-12.1.7: GIVEN `HU_IS_TEST` is defined, WHEN any test in `test_dpop_loss.py` runs, THEN no real MLX weights are loaded; all log-probability values are fixture inputs; verified by asserting no `mlx` import is triggered.

**Estimate:** S
**Priority:** P0 (Wave 0; entry condition; blocks US-12.4)
**Risk tier:** LOW (Python tests + fixture file; no C changes; no vtable changes)
**Dependencies:** none
**Test seam:** `tests/test_dpop_loss.py`; `tests/fixtures/dpop_golden.json`
Command: `python3 -m pytest tests/test_dpop_loss.py -v`
**Out of scope:** Re-implementing DPOP loss math in Python (the test exercises the contract of the existing mlx-lm-lora DPOP path via analytical fixtures, not by re-writing the loss); ORPO golden tests (separate story); C-layer DPOP factory.

---

### US-12.2 (P0): As a developer making a publishable improvement claim, I want `scripts/yntp_eval.py` to run real Gemma-4 teacher-forced log-probability inference via MLX so that the YNTP gate produces real numbers and not NotImplementedError.

**Source:** FU-11.6.b — sprint-auditor mandatory entry condition #2. `scripts/yntp_eval.py:_real_compute_logprob` raises `NotImplementedError` unconditionally. Decision D1 (BINDING): the 30-row private holdout lives only at `~/.human/private/yntp_holdout_30.jsonl` on Seth's machine; the golden output is committed to `sprints/sprint-12/evidence/US-12.2/` per design doc §4 Risk 1 mitigation.

**Rationale:** The entire Sprint 11-12 publishable claim rests on `yntp_eval.py` producing real log-likelihood numbers from a real Gemma-4-E2B adapter run. Sprint 11 correctly deferred this to Sprint 12 (the NLL gate logic is sound; the mock seam validates the aggregation, gate decision, and regression guard). Now it is the linchpin: until `_real_compute_logprob` runs successfully on Seth's machine and the output is committed to evidence, the "Sprint 11 goal: publishable claim backed by empirical evidence" (stories.md §sprint-success-metric) is unmet. The D1 fixture policy (synthetic-5 in CI, real-30 on Seth's machine) is preserved. Tests use mocks; the end-to-end run on Seth's machine produces the committed golden output.

**Acceptance criteria:**

- AC-12.2.1: GIVEN `mlx_lm` and a real Gemma-4-E2B model are available, WHEN `_real_compute_logprob(prompt, continuation, model, tokenizer)` is called, THEN it returns a finite negative float (the sum of per-token log-probabilities of `continuation` given `prompt`), NOT `NotImplementedError`; verified by `tests/test_yntp_eval_mlx.py::test_real_compute_logprob_signature_not_notimplemented` using a mock `mlx_lm` module that returns deterministic logits.

- AC-12.2.2: GIVEN a mock `mlx_lm` that returns uniform logits over a 256-token vocab for a 5-token continuation, WHEN `_real_compute_logprob` is called, THEN the returned value equals `-5 * log(256)` within 1e-4; verified by `tests/test_yntp_eval_mlx.py::test_real_compute_logprob_uniform_logits_golden`.

- AC-12.2.3: GIVEN the real MLX path and a continuation containing `<pad>` tokens, WHEN `_real_compute_logprob` is called, THEN pad-token log-probabilities are excluded from the sum (the Sprint 11 US-11.1 masking contract extends to the evaluator); verified by `tests/test_yntp_eval_mlx.py::test_real_logprob_excludes_pad_tokens`.

- AC-12.2.4: GIVEN `mlx_lm` is NOT importable (CI, clean checkout), WHEN `yntp_eval.py` runs without `--mock-from-jsonl`, THEN the script exits with a clear error message referencing `docs/plans/2026-05-10-m3-frontier-model-bridge.md` (not a bare ImportError traceback); verified by `tests/test_yntp_eval_mlx.py::test_missing_mlx_exits_cleanly_with_bridge_reference`.

- AC-12.2.5 (real machine run — requires Seth's machine): GIVEN `HU_YNTP_HOLDOUT=~/.human/private/yntp_holdout_30.jsonl` and a real Gemma-4-E2B model at the configured path, WHEN `python3 scripts/yntp_eval.py --adapter <base_only>` runs (base+system-prompt baseline pass, no adapter weights), THEN the output JSON is committed to `sprints/sprint-12/evidence/US-12.2/baseline_yntp_result.json`; verified by the committed file containing `"n_pairs": 30`, `"source": "real"`, and a finite `"base_mean_ll"`.

- AC-12.2.6: The pre-commit hook `scripts/check_no_yntp_holdout_staged.sh` continues to pass: the 30-row private holdout and any `baseline_yntp_result.json` that could contain conversation content are git-ignored; the evidence file committed to `sprints/sprint-12/evidence/US-12.2/` contains only aggregate statistics (mean, delta, gate_decision), not raw prompts; verified by the hook passing on the evidence commit.

**Estimate:** M
**Priority:** P0 (Wave 0; entry condition; blocks US-12.4)
**Risk tier:** HIGH (real model inference on Seth's machine; PII policy per D1 governs what is committable; test seam uses mocks but AC-12.2.5 requires a real run)
**Dependencies:** none (D1 fixture policy is already binding; no new stakeholder decisions needed)
**Test seam:** `tests/test_yntp_eval_mlx.py` with mock `mlx_lm`; real run on Seth's machine for AC-12.2.5
Command (CI): `python3 -m pytest tests/test_yntp_eval_mlx.py -v`
Command (Seth's machine): `HU_YNTP_HOLDOUT=~/.human/private/yntp_holdout_30.jsonl python3 scripts/yntp_eval.py --adapter base`
**Out of scope:** Multi-user evaluation; extending the holdout beyond 30 rows this sprint (see OQ-12.1); automatic chat.db extraction pipeline; model downloading automation.

---

### US-12.3 (P0): As an operator running the W14 nightly retrain cron, I want `scripts/compute_kl_drift.py` to compute real KL(base || candidate) divergence against the 200-prompt probe set so that the 0.5 nats tau gate actually rejects high-drift adapters rather than always passing.

**Source:** FU-11.8.f — sprint-auditor mandatory entry condition #3. `scripts/compute_kl_drift.py` returns `{kl_nats: 0.0, source: "stub"}` when torch/transformers are unavailable. The C runner detects the stub and emits `lora_retrain_kl_gate_stubbed` (Sprint 11 CRITICAL fix) but the gate remains observability-only. Sprint 12 must implement real `KL(base || candidate)` so production W14 actually enforces the tau.

**Rationale:** The dual fast/slow LoRA loop (US-11.8) gates EMA promotion on two signals: the 4-stage Pareto gate and the KL drift gate. Sprint 11 correctly identified the KL gate as "stubbed" and added an observability event. But an observability event is not enforcement: a stub that emits `lora_retrain_kl_gate_stubbed` still lets the EMA proceed. Sprint 12 must wire the real path. The probe set question is OQ-12.2. Tests use mock inference; the production path requires MLX on Seth's machine.

**Acceptance criteria:**

- AC-12.3.1: GIVEN `torch` and a `transformers`-style tokenizer are available, WHEN `_try_real_kl(base, candidate, probe_path)` is called with the 200-prompt probe set, THEN it returns a dict `{"kl_nats": <positive float>, "source": "real"}` (not `None` and not stub); verified by `tests/test_compute_kl_drift.py::test_real_kl_path_returns_real_source` with a mock model that returns uniform token probabilities.

- AC-12.3.2: GIVEN a mock base model that assigns probability 0.5 to token A and 0.5 to token B, and a candidate model that assigns 0.9 to A and 0.1 to B, WHEN `_try_real_kl` is called with a single-token probe, THEN the returned `kl_nats` equals `KL(P_base || P_cand) = 0.5 * log(0.5/0.9) + 0.5 * log(0.5/0.1)` within 1e-4; verified by `tests/test_compute_kl_drift.py::test_kl_numerical_golden`.

- AC-12.3.3: GIVEN a probe set of 200 prompts and a candidate adapter whose mean token-level KL from base exceeds 0.5 nats, WHEN `compute_kl_drift.py` runs, THEN the C runner's `hu_lora_compute_kl_drift` receives a `kl_nats > tau` result and the EMA promotion is blocked (the slow symlink does NOT advance); verified by extending `tests/test_w14_dual_lora.c::test_kl_gate_blocks_promotion_above_tau` with a fixture subprocess mock returning `{"kl_nats": 0.8, "source": "real"}`.

- AC-12.3.4: GIVEN the real path fails mid-run (model load error, OOM), WHEN `_try_real_kl` raises an exception, THEN `compute_kl_drift.py` exits non-zero with a structured error JSON `{"ok": false, "reason": "<exception type>"}` (not a bare Python traceback); verified by `tests/test_compute_kl_drift.py::test_kl_model_load_error_exits_nonzero`.

- AC-12.3.5: GIVEN `torch` is NOT importable, WHEN `compute_kl_drift.py` runs, THEN the script emits `{"kl_nats": 0.0, "source": "stub"}` AND exits non-zero (not zero as in Sprint 11), so the C runner's existing stub-detection code triggers and the `lora_retrain_kl_gate_stubbed` event fires; verified by `tests/test_compute_kl_drift.py::test_stub_path_exits_nonzero`. This closes the silent-pass loophole while preserving backward compatibility with the C runner's stub-detection logic.

- AC-12.3.6: The location and format of the 200-prompt probe set is documented in a `--probe-set` validation error message that explains where to find or generate the file (per OQ-12.2 resolution); verified by `tests/test_compute_kl_drift.py::test_missing_probe_set_error_message_helpful`.

**Estimate:** M
**Priority:** P0 (Wave 0; entry condition; blocks US-12.4 and production W14 trust)
**Risk tier:** HIGH (changes production KL gate behavior; a bug here could block all EMA promotions or let high-drift adapters through; stub exit-code change is a C-runner behavioral change)
**Dependencies:** none (C runner's `out_is_stub` detection is already in place from Sprint 11 CRITICAL fix; this story flips the stub exit code and wires the real path)
**Test seam:** `tests/test_compute_kl_drift.py` with mock torch/transformers; `tests/test_w14_dual_lora.c` extended with real-KL subprocess fixture
Command: `python3 -m pytest tests/test_compute_kl_drift.py -v && cmake --build --preset dev && ./build/human_tests --filter=w14_dual_lora`
**Out of scope:** Implementing the probe set itself (that is OQ-12.2, resolved before tech-leads begin); per-layer KL analysis; using MLX-native KL (this story uses transformers-compatible inference; MLX path is a potential future optimization).

---

### US-12.4 (P0): As a researcher claiming measurable digital-twin lift, I want to run DPOP+DoRA training on Seth's real preference pairs and evaluate on the holdout with the real MLX YNTP gate, so that the sprint's publishable headline is grounded in empirical numbers rather than fixture simulations.

**Source:** Sprint goal "Prove the metric on real data"; convergence of FU-11.4.a + FU-11.6.b + FU-11.8.f; sprint-11/stories.md §sprint-success-metric.

**Rationale:** Sprints 7-11 built all the infrastructure: pad masking (US-11.1), DoRA (US-11.2), early stopping (US-11.3), DPOP loss (US-11.4), YNTP evaluator (US-11.6), 4-stage gate (US-11.7), dual fast/slow loop (US-11.8). Sprint 12's Wave 0 unlocks the real-inference paths (US-12.1/12.2/12.3). This story performs the actual training run and evaluation, commits the golden output to evidence, and produces the falsifiable comparison. If the result is negative (adapter hurts), that is still a valid publishable finding — see OQ-12.4.

**Acceptance criteria:**

- AC-12.4.1: GIVEN Wave 0 (US-12.1, US-12.2, US-12.3) has merged, WHEN a DPOP+DoRA training run is launched via `python3 scripts/finetune-gemma.py --dpo --variant dpop --train-type dora --early-stopping-signal chosen_r` against Seth's DPO preference pairs, THEN the training run completes (or early-stops per US-11.3), producing a `fast.safetensors` adapter artifact; the training log is committed to `sprints/sprint-12/evidence/US-12.4/train_log.jsonl`.

- AC-12.4.2: GIVEN the trained DPOP+DoRA adapter from AC-12.4.1, WHEN `HU_YNTP_HOLDOUT=~/.human/private/yntp_holdout_30.jsonl python3 scripts/yntp_eval.py --adapter <adapter_path>` runs via the real MLX path (US-12.2 AC-12.2.1 shipped), THEN the output JSON is committed to `sprints/sprint-12/evidence/US-12.4/yntp_result_dpop_dora.json` containing `"n_pairs": 30`, `"source": "real"`, finite `delta_ll`, and `pad_rate`.

- AC-12.4.3: GIVEN the baseline result from US-12.2 AC-12.2.5 (`baseline_yntp_result.json`) and the adapter result from AC-12.4.2, WHEN `scripts/pareto_picker.py --input-schema yntp` is run against both, THEN a machine-readable Pareto verdict (PROMOTE / DEFER / REJECT) is committed to `sprints/sprint-12/evidence/US-12.4/pareto_verdict.json`.

- AC-12.4.4 (adversarial regression guard — must survive a negative result): GIVEN the Sprint 8 broken adapter (iter-200, pad=80%), WHEN the same YNTP evaluation pipeline runs against it, THEN the gate produces `gate_decision: "FAIL"` and `pad_rate >= 0.5` (the US-11.6 AC-11.6.3 regression guard must hold end-to-end on the real inference path, not just on mock fixtures); verified by a real-machine smoke test that replays the Sprint 8 fixture log through the live `yntp_eval.py` real-path code.

- AC-12.4.5: GIVEN the committed evidence in `sprints/sprint-12/evidence/US-12.4/`, WHEN a human reads the three files (train_log, yntp_result, pareto_verdict), THEN the claim is self-contained and falsifiable: the evidence includes model variant, adapter rank, lambda, n training pairs, n holdout pairs, both mean log-likelihoods, delta, pad_rate, and the Pareto verdict with its thresholds.

- AC-12.4.6: GIVEN the real run produces any numeric outcome (positive or negative delta), WHEN the sprint closes, THEN a one-paragraph `sprints/sprint-12/evidence/US-12.4/interpretation.md` is committed stating the verdict honestly, including whether the result supports, falsifies, or is inconclusive relative to the Sprint 11 goal; see OQ-12.4 for the negative-result protocol.

**Estimate:** L
**Priority:** P0 (Wave 1; the sprint's capstone deliverable; requires US-12.1, US-12.2, US-12.3)
**Risk tier:** HIGH (real user data; PII policy per D1; the result may be negative; see OQ-12.4; this story cannot be mocked away — it is the proof)
**Dependencies:** US-12.1 (DPOP numerical correctness verified), US-12.2 (real MLX inference), US-12.3 (KL gate trusted)
**Test seam:** Real machine run on Seth's machine; committed evidence files; AC-12.4.4 regression guard on real inference path; no CI gate for the run itself (runs once on Seth's machine, evidence committed)
**Out of scope:** Hyperparameter sweep (single best-known config per Sprint 11 guidance: rank 16, lambda 0.1, DoRA, early-stopping); ORPO comparison run (US-12.5 must land first, and ORPO vs DPOP comparison is a stretch goal for US-12.5); Twin-2K-500 behavioral evaluation (D2 still binding per OQ-12.3).

---

### US-12.5 (P1): As a developer comparing preference optimization algorithms, I want `src/ml/rl_trainer_orpo.c`'s production `train_step` to run a real forward pass with tokenizer wiring so that `human ml rl-train --algorithm orpo` actually trains rather than returning NOT_SUPPORTED.

**Source:** FU-11.5.a — production ORPO `train_step` returns `HU_ERR_NOT_SUPPORTED`; symmetric with FU-7.10.a for SimPO. Sprint 11 US-11.5 wired the vtable and the loss formula but deferred the production execution path.

**Rationale:** Sprint 11 US-11.5 delivered an analytically correct ORPO loss (AC-11.5.2 golden fixture, `test_orpo_loss_golden`) and a vtable factory that exits 0. However `train_step` under `HU_IS_TEST` returns a mock and in production returns `HU_ERR_NOT_SUPPORTED` — the same gap that existed for SimPO until FU-7.10.a was closed. ORPO is the reference algorithm for single-stage SFT+preference (no reference model, anti-DCR by construction). Wiring the production path is a P1 because it enables the ORPO vs DPOP comparison in evidence — but it should not block the Wave 0/1 publishable claim.

**Acceptance criteria:**

- AC-12.5.1: GIVEN a production build (without `HU_IS_TEST`) and a real `hu_tokenizer_t` + `hu_provider_t` registered, WHEN `human ml rl-train --algorithm orpo --lambda-orpo 0.1` runs against the fixture dataset, THEN it exits 0 AND `train_step` returns `HU_OK` (not `HU_ERR_NOT_SUPPORTED`); verified by `tests/test_rl_trainer_orpo_production.c::test_orpo_production_train_step_returns_ok` with `HU_IS_TEST` absent and a mock provider seam.

- AC-12.5.2: GIVEN the production `train_step` is invoked with a batch where `logp > 0` (caller sign error), WHEN `orpo_compute_loss` is called, THEN it returns `HU_ERR_INVALID_ARGUMENT` (not a silent clamp to `-1e-12`); verified by `tests/test_rl_trainer_orpo_production.c::test_orpo_positive_logp_invalid_argument` (FU-11.5.b closes here).

- AC-12.5.3: GIVEN `--algorithm simpo` (Sprint 7 golden), WHEN `human ml rl-train` is invoked in a production build, THEN behavior is unchanged; no regression; verified by `tests/test_rl_trainer_simpo.c` passing without modification.

- AC-12.5.4: GIVEN `--algorithm grpo2`, WHEN `human ml rl-train` is invoked, THEN it still exits 2 with "not yet implemented"; the boundary is explicitly preserved.

- AC-12.5.5: All new C code compiles with `-Wall -Wextra -Wpedantic -Werror` and zero ASan errors under `cmake --preset dev`.

**Estimate:** M
**Priority:** P1 (Wave 2; enables ORPO vs DPOP comparison but does not block Wave 1)
**Risk tier:** MEDIUM (extends a C vtable; new production code path in `src/ml/rl_trainer_orpo.c`; tokenizer wiring touches cross-module interface)
**Dependencies:** US-12.1 (DPOP numerical correctness established; ORPO comparison is meaningful once baseline DPOP is proven)
**Test seam:** `tests/test_rl_trainer_orpo_production.c`; mock provider seam with `HU_IS_TEST` absent
Command: `cmake --build --preset dev && ./build/human_tests --filter=rl_trainer_orpo`
**Out of scope:** ORPO integration into `finetune-gemma.py` Python path; ORPO vs DPOP comparison run (stretch goal after this story lands); GRPO-2.

---

### US-12.6 (P1): As an operator running adapter rollback, I want `human ml adapter-rollback` to acquire a directory-level flock before modifying the slow adapter symlink so that concurrent invocations cannot race to undefined behavior.

**Source:** FU-11.8.h — two concurrent `human ml adapter-rollback` invocations race on `slow.safetensors.v{N}`; the second `rename` fails and the cross-FS fallback hits `fclose(NULL)` — undefined behavior. The W14 cron holds a PID lock but the rollback CLI is a separate process.

**Rationale:** This is a real concurrency bug with a clear undefined-behavior consequence (`fclose(NULL)`) that can be triggered before `dual_lora_enabled=true` ships to users. The fix is small (one `flock` call + one null-guard), the risk of NOT fixing it grows as soon as more than one terminal window can invoke `human ml adapter-rollback`. It is P1 (not P0) because the dual-LoRA loop is not yet in production (W14 cron still uses single-adapter mode); it must land before `dual_lora_enabled=true` is set.

**Acceptance criteria:**

- AC-12.6.1: GIVEN two concurrent `adapter-rollback` invocations targeting the same `slow_dir`, WHEN the first acquires the `flock` on `slow_dir` and holds it, THEN the second blocks until the first releases; verified by `tests/test_adapter_rollback_flock.c::test_concurrent_rollback_second_blocks` using `fork` + `pipe` synchronization under `HU_IS_TEST`.

- AC-12.6.2: GIVEN a rollback where the cross-FS copy target `in` is `NULL` (open failed), WHEN the copy code path is reached, THEN `HU_ERR_IO` is returned immediately without calling `fclose(NULL)`; verified by `tests/test_adapter_rollback_flock.c::test_null_in_handle_returns_io_error`.

- AC-12.6.3 (adversarial regression guard): GIVEN the pre-fix code path (flock absent, no null guard), a test that forces a concurrent second invocation MUST reliably trigger the race condition (exit non-zero or ASan violation) on the fixture — establishing that the test is not vacuously passing; verified by a negative-mode fixture in `tests/test_adapter_rollback_flock.c::test_regression_no_flock_races_without_guard`. This test must FAIL (detect the race) when the flock is intentionally removed from the implementation.

- AC-12.6.4: All new C code compiles with `-Wall -Wextra -Wpedantic -Werror` and zero ASan errors under `cmake --preset dev`.

**Estimate:** S
**Priority:** P1 (Wave 2; must land before `dual_lora_enabled=true`; small fix, high value)
**Risk tier:** MEDIUM (touches rollback CLI and symlink-management paths; incorrect locking could deadlock; the adversarial regression guard is essential to verify the fix is real)
**Dependencies:** none
**Test seam:** `tests/test_adapter_rollback_flock.c`; `fork` + `pipe` for concurrency testing; `HU_IS_TEST` subprocess mock on filesystem operations
Command: `cmake --build --preset dev && ./build/human_tests --filter=adapter_rollback_flock`
**Out of scope:** Cron-level PID lock (already exists in W14 scheduler); rollback undo (not requested); cross-host distributed locking.

---

### US-12.7 (P2): As a developer running Stage 2 coherence tests, I want the array-form env mock to correctly propagate `pad_rate` and the cascade execution order to be driven by a dispatch dict rather than implicit imperative sequencing, so that test-environment bugs cannot silently disable the pad gate.

**Source:** FU-11.7.d (array-form env mock silently sets `pads=[]`; pad gate silently disabled in array-form tests) + FU-11.7.e (`_CASCADE_ORDER` defense-in-depth; dispatch-dict execution).

**Rationale:** FU-11.7.d is a real test-infrastructure bug: any test using the array-form mock `HU_CASCADE_STAGE2_MOCK='[0.8, 0.8]'` gets `pad_rate=0.0` silently, meaning the pad gate never fires. Tests that should catch a pad-leaking adapter will pass vacuously. FU-11.7.e strengthens the execution order contract beyond the comment added in Sprint 11 — a dispatch dict makes reordering require a structural change. Both are P2: they do not block the publishable claim but they are real quality defects in the regression-guard infrastructure we rely on.

**Acceptance criteria:**

- AC-12.7.1: GIVEN `HU_CASCADE_STAGE2_MOCK='[0.8, 0.8, 0.8]'` (array form), WHEN `stage2_coherence.py` processes this mock, THEN it logs a deprecation warning to stderr and defaults `pads` to `[False, False, False]` (all false — no pads), NOT silently returns `pads=[]`; the behavior is explicitly documented; verified by `tests/test_pareto_gate.py::test_array_mock_logs_deprecation_and_defaults_pads`.

- AC-12.7.2: GIVEN `HU_CASCADE_STAGE2_MOCK='[0.8, 0.8, 0.8]'` where the user intends to test a pad-leaking adapter, WHEN `test_coherence_judge_rejects_pad_outputs` uses this form, THEN the test FAILS (demonstrating that the array form cannot test pad rejection), forcing test authors to use the dict form `{"scores": [0.8], "pads": [true]}`; verified by a new test `tests/test_pareto_gate.py::test_array_mock_cannot_simulate_pad_rejection`.

- AC-12.7.3: GIVEN the cascade stages, WHEN `stage_cascade.py` executes stages 1 through 4, THEN execution is driven by iterating a `STAGES` dispatch dict (not by implicit imperative order), so reordering requires editing the dict definition — any reordering causes AC-11.7.3 to fail; verified by `tests/test_pareto_gate.py::test_stages_executed_from_dispatch_dict` using `unittest.mock.patch` on each stage's `.run` method and asserting `call_count == 1` per stage in dict-iteration order (FU-11.7.e + FU-11.7.f upgrade: call counter, not JSON marker).

- AC-12.7.4: GIVEN `check-lora-ab.sh --cascade` (FU-11.7.g mktemp leak), WHEN the script runs and exits via the trap, THEN no orphaned tmp files remain in `/var/folders/`; verified by `tests/test_check_lora_ab_staged.sh::test_no_tmpfile_leak` counting files before and after a run.

**Estimate:** S
**Priority:** P2 (Wave 3; stretch; real bugs but does not block the publishable claim)
**Risk tier:** LOW (Python cascade scripts and shell test; no C changes; no vtable changes)
**Dependencies:** none
**Test seam:** `tests/test_pareto_gate.py`; `tests/test_check_lora_ab_staged.sh`
Command: `python3 -m pytest tests/test_pareto_gate.py -v && bash tests/test_check_lora_ab_staged.sh`
**Out of scope:** Stage 3 ThinkPRM real training (deferred); per-channel gates; GRPC-based stage execution.

---

### US-12.8 (P2): As a developer reading the scheduler status dashboard, I want `human doctor scheduler`'s `lora_retrain` block to be populated by actually calling `hu_w14_scheduler_status_save`, so that format changes or field renames are caught by the test rather than silently breaking the dashboard.

**Source:** FU-11.8.g — AC-11.8.5 status JSON test self-asserts: constructs the expected JSON manually and asserts `strstr` on itself, verifying nothing about the actual writer in `world_model_bridge.c`. A `%.4f` format change or field rename would silently break the writer while all tests pass.

**Rationale:** The status JSON is the operator's primary signal that the W14 dual-LoRA loop is healthy. If the writer and the test diverge, operators see a stale or corrupt status silently. The fix is straightforward: drive the test through `hu_w14_scheduler_status_save` with a populated context and assert field values in the output. This is P2 because it is a test quality fix — the writer works today — but it is load-bearing for any future field additions (Sprint 12 adds KL-gate fields in US-12.3).

**Acceptance criteria:**

- AC-12.8.1: GIVEN a populated `hu_lora_retrain_ctx_t` with `fast_version = 3`, `slow_version = 2`, `last_ema_alpha = 0.05f`, `last_gate_verdict = "PROMOTE"`, and `last_kl_drift_nats = 0.31f`, WHEN `hu_w14_scheduler_status_save(&ctx, path)` is called, THEN `strstr(output_json, "\"fast_version\":3")` AND `strstr(output_json, "\"last_kl_drift_nats\":0.3100")` both hold; verified by `tests/test_scheduler_status.c::test_status_save_drives_real_writer` (replacing the prior self-asserting test).

- AC-12.8.2 (adversarial regression guard): GIVEN the status writer uses `%.4f` for `last_kl_drift_nats`, WHEN the format string is changed to `%.2f` in the implementation, THEN the test FAILS; verified by a comment in the test citing this as the load-bearing precision contract. The test must be specific enough that a format-string change breaks it.

- AC-12.8.3: GIVEN `last_kl_drift_nats = -1.0f` (the Sprint 11 stub-detected sentinel from `lora_retrain_runner.c:696`), WHEN `hu_w14_scheduler_status_save` is called, THEN the output JSON contains `"last_kl_drift_nats":-1.0000` (not `0.0000`), and `human doctor scheduler` displays "KL gate: stubbed/unavailable" rather than "0.0000 nats (OK)"; verified by `tests/test_scheduler_status.c::test_status_save_kl_stub_sentinel`.

- AC-12.8.4: All new C code compiles with `-Wall -Wextra -Wpedantic -Werror` and zero ASan errors under `cmake --preset dev`.

**Estimate:** S
**Priority:** P2 (Wave 3; test quality fix; load-bearing once US-12.3 adds KL fields)
**Risk tier:** LOW (test replacement and display-string change in `human doctor`; no vtable changes; no behavioral change in production)
**Dependencies:** US-12.3 (the KL sentinel and real-KL fields are the new content this test must cover)
**Test seam:** `tests/test_scheduler_status.c`
Command: `cmake --build --preset dev && ./build/human_tests --filter=scheduler_status`
**Out of scope:** Full dashboard UI refresh; per-adapter status history; REST endpoint for status.

---

## Non-goals

- We will NOT implement ThinkPRM Stage 3 training (requires 5-10K persona labels from Seth; deferred).
- We will NOT lift D2 (Twin-2K-500 real 50-question labeling) this sprint; see OQ-12.3.
- We will NOT implement GRPO-2, XPO, EWC-LoRA, or any additional loss heads beyond the three already shipped.
- We will NOT automate chat.db extraction or grow the holdout beyond 30 rows this sprint; see OQ-12.1.
- We will NOT ship any M4/M6 user-facing features (100 DAU, channel focus) until the SOTA gate produces a PROMOTE or DEFER verdict.

---

## Open questions for stakeholder

### OQ-12.1 — Holdout size and statistical power

The 30-row holdout (`yntp_holdout_30.jsonl`) was chosen to match Sprint 8's smoke run. For a
publishable claim, 30 rows is borderline: the 95% confidence interval on `delta_nll` will be
wide. Two options:

A. Accept 30 rows this sprint; report CI alongside the point estimate; note the limitation.
B. Grow the holdout to 60-100 rows before the US-12.4 run; this requires Seth's time to
   curate additional PII-redacted pairs from chat.db.

**Recommendation:** Option A. Get the real number first; if the CI is unacceptably wide, Sprint 13
grows the holdout. Decision needed before US-12.2 AC-12.2.5 runs.

Also: Sprint 11 preferred Gemma-4-E2B. Is E4B being considered for Sprint 12? E4B has higher
baseline quality but longer inference time. Decision needed before the training run.

### OQ-12.2 — Probe set for KL drift (200-prompt set)

`compute_kl_drift.py` requires a `--probe-set` JSONL with 200 prompts. This probe set is not
in the repo. Two options:

A. Synthetic probe set: 200 generic conversational prompts, generated deterministically and
   committed to `tests/fixtures/kl_probe_200.jsonl`. No PII, runs in CI.
B. Real probe set: 200 PII-redacted prompts from Seth's chat.db. Stronger signal; requires
   Seth's manual curation step; must be git-ignored per D1.

**Recommendation:** Option A for this sprint. The KL gate's job is to detect catastrophic
distribution shift — synthetic prompts are sufficient for that. A real probe set is a Sprint 13
refinement. Decision needed before US-12.3 implementation begins.

### OQ-12.3 — Does Sprint 12 lift D2 (Twin-2K-500 real labeling)?

D2 deferred Seth's 50-question behavioral labeling to Sprint 12. D2 is still binding.
Does Seth want to invest 2-3 hours this sprint to label `twin2k_seth_50q.jsonl`?

**Options:**
A. Lift D2: add a US-12.X for the forced-choice behavioral evaluation with real Seth data.
B. Extend D2 deferral: Sprint 12 is depth-focused on the YNTP publishable claim; behavioral
   evaluation waits for Sprint 13.

**Recommendation:** Extend D2 deferral. The YNTP claim is the higher-priority publishable
result. If the YNTP result is positive, Twin-2K-500 is additive evidence. If negative, Twin-2K-500
may reveal whether behavioral alignment was also hurt. Either way, it's Sprint 13 work.

### OQ-12.4 — What if the real YNTP run produces a negative result (adapter hurts)?

US-12.4 AC-12.4.6 requires committing an honest interpretation. If `delta_ll < 0`, options:

A. Publish the negative result, pause Sprint 13, re-design (which loss variant? which rank?
   which training data quality?) before further runs.
B. Publish the negative result, run a rapid ORPO vs DPOP comparison (requires US-12.5 to land
   first), and treat Sprint 12 as an A/B experiment.
C. Consider the Sprint 11 infrastructure a success regardless of the metric direction; the
   metric being ungameable IS the contribution; publish as "infrastructure sprint with honest
   null result."

**Recommendation:** Decide Option A vs B before US-12.4 runs. If the result is negative, the
product-owner will surface this immediately so Seth can decide before any follow-on runs are
scheduled.

---

Last line: RESULT_product-owner=READY
