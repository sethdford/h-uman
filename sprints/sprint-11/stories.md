# Sprint 11 Backlog — SOTA Digital Twin

## Goal

Advance the digital-twin fine-tune loop from Sprint 8's "infrastructure works,
metric gameable, +0.019 delta with 40% pad-leakage" to a SOTA-defensible
held-out next-utterance prediction lift on the user's own data, with zero
pad-token leakage and a multi-dimensional Pareto promotion gate.

---

## Sprint success metric

The sprint closes when the following publishable claim can be made with
empirical evidence from the held-out evaluator (US-11.6):

> "On real user data (n=N conversations from Seth's chat.db), DPOP+DoRA+
> early-stopping on Gemma-4-E2B improves held-out next-utterance
> log-likelihood by X% vs. the base+system-prompt persona baseline, with
> 0 pad-token leakage in 30+ held-out prompts."

This claim is falsifiable, not gameable by lexical fingerprints, and
satisfies the SOTA-citation bar from the research synthesis.

Pareto gate thresholds (from `scripts/pareto_picker.py`, calibrated to
Sprint 8 evidence):

- PROMOTE: delta >= +0.03 AND pad_rate <= 10%
- DEFER:   delta >= +0.01 AND pad_rate <= 50%
- REJECT:  otherwise

Adapter promotion requires a PROMOTE verdict from the gate. An adapter
that achieves DEFER is tracked but not promoted to production.

---

## User Stories (priority order within wave)

---

### US-11.1 (P0): Enforce pad-token masking and length normalization in the DPO loss

**As a** developer running DPO fine-tuning on Gemma-4-E2B,
**I want** the training loop to mask pad tokens from the loss and normalize
log-probabilities by sequence length,
**so that** the optimizer cannot achieve a low loss by emitting short pad-token
sequences, eliminating the primary cause of our 40% pad-leakage failure rate.

**Source:** DPO research (sota-dpo.md §4; Meng et al. SimPO arXiv 2405.14734
§3 length normalization rationale) + MLX on-device research (sota-mlx-ondevice.md §2).

**Rationale:** Sprint 8 smoke run #3 showed 40% of outputs from the best DPO
checkpoint (iter 60) contained pad-token leakage. The root cause is that
un-normalized DPO assigns higher reward to shorter sequences, and pad-spam is
the cheapest local minimum. Both the DPO and MLX research agents independently
identified pad masking + length normalization as the mandatory prerequisite for
any other loss improvement. This is the cheapest, highest-impact fix in the
sprint and must land before DPOP (US-11.4) or ORPO (US-11.5) training begins.

**Acceptance criteria:**

- AC-11.1.1: GIVEN a DPO training run launched via `finetune-gemma.py --dpo`,
  WHEN the training loop processes a batch, THEN the loss computation calls the
  mlx-lm-lora API with padding tokens excluded from the log-probability sum
  (verified by asserting the per-token mask tensor has 0.0 at pad positions in
  a unit test that inspects the loss-computation call with a fixture batch
  containing known pad positions).

- AC-11.1.2: GIVEN a batch where the chosen sequence is 10 tokens and the
  rejected sequence is 50 tokens (simulating pad-inflation of the rejected),
  WHEN the length-normalized loss is computed, THEN the per-token
  log-probability of each sequence is divided by its non-pad token count before
  the margin is taken (verified numerically: normalized_loss == unnormalized_loss
  / nonpad_count within 1e-5 tolerance, in `tests/test_dpo_pad_masking.py`).

- AC-11.1.3: GIVEN a training run on the Sprint 8 fixture dataset (30-prompt
  held-out set), WHEN pad masking + length normalization are active, THEN
  the post-training evaluation produces a pad-leakage rate strictly less than
  the Sprint 8 best (12/30 = 40%); verified by running `scripts/pareto_picker.py`
  against the fixture sweep output and asserting the best checkpoint does not
  score REJECT solely due to pad rate.

- AC-11.1.4 (regression guard): GIVEN a training configuration identical to
  Sprint 8 iter-60 but WITHOUT pad masking (the Sprint 8 broken adapter),
  WHEN evaluated through `scripts/pareto_picker.py`, THEN the verdict is DEFER
  or REJECT (pad_rate >= 40%), demonstrating the old config still fails the gate
  and the new masking config is a genuine improvement. This is the "Sprint 8
  broken adapter must FAIL this gate" regression guard.

- AC-11.1.5: GIVEN `HU_IS_TEST` is defined, WHEN the test suite runs,
  THEN no real model weights are loaded; all NLL computations use the
  `hu_ml_nll_compute_fn_t` mock seam registered in test setup.

**Estimate:** S
**Priority:** P0 (Wave 0)
**Dependencies:** none
**Risk tier:** LOW (scripts and training configuration only; no C vtable changes)
**Test seam:**
  - `tests/test_dpo_pad_masking.py` — fixture batch with known pad positions;
    numerical assertion on loss tensor; mock `mlx.core` loss call
  - `scripts/pareto_picker.py` against fixture sweep JSON to verify regression guard
  - Command: `python3 -m pytest tests/test_dpo_pad_masking.py -v`
**Out of scope:** Changes to `src/ml/` C code; DPOP or ORPO loss heads
  (US-11.4/11.5); changes to the inference-time padding behavior.

---

### US-11.2 (P0): Add DoRA training mode to finetune-gemma.py

**As a** developer optimizing the persona adapter,
**I want** to train with DoRA (Weight-Decomposed Low-Rank Adaptation) instead
of LoRA by passing `--train-type dora`,
**so that** the adapter closes approximately 93% of the LoRA-to-full-fine-tune
quality gap at the same rank, with no inference overhead after merge-back.

**Source:** NVIDIA Liu et al. 2025 DoRA paper, NVIDIA developer blog 2025;
MLX on-device research (sota-mlx-ondevice.md §2 DoRA section).

**Rationale:** DoRA adds weight-magnitude decomposition to LoRA's
direction-only update, adding only 2-3% more trainable parameters but closing
93% of the remaining quality gap to full fine-tuning. It incurs no inference
overhead because the magnitude and direction components merge back identically
to LoRA. `mlx-lm-lora` supports `--train-type dora` natively. This is a
drop-in replacement for the current LoRA training flag and is P0 because every
downstream training story (US-11.4, US-11.5, US-11.8) should train with DoRA
as the default.

**Acceptance criteria:**

- AC-11.2.1: GIVEN `finetune-gemma.py` is called with `--train-type dora`,
  WHEN the training subprocess is constructed, THEN the mlx-lm-lora CLI
  invocation contains `--train-type dora` (not `--train-type lora`); verified
  by `tests/test_finetune_gemma_dora.py::test_dora_flag_propagated_to_mlx_cmd`
  using `subprocess.run` mock asserting the argv shape.

- AC-11.2.2: GIVEN `--train-type` is not specified (default), WHEN
  `finetune-gemma.py` runs, THEN the default is `dora` (upgrading the prior
  default of `lora`); verified by
  `tests/test_finetune_gemma_dora.py::test_default_train_type_is_dora`.

- AC-11.2.3: GIVEN `--train-type lora` is explicitly passed, WHEN the script
  runs, THEN the CLI invocation contains `--train-type lora` and DoRA is NOT
  used; verified by
  `tests/test_finetune_gemma_dora.py::test_explicit_lora_flag_respected`.

- AC-11.2.4: GIVEN a DoRA training run completes on the fixture dataset,
  WHEN `scripts/check-lora-baseline.sh` is run against the resulting adapter,
  THEN the script exits 0 (adapter is structurally valid and loadable);
  verified by the baseline gate in CI.

- AC-11.2.5: GIVEN the `train_config.json` written by the versioning function,
  WHEN it is inspected, THEN the `train_type` field records `"dora"` when DoRA
  was used; verified by
  `tests/test_finetune_gemma_dora.py::test_train_config_records_dora`.

**Estimate:** S
**Priority:** P0 (Wave 0)
**Dependencies:** none
**Risk tier:** LOW (scripts only; no C changes)
**Test seam:**
  - `tests/test_finetune_gemma_dora.py` — mock `subprocess.run`; assert argv;
    assert `train_config.json` field
  - `scripts/check-lora-baseline.sh` in CI against DoRA fixture adapter
  - Command: `python3 -m pytest tests/test_finetune_gemma_dora.py -v`
**Out of scope:** DoRA support in the C `src/ml/` training path; inference-time
  DoRA merge (handled by mlx-lm-lora automatically); per-module DoRA vs LoRA
  mixing.

---

### US-11.3 (P0): Persona-vector projection as an early-stopping signal

**As a** developer running DPO fine-tuning,
**I want** the training loop to project intermediate model activations onto a
pre-computed "user-voice" persona direction and stop training when that
projection drifts beyond a threshold,
**so that** training halts before the model's learned voice diverges from the
user's actual voice, replacing the Sprint 8 val_accuracy signal that was shown
to be non-predictive.

**Source:** Anthropic Chen et al. arXiv 2507.21509 (Persona Vectors); also
convergent with Sprint 8 insights-addressed.md Insight 2 (chosen_r plateau-break
Rule A/B).

**Rationale:** Sprint 8 smoke run #3 and insights-addressed.md proved that
val_accuracy is not predictive of generation quality: val_acc peaked at iter
20/40 (89.8%) but generation quality peaked at iter 60, and collapse began at
iter 65 when train_chosen_r dropped 56%. Anthropic's persona-vector paper
(2507.21509) provides a theoretically grounded early-stopping signal:
project model activations onto the user-voice direction; stop when the
projection drifts. This converges with our empirical Rule A/B finding. The
implementation is a post-step callback in the DPO training loop that reads
train_chosen_r from the training log and fires the plateau-break rule. No
actual activation-projection infrastructure is required in Wave 0 — the
`chosen_r` signal from the training output is a valid proxy for the persona
vector projection (both capture the same underlying divergence).

**Acceptance criteria:**

- AC-11.3.1: GIVEN a DPO training run with `--early-stopping-signal chosen_r`,
  WHEN `train_chosen_r` drops below 50% of its trailing-5-window mean for two
  consecutive evaluation steps, THEN the training loop sets `should_stop=True`
  and saves the adapter from the prior window as the final artifact; verified by
  `tests/test_early_stopping.py::test_chosen_r_plateau_break_fires` with a
  fixture log containing the known Sprint 8 trajectory (plateau at iter 35-60,
  cliff at iter 65).

- AC-11.3.2: GIVEN the Sprint 8 iter 60-80 training log as a fixture, WHEN the
  early-stopping callback processes it, THEN it would have halted at iter 65
  (the cliff iteration), saving the iter-60 checkpoint as the promoted artifact;
  verified by replaying the fixture log through the callback in
  `tests/test_early_stopping.py::test_sprint8_trajectory_stops_at_iter65`.

- AC-11.3.3: GIVEN a training run where `train_chosen_r` stays within 20% of
  its plateau mean throughout all iterations, WHEN the callback processes the
  run, THEN no early stopping fires and training runs to the configured
  `--iters` limit; verified by
  `tests/test_early_stopping.py::test_stable_chosen_r_no_early_stop`.

- AC-11.3.4: GIVEN the early-stopping callback fires, WHEN the training log is
  inspected, THEN a structured log line is emitted containing `"early_stop"`,
  `"reason": "chosen_r_plateau_break"`, `"stopped_iter"`, and
  `"promoted_iter"` fields; verified by
  `tests/test_early_stopping.py::test_early_stop_log_format`.

- AC-11.3.5: GIVEN `--early-stopping-signal none` (disabled), WHEN the
  training loop runs, THEN the plateau-break callback is never invoked and
  training runs to `--iters`; backward-compatible with existing CI scripts.

**Estimate:** M
**Priority:** P0 (Wave 0)
**Dependencies:** none (can develop against Sprint 8 fixture log independently)
**Risk tier:** MEDIUM (modifies training loop callback; fixture-grounded but
  touches live training code path)
**Test seam:**
  - `tests/test_early_stopping.py` with Sprint 8 trajectory as a JSONL fixture
    in `tests/fixtures/sprint8_dpo80_log.jsonl`
  - No real model weights; all training-log values are fixture-driven
  - Command: `python3 -m pytest tests/test_early_stopping.py -v`
**Out of scope:** Full Anthropic activation-projection infrastructure (requires
  reading intermediate layer activations, deferred to Sprint 12); EWC-LoRA
  regularization (US-11.8); per-layer stopping signals.

---

### US-11.4 (P0): DPOP loss head (Smaug positive-clipping)

**As a** developer fine-tuning the digital-twin adapter,
**I want** the `finetune-gemma.py --dpo --variant dpop` path to apply the
DPOP positive-clipping penalty that anchors chosen log-probability above the
reference model's chosen log-probability,
**so that** the Degraded Chosen Responses (DCR) failure mode that Sprint 8
observed (chosen_r going negative, pad-token collapse) is structurally
prevented rather than caught after the fact by early stopping.

**Source:** Pal et al. Smaug arXiv 2402.13228 (DPO-Positive); convergent
finding from both the DPO agent and the personalization agent in the SOTA
roadmap.

**Rationale:** The SOTA roadmap identifies DPOP as the single strongest
recommendation, surfaced independently by two research agents. DPOP adds one
penalty term: `lambda * max(0, log pi_ref(y_chosen) - log pi(y_chosen))`. This
explicitly prevents the optimizer from reducing chosen log-probability, which
is the root cause of our chosen_r sign-flip and pad-token cascade. Sprint 8
smoke-run #3 demonstrated exactly this failure at iter 80 (chosen_r = -8.867).
DPOP is a 3-line loss change on top of existing DPO infrastructure and should
be the first loss variant tried after pad masking (US-11.1) is in place.

**Acceptance criteria:**

- AC-11.4.1: GIVEN `finetune-gemma.py --dpo --variant dpop --dpop-lambda 0.1`,
  WHEN the training subprocess is constructed, THEN the CLI contains `--variant
  dpop` and `--dpop-lambda 0.1` passed to mlx-lm-lora; verified by
  `tests/test_finetune_gemma_dpop.py::test_dpop_flag_propagated` using
  `subprocess.run` mock.

- AC-11.4.2: GIVEN the DPOP loss function with a fixture batch where
  `log_pi_ref(y_chosen) > log_pi(y_chosen)` (the DCR failure condition),
  WHEN `compute_dpop_loss` is called, THEN the returned loss is strictly
  greater than the corresponding vanilla DPO loss (the positive-clipping
  penalty adds a positive penalty term); verified numerically in
  `tests/test_dpop_loss.py::test_dpop_penalty_fires_on_dcr_condition`
  within 1e-5 tolerance.

- AC-11.4.3: GIVEN the DPOP loss function with a fixture batch where
  `log_pi(y_chosen) >= log_pi_ref(y_chosen)` (the non-DCR case),
  WHEN `compute_dpop_loss` is called, THEN the penalty term is exactly 0 and
  the loss equals vanilla DPO loss; verified numerically in
  `tests/test_dpop_loss.py::test_dpop_penalty_zero_on_healthy_chosen`.

- AC-11.4.4 (regression guard): GIVEN the Sprint 8 iter-80 training scenario
  (chosen_r went to -8.867 under vanilla DPO), WHEN the same training
  configuration runs with `--variant dpop`, THEN `train_chosen_r` must NOT
  drop below 0 at any sampled checkpoint in the fixture simulation; verified by
  `tests/test_dpop_loss.py::test_sprint8_iter80_dcr_prevented_by_dpop` with
  a synthetic fixture that replays the Sprint 8 gradient direction.

- AC-11.4.5: GIVEN `--variant dpo` (vanilla DPO, the prior default), WHEN the
  script runs, THEN behavior is identical to pre-story DPO behavior; no
  regression; verified by existing `tests/test_finetune_gemma_dpo.py` tests
  passing without modification.

**Estimate:** M
**Priority:** P0 (Wave 1; requires US-11.1 pad masking to be meaningful)
**Dependencies:** US-11.1 (pad masking must be active to isolate the DPOP
  improvement from the pad-leakage noise)
**Risk tier:** MEDIUM (new loss variant in the Python training pipeline;
  touches loss computation path)
**Test seam:**
  - `tests/test_dpop_loss.py` — analytical fixture batches; numerical loss
    assertions; Sprint 8 regression guard
  - `tests/test_finetune_gemma_dpop.py` — mock subprocess; argv assertion
  - No real model weights; all log-probability values are fixture inputs
  - Command: `python3 -m pytest tests/test_dpop_loss.py tests/test_finetune_gemma_dpop.py -v`
**Out of scope:** ORPO loss head (US-11.5); BPO one-line variant; Cal-DPO;
  the C-layer `hu_rl_trainer_t` DPOP factory (deferred to Sprint 12 after
  Python path is validated).

---

### US-11.5 (P0): Wire ORPO train_step (finish Sprint 7 US-7.10 stub)

**As a** developer comparing preference optimization algorithms,
**I want** `finetune-gemma.py --variant orpo` to execute a real ORPO training
step (odds-ratio penalty fused with SFT NLL on chosen, no reference model),
**so that** I can run a single-stage SFT+preference pass and compare it against
DPOP on the Sprint 8 fixture dataset, using the `hu_rl_trainer_t` vtable
already defined in Sprint 7.

**Source:** Hong et al. ORPO arXiv 2403.07691 (EMNLP 2024/ICLR 2025);
Sprint 7 US-7.10 delivered the vtable but left ORPO and GRPO-2 as
"not yet implemented" stubs (AC-7.10.5).

**Rationale:** Sprint 7 US-7.10 delivered the `hu_rl_trainer_t` vtable with
a SimPO loss head. The ORPO stub (`--algorithm orpo` exits 2 with "not yet
implemented") was explicitly deferred. ORPO is the primary recommendation from
the DPO research agent for small-data persona transfer: the SFT NLL term
directly prevents DCR collapse by construction, and the absence of a reference
model reduces memory pressure. AC-7.10.5 established the stub; this story
wires the real train_step. The vtable interface (Sprint 7 US-7.10) is already
stable.

**Acceptance criteria:**

- AC-11.5.1: GIVEN `human ml rl-train --algorithm orpo --lambda-orpo 0.1`
  with the fixture dataset, WHEN the command runs, THEN it exits 0 (not exit
  code 2 "not yet implemented"), and the `hu_rl_trainer_orpo_t` factory is
  selected; verified by `tests/test_rl_trainer_orpo.c::test_orpo_train_exits_0`
  with `HU_IS_TEST` guards on file writes.

- AC-11.5.2: GIVEN a fixture batch `{prompt, chosen}` (no `rejected` field
  required by ORPO), WHEN `compute_loss` is called on the ORPO trainer,
  THEN the returned loss equals `NLL(chosen) + lambda * OR_penalty(chosen)`
  within 1e-4 absolute tolerance, where `NLL` is the standard cross-entropy
  loss and `OR_penalty` is the odds-ratio term from Hong et al. Eq. 7;
  verified analytically in `tests/test_rl_trainer_orpo.c::test_orpo_loss_golden`.

- AC-11.5.3: GIVEN the ORPO loss is computed on a batch where `log_pi(chosen)`
  is already high (model has learned the preference), WHEN the OR penalty is
  computed, THEN the penalty term approaches 0 (the SFT term dominates),
  preventing continued chosen-log-prob degradation; verified in
  `tests/test_rl_trainer_orpo.c::test_orpo_or_penalty_diminishes_at_high_log_prob`.

- AC-11.5.4: GIVEN `--algorithm simpo` (Sprint 7 US-7.10 golden factory),
  WHEN `human ml rl-train` is invoked, THEN behavior is identical to Sprint 7
  baseline (no regression); verified by `tests/test_rl_trainer_simpo.c`
  passing without modification.

- AC-11.5.5: GIVEN `--algorithm grpo2`, WHEN `human ml rl-train` is invoked,
  THEN it still exits 2 with "not yet implemented" (explicitly keeping the
  boundary clean); verified by existing `tests/test_ml_cli_rl_train.c::test_rl_train_unimplemented_algorithms`.

- AC-11.5.6: All new C code compiles with `-Wall -Wextra -Wpedantic -Werror`
  and zero ASan errors under `cmake --preset dev`.

**Estimate:** M
**Priority:** P0 (Wave 1; requires US-11.1 for meaningful comparison)
**Dependencies:** US-11.1 (ORPO training should use pad-masked NLL by default);
  Sprint 7 US-7.10 vtable (already shipped)
**Risk tier:** MEDIUM (new C factory in `src/ml/rl_trainer.c`; extends existing
  vtable; new `hu_rl_trainer_type_t` enum value active)
**Test seam:**
  - `tests/test_rl_trainer_orpo.c` — analytical golden fixture; NLL + OR
    penalty numerical assertions; `HU_IS_TEST` guards
  - `tests/test_ml_cli_rl_train.c` — extended; exit-code assertions
  - Command: `cmake --build --preset dev && ./build/human_tests --filter=rl_trainer_orpo`
**Out of scope:** GRPO-2 implementation; integration of ORPO into
  `finetune-gemma.py` Python path (only the C vtable is wired here; Python
  integration is deferred to Sprint 12); ORPO A/B comparison against DPOP
  (that requires US-11.6 evaluator).

---

### US-11.6 (P1): Held-out next-utterance log-likelihood evaluator (YNTP-100 protocol on chat.db)

**As a** developer claiming measurable digital-twin lift,
**I want** a `human ml twin-eval --protocol yntp` command that computes
held-out next-utterance log-likelihood on real Seth chat.db conversations,
comparing the persona-adapted model against the base+system-prompt baseline,
**so that** the sprint's headline improvement claim is backed by a metric that
cannot be gamed by lexical fingerprints and is grounded in real user data.

**Source:** TwinVoice arXiv 2510.25536 (ACL 2026 Findings) + YNTP-100
arXiv 2510.14398; convergent finding from both the benchmarks agent and the
reward-eval agent.

**Rationale:** Sprint 8 smoke-run #3 and insights-addressed.md §insight-3
proved that the synthetic lexical fingerprint rewarded broken (pad-token) output
higher than coherent output. The SOTA benchmarks research confirms: the only
ungameable metric for digital-twin evaluation is held-out next-utterance
log-likelihood on the actual user's own data. This story implements the YNTP-100
protocol adapted to Seth's chat.db: hold out the last N utterances per
conversation, compute mean negative log-likelihood of the real utterance under
each model, and report the delta. The Sprint 8 broken adapter (iter 200,
+0.046 lexical with 80% pad leakage) MUST fail this gate, establishing that
the metric is not gameable.

**Acceptance criteria:**

- AC-11.6.1: GIVEN `human ml twin-eval --protocol yntp --holdout-file
  tests/fixtures/yntp_holdout_30.jsonl --adapter <adapter_path>`,
  WHEN the command runs, THEN it outputs a JSON object with fields
  `{"base_nll": float, "adapter_nll": float, "delta_nll": float,
  "n_prompts": int, "pad_rate": float}` where `delta_nll` is
  `base_nll - adapter_nll` (positive = improvement); verified by
  `tests/test_twin_eval_yntp.c::test_yntp_output_schema` with a mock
  `hu_ml_nll_compute_fn_t` seam.

- AC-11.6.2: GIVEN a fixture adapter that returns lower NLL than base on all
  holdout prompts, WHEN `twin-eval` runs, THEN `delta_nll > 0` and
  `pad_rate == 0.0`; verified by
  `tests/test_twin_eval_yntp.c::test_positive_delta_on_fixture_good_adapter`.

- AC-11.6.3 (regression guard): GIVEN a fixture that simulates the Sprint 8
  iter-200 broken adapter (high NLL on coherent continuations, many pad tokens),
  WHEN `twin-eval` runs, THEN `delta_nll <= 0` OR `pad_rate >= 0.5`, confirming
  the lexical fingerprint's false positive cannot recur on this metric; verified
  by `tests/test_twin_eval_yntp.c::test_sprint8_broken_adapter_fails_yntp`.
  This is the "Sprint 8 broken adapter must FAIL this gate" regression guard.

- AC-11.6.4: GIVEN `HU_IS_TEST` is defined, WHEN `twin-eval` runs, THEN no
  real model weights are loaded; all NLL values are injected via the
  `hu_ml_nll_compute_fn_t` mock seam (same seam pattern as US-7.6); verified
  by asserting the real model-load path is never reached.

- AC-11.6.5: GIVEN the holdout fixture with 30 prompts matching the Sprint 8
  smoke-run format, WHEN the evaluator runs against two fixture adapters (one
  "good" and one "broken"), THEN the output JSON is machine-parseable by
  `scripts/pareto_picker.py` as the `fidelity_delta` and `pad_failure_rate`
  inputs; verified by a round-trip test in `tests/test_twin_eval_integration.sh`.

- AC-11.6.6: The holdout fixture `tests/fixtures/yntp_holdout_30.jsonl`
  contains >= 30 real-format (PII-scrubbed, no contact names/emails)
  `{prompt, continuation}` pairs derived from the YNTP-100 protocol structure;
  verified by a fixture-validation test asserting field presence and
  PII-scrub patterns.

**Estimate:** L
**Priority:** P1 (Wave 1; depends on Wave 0 infrastructure being stable)
**Dependencies:** US-11.1 (pad masking in training must be in place before the
  evaluation's pad_rate metric is meaningful); US-11.3 (early stopping ensures
  the adapter being evaluated is the plateau-window checkpoint, not the
  collapsed checkpoint)
**Risk tier:** HIGH (new C evaluator; new fixture with user-data-derived
  content; primary sprint metric; determines the publishable claim)
**Test seam:**
  - `tests/test_twin_eval_yntp.c` with `hu_ml_nll_compute_fn_t` mock seam
  - `tests/fixtures/yntp_holdout_30.jsonl` (30 PII-scrubbed prompt+continuation pairs)
  - `tests/test_twin_eval_integration.sh` — round-trip to `pareto_picker.py`
  - Command: `cmake --build --preset dev && ./build/human_tests --filter=twin_eval`
**Out of scope:** Actual chat.db extraction automation (requires Seth's live
  machine; fixture is hand-curated or script-generated offline); multi-user
  YNTP-100 scale (n=100 users; we target n=1 user, Seth, with N conversations);
  Twin-2K-500 forced-choice secondary metric (US-11.10).

---

### US-11.7 (P1): 4-stage Pareto gate (PPL floor, coherence, PRM, ensemble)

**As a** developer promoting an adapter to production,
**I want** adapter promotion to require passing all four stages of the Pareto
gate (PPL floor, coherence judge, persona PRM, ensemble aggregation) before
the W14 cron advances the `current` symlink,
**so that** a pad-token-broken or lexically-gameable adapter can never reach
production regardless of its raw delta score.

**Source:** Reward-eval research (sota-reward-eval.md §2, recommended cascade
architecture); ThinkPRM arXiv 2504.16828; Coste et al. arXiv 2310.02743
(ensemble de-biasing).

**Rationale:** The reward-eval research identified the catastrophic Goodhart
failure we observed in Sprint 8: a single lexical proxy metric rewarded broken
output. The recommended fix is a four-stage cascade where each stage is cheaper
than the next and gates it. Stage 1 (PPL floor) is free and deterministic;
Stage 2 (coherence judge) is local and cheap; Stage 3 (persona PRM) is the
only stage that can be gamed; Stage 4 (ensemble) caps damage from any single
judge. This story implements stages 1-2 and the ensemble aggregation framework
with stubs for stage 3, integrating with the existing `pareto_picker.py`.
Stage 3 (full ThinkPRM training) is explicitly deferred to Sprint 12 as it
requires Seth's manual labeling effort.

**Acceptance criteria:**

- AC-11.7.1: GIVEN an adapter whose held-out PPL is greater than 3 times the
  base model's PPL on the persona dev set, WHEN the 4-stage gate runs,
  THEN the gate exits with code 2 (REJECT) at Stage 1 without proceeding to
  Stage 2; verified by `tests/test_pareto_gate.py::test_ppl_floor_rejects_high_ppl`
  with a fixture where adapter_ppl = 4 * base_ppl.

- AC-11.7.2: GIVEN an adapter that passes the PPL floor (Stage 1) but produces
  pad-token-containing outputs on >= 50% of Stage 2 coherence prompts,
  WHEN the coherence judge runs, THEN the gate exits with code 2 (REJECT)
  at Stage 2; verified by `tests/test_pareto_gate.py::test_coherence_judge_rejects_pad_outputs`.

- AC-11.7.3 (regression guard): GIVEN the Sprint 8 iter-200 adapter scenario
  (pad_rate = 80%, delta = +0.046 on lexical fingerprint), WHEN the 4-stage
  gate processes it, THEN the gate REJECTS at Stage 1 or Stage 2 (PPL floor
  or coherence), despite the positive lexical delta; verified by
  `tests/test_pareto_gate.py::test_sprint8_iter200_rejected_by_gate`.

- AC-11.7.4: GIVEN an adapter that passes Stages 1-2, WHEN Stage 3 is
  invoked, THEN the stub returns a configurable fixture score (enabling
  testing of Stage 4 logic without a trained PRM); the stub is gated by
  `--stage3-stub` CLI flag, and the real PRM path returns NOT_IMPLEMENTED
  with a clear log message; verified by
  `tests/test_pareto_gate.py::test_stage3_stub_configurable`.

- AC-11.7.5: GIVEN 3 orthogonal judge scores (lexical, coherence, NLL-based)
  at Stage 4, WHEN ensemble aggregation runs with min-aggregation, THEN the
  final verdict is no better than the worst individual judge's verdict;
  verified by `tests/test_pareto_gate.py::test_ensemble_min_aggregation` with
  fixture scores where one judge is DEFER and two are PROMOTE.

- AC-11.7.6: The 4-stage gate is integrated into `scripts/check-lora-ab.sh`
  as a `--staged-gate` flag; when passed, the script emits a per-stage
  breakdown JSON in addition to the existing Pareto score; verified by
  `tests/test_check_lora_ab_staged.sh`.

**Estimate:** L
**Priority:** P1 (Wave 2; depends on US-11.6 providing the NLL-based Stage 1 input)
**Dependencies:** US-11.6 (NLL evaluator provides the PPL floor input for
  Stage 1); US-11.1 (pad masking ensures Stage 2 coherence is testing the
  right failure mode)
**Risk tier:** HIGH (gating logic that controls adapter promotion; incorrect
  implementation could block all promotions or allow broken adapters through)
**Test seam:**
  - `tests/test_pareto_gate.py` — fixture-driven; each stage independently
    testable; Sprint 8 regression guard fixture
  - `tests/test_check_lora_ab_staged.sh` — integration test of CLI flag
  - Command: `python3 -m pytest tests/test_pareto_gate.py -v`
**Out of scope:** ThinkPRM Stage 3 training (requires 5-10K persona labels
  from Seth's manual effort; deferred to Sprint 12); RewardHackWatch runtime
  detector integration (stretch goal); per-channel Pareto gates (deferred to
  MoLoRA phase).

---

### US-11.8 (P1): Dual fast/slow LoRA with EMA promotion for W14 cron

**As a** user whose assistant learns from my corrections nightly,
**I want** the W14 nightly re-train cron to maintain a fast adapter (trained
on tonight's batch) and a slow adapter (EMA-promoted from prior nights), with
the slow adapter advanced only when the Pareto gate passes,
**so that** a single bad night of corrections cannot overwrite weeks of
accumulated learning, and rollback is always safe.

**Source:** OFS-DPO / COFS-DPO arXiv 2406.05534 (Online Fast-Slow Chasing DPO);
continual learning research (sota-continual.md §2, §3).

**Rationale:** The W14 cron (Sprint 7 US-7.5) currently trains a single adapter
from scratch each night, erasing prior learning. OFS-DPO's dual-adapter design
prevents this: the fast adapter trains on tonight's preference batch (high
learning rate, small rank); the slow adapter is the EMA of all prior slow
checkpoints; on PASS, slow := EMA(slow, fast, alpha=0.95). On FAIL, the fast
adapter is quarantined and the slow adapter is unchanged. This is a structural
improvement to the continuous-learning loop independent of which loss function
is used (it composes with DPOP from US-11.4).

**Acceptance criteria:**

- AC-11.8.1: GIVEN the W14 idle scheduler fires and new correction pairs are
  available, WHEN the nightly retrain runs, THEN it produces two artifacts:
  `fast.safetensors` (tonight's batch only) and `slow.safetensors.v{N}`
  (the current promoted slow adapter); verified by
  `tests/test_w14_dual_lora.c::test_dual_adapter_artifacts_created` with
  `HU_IS_TEST` subprocess mock.

- AC-11.8.2: GIVEN the Pareto gate (from US-11.7 `scripts/check-lora-ab.sh
  --staged-gate`) returns PROMOTE for tonight's fast adapter, WHEN the EMA
  update runs, THEN `slow.safetensors` is updated as
  `slow = 0.95 * slow + 0.05 * fast` and a new `v{N+1}` versioned copy is
  saved; the `current` symlink advances to `slow.safetensors.v{N+1}`;
  verified by `tests/test_w14_dual_lora.c::test_ema_update_on_promote`.

- AC-11.8.3: GIVEN the Pareto gate returns REJECT for tonight's fast adapter,
  WHEN the cron observes the failure, THEN `fast.safetensors` is moved to
  `quarantine/{date}.safetensors`, the slow adapter is NOT updated, and a
  `nightly_retrain_rejected` event is emitted with the gate verdict;
  verified by `tests/test_w14_dual_lora.c::test_quarantine_on_reject`.

- AC-11.8.4: GIVEN the dual-LoRA cron has run for at least 3 nights (simulated
  via fixture), WHEN `human adapter rollback` is called, THEN the `current`
  symlink is moved back to `slow.safetensors.v{N-1}` and the current night's
  version is preserved as `quarantine/{date}.safetensors`; verified by
  `tests/test_w14_dual_lora.c::test_adapter_rollback_cli`.

- AC-11.8.5: The `~/.human/scheduler.status` JSON `lora_retrain` block gains
  `fast_version`, `slow_version`, `last_ema_alpha`, and `last_gate_verdict`
  fields; `human doctor scheduler` parses and displays them; verified by
  extending `tests/test_scheduler_status.c`.

**Estimate:** L
**Priority:** P1 (Wave 2; depends on US-11.7 Pareto gate for the promotion decision)
**Dependencies:** US-11.7 (Pareto gate must exist before the dual-LoRA loop can
  gate on it); US-11.4 (DPOP loss should be the default for fast adapter training)
**Risk tier:** MEDIUM (touches W14 scheduler invocation path and adapter symlink
  management; the EMA update is Python-side but the symlink and status JSON are C-side)
**Test seam:**
  - `tests/test_w14_dual_lora.c` with `HU_IS_TEST` subprocess mock; fixture
    metadata files for gate simulation
  - `tests/test_scheduler_status.c` extended with new JSON fields
  - Command: `cmake --build --preset dev && ./build/human_tests --filter=w14_dual_lora`
**Out of scope:** EWC-LoRA Fisher regularization (deferred; requires
  Fisher diagonal computation which is expensive at E2B scale);
  SuRe surprise-priority replay sampler (deferred to Sprint 12); per-user
  adapter management (single-user scope for Sprint 11).

---

### US-11.9 (P2): POPI summarizer baseline (cloud-provider personalization)

**As a** developer comparing personalization approaches,
**I want** a `human ml popi-summarize` command that compresses Seth's last
N correction pairs and conversation examples into a <= 100-token preference
summary injected into the system prompt,
**so that** I can compare POPI-style in-context personalization against the
LoRA+DPOP adapter approach on the YNTP-100 evaluator, establishing a cloud-
provider-compatible baseline that does not require fine-tuning.

**Source:** POPI arXiv 2510.17881 (Personalizing LLMs via Optimized Natural
Language Preference Inference); personalization research (sota-personalization.md §2).

**Rationale:** POPI is the Plan B for the M3 bridge problem: if LoRA
personalization degrades on a given cloud provider (the Sprint 7 US-7.3
honesty gate scenario), a system-prompt summary of the user's preferences can
match LoRA fidelity with no weight updates. This story implements the
summarizer as a simple extraction prompt over the last 50 correction pairs,
producing a <= 100-token summary that is inserted into the system prompt.
No RL-trained inference model (the full POPI architecture) is required at
this stage; the baseline is a zero-shot compression of the correction history.
The comparison against the LoRA adapter (via US-11.6) will determine whether
POPI or LoRA is the stronger approach at our data scale.

**Acceptance criteria:**

- AC-11.9.1: GIVEN `human ml popi-summarize --corrections-db dpo_pairs.db
  --max-pairs 50 --max-tokens 100`, WHEN the command runs, THEN it produces
  a plain-text preference summary of <= 100 tokens that captures at least 3
  distinct style preferences evidenced in the corrections; verified by
  `tests/test_popi_summarize.py::test_summary_under_token_limit` counting
  whitespace-split tokens.

- AC-11.9.2: GIVEN the POPI summary is injected into the system prompt alongside
  the existing persona context, WHEN `human ml twin-eval --protocol yntp
  --baseline popi` runs, THEN the JSON output includes `popi_nll` alongside
  `base_nll` and `adapter_nll`, enabling a three-way comparison; verified by
  `tests/test_twin_eval_popi.py::test_popi_baseline_in_yntp_output`.

- AC-11.9.3: GIVEN `HU_IS_TEST` is defined, WHEN `popi-summarize` runs,
  THEN no real LLM calls are made; the summarizer uses a fixture correction
  set and a deterministic template-based compression; verified by asserting
  no network calls in the test environment.

- AC-11.9.4: GIVEN the POPI summarizer is run with 0 correction pairs
  (cold start), WHEN `popi-summarize` runs, THEN it exits 0 and returns the
  empty string (not an error), and `twin-eval` falls back to base+system-prompt
  persona; verified by `tests/test_popi_summarize.py::test_empty_corrections_returns_empty`.

**Estimate:** M
**Priority:** P2 (Wave 3; stretch goal; only if Waves 0-2 land clean)
**Dependencies:** US-11.6 (YNTP evaluator must exist to run the comparison)
**Risk tier:** LOW (scripts and Python only; no C changes; no vtable interfaces)
**Test seam:**
  - `tests/test_popi_summarize.py` — fixture correction DB; token-count assertion
  - `tests/test_twin_eval_popi.py` — extended twin-eval with POPI baseline output
  - Command: `python3 -m pytest tests/test_popi_summarize.py tests/test_twin_eval_popi.py -v`
**Out of scope:** RL-trained POPI inference model (the full paper's architecture;
  deferred); POPI integration with live cloud providers (Gemini/Claude API calls
  with real user data); automatic preference extraction from conversation history
  (manual fixture in this story).

---

### US-11.10 (P2): Forced-choice held-out (Twin-2K-500 protocol) as secondary metric

**As a** developer evaluating behavioral alignment beyond linguistic mimicry,
**I want** a `human ml twin-eval --protocol twin2k` command that runs
forced-choice behavioral prediction on a held-out question set,
**so that** I can report whether the DPOP+DoRA adapter improves the model's
ability to predict Seth's actual behavioral choices, not just his linguistic
style.

**Source:** Twin-2K-500 arXiv 2505.17479 (2,058 real people, 500 behavioral
questions); benchmarks research (sota-benchmarks.md §3 behavioral consistency).

**Rationale:** The SOTA benchmarks research identifies a critical limitation:
"persona adaptation improves tone and lexical fidelity but does not transfer to
decision-making, planning, or strategic reasoning." The Twin-2K-500 protocol
tests this gap directly: given a user's answers to 40 behavioral questions,
predict their answer to the held-out 10th question. This is a forced-choice
metric that cannot be gamed by lexical fingerprints. Placing it as P2 (Wave 3)
is honest about scope: it requires Seth's manual labeling of 50 behavioral
questions and is a secondary metric to the primary YNTP NLL evaluator.

**Acceptance criteria:**

- AC-11.10.1: GIVEN `human ml twin-eval --protocol twin2k --holdout-file
  tests/fixtures/twin2k_seth_50q.jsonl`, WHEN the command runs, THEN it outputs
  `{"n_questions": int, "adapter_accuracy": float, "base_accuracy": float,
  "delta_accuracy": float, "stderr": float}`; verified by
  `tests/test_twin_eval_twin2k.c::test_twin2k_output_schema` with mock NLL seam.

- AC-11.10.2: GIVEN a fixture where the adapter's log-probability distribution
  correctly predicts 4 of 5 held-out answers vs base's 2 of 5, WHEN `twin-eval`
  runs, THEN `delta_accuracy = +0.40` (2/5) and `adapter_accuracy = 0.80`;
  verified numerically by `tests/test_twin_eval_twin2k.c::test_twin2k_accuracy_computation`.

- AC-11.10.3: GIVEN the fixture `tests/fixtures/twin2k_seth_50q.jsonl`
  contains 50 Seth-specific behavioral questions in the Twin-2K-500 format
  (`{question, options: [A, B, C, D], seth_answer}`), WHEN the evaluator loads
  it, THEN it validates structure and reports an error on malformed entries;
  verified by `tests/test_twin_eval_twin2k.c::test_twin2k_fixture_validation`.

- AC-11.10.4: GIVEN `HU_IS_TEST` is defined, WHEN `twin-eval` runs in twin2k
  mode, THEN no real model inference occurs; forced-choice probabilities are
  injected via the `hu_ml_nll_compute_fn_t` mock seam; verified by asserting
  no real weights are loaded.

**Estimate:** M
**Priority:** P2 (Wave 3; stretch goal; only if Waves 0-2 land clean)
**Dependencies:** US-11.6 (twin-eval command must exist as the base; twin2k
  adds a second --protocol mode)
**Risk tier:** MEDIUM (requires Seth's manual curation of 50 behavioral
  questions; if the fixture is not ready, the story blocks; risk is in data
  preparation, not implementation)
**Test seam:**
  - `tests/test_twin_eval_twin2k.c` with mock NLL seam and fixture validation
  - `tests/fixtures/twin2k_seth_50q.jsonl` (50 manually-curated behavioral
    questions; must be ready before implementation begins)
  - Command: `cmake --build --preset dev && ./build/human_tests --filter=twin_eval_twin2k`
**Out of scope:** Full Twin-2K-500 dataset evaluation (2,058 users × 500 Qs;
  we target n=1 user, Seth, with 50 questions); automated behavioral question
  generation; comparison across multiple users.

---

## Non-goals

- We will NOT implement the full ThinkPRM Stage 3 training pipeline (requires
  5-10K persona labels from Seth's manual effort; deferred to Sprint 12).
- We will NOT wire GRPO-2 or XPO loss heads (require online preference oracle;
  not available for persona transfer).
- We will NOT implement the full Anthropic activation-projection infrastructure
  for persona vectors (reading intermediate layer activations from Gemma-4;
  deferred to Sprint 12; US-11.3 uses the proxy `chosen_r` signal instead).
- We will NOT ship any M4/M6 user-facing features (100 DAU, channel focus);
  these are premature until the SOTA gate passes.
- We will NOT implement EWC-LoRA Fisher regularization (expensive Fisher
  diagonal computation at E2B scale; deferred to Sprint 12 if EMA alone
  proves insufficient).

---

## Dependency graph / wave assignments

```
Wave 0 (P0; parallel; unblock the rest):
  US-11.1  — Pad-token masking + length normalization   [no deps]
  US-11.2  — DoRA flag in finetune-gemma.py             [no deps]
  US-11.3  — Persona-vector early-stopping signal       [no deps]

Wave 1 (P0; sequential after Wave 0 merges to sprint-11-sota-twin):
  US-11.4  — DPOP loss head                             [requires US-11.1]
  US-11.5  — ORPO train_step wire-up                    [requires US-11.1]
  US-11.6  — Held-out next-utterance LL evaluator       [requires US-11.1, US-11.3]

Wave 2 (P1; sequential after Wave 1):
  US-11.7  — 4-stage Pareto gate                        [requires US-11.6]
  US-11.8  — Dual fast/slow LoRA + EMA promotion        [requires US-11.7, US-11.4]

Wave 3 (P2; stretch; only if Waves 0-2 land clean):
  US-11.9  — POPI summarizer baseline                   [requires US-11.6]
  US-11.10 — Forced-choice held-out (Twin-2K-500)       [requires US-11.6]
```

Stories within the same wave share no state and may be assigned to separate
worktrees and run in parallel. The Sprint 7 CHANGE-1 lesson applies: set
`worktree.baseRef=head` in `.claude/settings.local.json` BEFORE dispatching
Wave 0 implementers.

---

## Sprint success metric

Sprint 11 closes when ALL of the following are true:

1. **Primary:** `human ml twin-eval --protocol yntp` on the 30-prompt
   held-out fixture reports `delta_nll > 0` for the DPOP+DoRA adapter vs
   the base+system-prompt baseline, with `pad_rate == 0` on all 30 prompts.

2. **Pareto gate:** `scripts/pareto_picker.py` classifies the best
   Wave-1-trained adapter as PROMOTE (delta >= +0.03 AND pad <= 10%) or
   DEFER (delta >= +0.01 AND pad <= 50%). A REJECT outcome means the
   wave-0 infrastructure fixes alone did not close the gap; publish the
   negative result honestly and escalate to stakeholder.

3. **Regression guard:** The Sprint 8 broken adapter (iter-200, pad=80%,
   lexical-delta=+0.046) must FAIL the YNTP evaluator (AC-11.6.3) and the
   4-stage gate (AC-11.7.3). This is the canonical proof that the new
   metrics are not gameable by the old fingerprint.

4. **Early-stopping:** The Sprint 8 iter-60 trajectory (the best empirical
   result) must be reproducible by the US-11.3 early-stopping rule applied
   to the Sprint 8 fixture log (AC-11.3.2). The rule must not require
   manual inspection to fire.

5. **All P0 stories pass verifier + critic + aspect-panel** and are committed
   to `sprint-11-sota-twin` before any story is reported DONE.

6. **Sprint-auditor sign-off** with the same adversarial standard as Sprint 7.

---

Last line: RESULT_product-owner=READY
