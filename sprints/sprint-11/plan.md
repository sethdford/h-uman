# Sprint 11 Plan — SOTA Digital Twin

## Branch
`sprint-11-sota-twin`

## Base SHA
`8528cc55` (sprint scaffold commit)

## Head SHA
`312029bb` (10 tech-lead designs, all RESULT_tech-lead=READY or DESIGN_READY)

## Working directory
`/Users/sethford/Projects/h-uman/.claude/worktrees/hardcore-goldwasser-af5a11`

## Isolation level
Branch-only (workspace is on dedicated sprint branch; main worktree is on
`main`; other locked worktrees are from prior sprints and unrelated to this
sprint's file surface).

## Story count
10 stories across 4 waves.

## Designs
`sprints/sprint-11/designs/US-11.{1..10}.md` — 2,601 lines total.

## Research grounding
- `sprints/sprint-9-plus/sota-roadmap.md` — synthesized from 7 parallel
  research agents
- `sprints/sprint-9-plus/research/sota-dpo.md`
- `sprints/sprint-9-plus/research/sota-mlx-ondevice.md`
- `sprints/sprint-9-plus/research/sota-personalization.md`
- `sprints/sprint-9-plus/research/sota-reward-eval.md`
- `sprints/sprint-9-plus/research/sota-continual.md`
- `sprints/sprint-9-plus/research/sota-benchmarks.md`
- Sprint 8 smoke-run-3 evidence (pad-leakage root-cause, chosen_r cliff data)

---

## §1 Sprint Metadata

**Sprint goal:** Advance the digital-twin fine-tune loop from Sprint 8's
"infrastructure works, metric gameable, +0.019 delta with 40% pad-leakage"
to a SOTA-defensible held-out next-utterance prediction lift on the user's
own data, with zero pad-token leakage and a multi-dimensional Pareto
promotion gate.

**Sprint success metric (publishable claim):**

> "On real user data (n=N conversations from Seth's chat.db), DPOP+DoRA+
> early-stopping on Gemma-4-E2B improves held-out next-utterance
> log-likelihood by X% vs. the base+system-prompt persona baseline, with
> 0 pad-token leakage in 30+ held-out prompts."

**Pareto gate thresholds:**
- PROMOTE: delta >= +0.03 AND pad_rate <= 10%
- DEFER:   delta >= +0.01 AND pad_rate <= 50%
- REJECT:  otherwise

---

## §2 Wave Assignments Table

| Story | Title | Risk | Primary Files (summary from design) | Implementer | Verifier scope | Critic scope | Aspect-panel? | Worktree isolation? |
|---|---|---|---|---|---|---|---|---|
| US-11.1 | Pad masking + length norm | LOW | `scripts/mlx_lora_patch.py` (NEW), `scripts/mlx_lora_entry.py` (NEW), `scripts/finetune-gemma.py` (MODIFY), `tests/test_dpo_pad_masking.py` (NEW), `tests/test_pareto_pad_regression.py` (NEW), `scripts/pareto_picker.py` (MODIFY), 2 fixtures | general-purpose | `python3 -m pytest tests/test_dpo_pad_masking.py tests/test_pareto_pad_regression.py -v` all pass, 0 FAILED | Length-norm correctness, patch idempotency, Sprint 8 regression guard logic | YES (MEDIUM risk via regression guard on training pipeline) | YES |
| US-11.2 | DoRA flag | LOW | `scripts/finetune-gemma.py` (MODIFY +20/-2), `tests/test_finetune_gemma_dora.py` (NEW) | general-purpose | `python3 -m pytest tests/test_finetune_gemma_dora.py -v` all pass | Flag propagation correctness, default-change backward-compat risk | NO (LOW risk only) | YES |
| US-11.3 | chosen_r early-stop | MEDIUM | `scripts/early_stopping.py` (NEW), `scripts/finetune-gemma.py` (MODIFY +50), `tests/test_early_stopping.py` (NEW), `tests/fixtures/sprint8_dpo80_log.jsonl` (NEW) | general-purpose | `python3 -m pytest tests/test_early_stopping.py -v` all 5 pass; fixture replay asserts `first_breach_iter=65, stopped_iter=70, promoted_iter=60` | Window math, subprocess SIGTERM race, backward-compat of `--early-stopping-signal none` | YES | YES |
| US-11.4 | DPOP loss head | MEDIUM | `scripts/finetune-gemma.py` (MODIFY +47), `tests/test_finetune_gemma_dpop.py` (NEW), `tests/test_dpop_loss.py` (NEW), `tests/fixtures/dpop_golden.json` (NEW) | general-purpose | `python3 -m pytest tests/test_dpop_loss.py tests/test_finetune_gemma_dpop.py tests/test_finetune_gemma_dpo.py -v` all pass | `--delta` default-override correctness (Risk 1 — upstream default 50.0 vs our 0.1), DCR regression guard | YES | YES |
| US-11.5 | ORPO train_step | MEDIUM | `include/human/ml/rl_trainer.h` (MODIFY +18), `src/ml/rl_trainer_orpo.c` (NEW +180), `src/ml/cli.c` (MODIFY +85), `tests/test_rl_trainer_orpo.c` (NEW +200), `tests/test_ml_cli_rl_train.c` (MODIFY +50), `tests/fixtures/orpo_golden.json` (NEW), `CMakeLists.txt` (MODIFY +4) | general-purpose | `cmake --build --preset dev && ./build/human_tests --filter=rl_trainer_orpo` 0 failures, 0 ASan errors; `grpo2` still exits 2 | `log1mexp` numerical floor, grpo2 split correctness, SimPO regression | YES | YES |
| US-11.6 | Held-out NLL evaluator | HIGH | `include/human/ml/twin_eval.h` (NEW +90), `src/ml/twin_eval.c` (NEW +220), `src/ml/cli.c` (MODIFY +110), `src/ml/nll_backend_subprocess.c` (NEW +280), `include/human/ml/nll_backend_subprocess.h` (NEW +40), `scripts/yntp_nll_backend.py` (NEW +250), `scripts/derive_yntp_holdout.py` (NEW +180), 2 fixture files, `tests/test_twin_eval_yntp.c` (NEW +420), `tests/test_twin_eval_integration.sh` (NEW +90), `tests/test_yntp_holdout_fixture.c` (NEW +130), `scripts/pareto_picker.py` (MODIFY +40) | general-purpose | `cmake --build --preset dev && ./build/human_tests --filter=twin_eval`; `tests/test_twin_eval_integration.sh`; Sprint 8 broken-adapter fails gate | Schema correctness, PII guard, Sprint 8 regression (AC-11.6.3), pareto round-trip, `HU_IS_TEST` seam | YES (HIGH risk — primary sprint metric) | YES |
| US-11.7 | 4-stage Pareto gate | HIGH | `scripts/stage_cascade.py` (NEW +220), `scripts/cascade_stages/` pkg (4 NEW modules +285), `scripts/pareto_picker.py` (MODIFY +80), `scripts/check-lora-ab.sh` (MODIFY +60), `tests/test_pareto_gate.py` (NEW +280), `tests/test_check_lora_ab_staged.sh` (NEW +60), 3 fixture files | general-purpose | `python3 -m pytest tests/test_pareto_gate.py -v`; `tests/test_check_lora_ab_staged.sh`; Sprint 8 iter-200 REJECT at Stage 1 (AC-11.7.3) | Fail-fast ordering invariant, Stage 3 dormancy (D3 pattern), ensemble min-agg | YES (HIGH risk — gates adapter promotion) | YES |
| US-11.8 | Dual fast/slow LoRA + EMA | MEDIUM | `src/ml/lora_retrain_runner.c` (MODIFY +180), `include/human/ml/lora_retrain_runner.h` (MODIFY +60), `src/ml/lora_ema.c` (NEW +220), `include/human/ml/lora_ema.h` (NEW +50), `scripts/lora_ema.py` (NEW +120), `scripts/compute_kl_drift.py` (NEW +100), `src/agent/scheduler_status_json.c` (MODIFY +80), `src/cli/cmd_doctor.c` (MODIFY +20), `src/cli/cmd_adapter.c` (MODIFY +60), `tests/test_w14_dual_lora.c` (NEW +400), `tests/test_lora_ema.c` (NEW +150), fixtures | general-purpose | `cmake --build --preset dev && ./build/human_tests --filter=w14_dual_lora` + `--filter=lora_ema`; 4-night simulated scenario passes | Gate-verdict JSON contract fidelity, EMA matrix compat check, KL-drift threshold, rollback CLI symlink atomicity | YES | YES |
| US-11.9 | POPI summarizer baseline | LOW | `scripts/popi_summarize.py` (NEW +180), `scripts/popi_extract.py` (NEW +120), `scripts/twin_eval.py` (MODIFY +40), `tests/test_popi_summarize.py` (NEW +160), `tests/test_twin_eval_popi.py` (NEW +90), 2 fixture files, `src/cli/cli.c` (MODIFY +15) | general-purpose | `python3 -m pytest tests/test_popi_summarize.py tests/test_twin_eval_popi.py -v` | Token-limit determinism, no-LLM-call guard, three-way comparison schema | NO (LOW risk; pure scripts/Python) | YES |
| US-11.10 | Twin-2K-500 forced-choice | MEDIUM | `src/ml/twin_eval.c` (MODIFY +110), `include/human/ml/twin_eval.h` (MODIFY +15), `src/cli/cli_ml_twin_eval.c` (MODIFY +30), `src/ml/twin2k_fixture.c` (NEW +90), `include/human/ml/twin2k_fixture.h` (NEW +25), `tests/test_twin_eval_twin2k.c` (NEW +180), 3 fixture files | general-purpose | `cmake --build --preset dev && ./build/human_tests --filter=twin_eval_twin2k` | Fixture validation adversary test, binomial stderr arithmetic, `HU_IS_TEST` guard, argmax tie-break | YES (MEDIUM risk — fixture curation gates headline metric) | YES |

---

## §3 Per-Story Implementer Prompt Templates

All implementers receive the following preamble plus their story-specific section.

### Universal preamble (embed verbatim in every dispatch)

```
Sprint: sprint-11
Branch: sprint-11-sota-twin
Base SHA: 8528cc55
Working directory: /Users/sethford/Projects/h-uman/.claude/worktrees/hardcore-goldwasser-af5a11

READ BEFORE STARTING:
1. Your primary source is the design doc at sprints/sprint-11/designs/<your-story>.md — follow it
   exactly. The tech-lead has pre-resolved all major design questions.
2. Do NOT pause mid-task to ask clarifying questions (Sprint 7 CHANGE-2). Design docs contain
   recommended defaults for all open questions; proceed with those defaults.
3. Your work is NOT done until you have committed to sprint-11-sota-twin:
   git add <your files> && git commit -m "feat(<scope>): US-<N> <description>"
   Working-tree-only DONE reports will be rejected. The commit must appear in:
   git log sprint-11-sota-twin ^8528cc55 --oneline
4. Complete the quality gate sequence (§4 of the plan) before reporting DONE.
5. Honor all cross-story dependencies listed in §5 of the plan — do not start
   a story whose dependency has not yet merged to sprint-11-sota-twin.
```

---

### US-11.1 prompt

**Story:** US-11.1 (P0, Wave 0) — Pad-token masking + length normalization in DPO loss

**Design doc:** `sprints/sprint-11/designs/US-11.1.md` (primary source — read fully before touching any file)

**AC (verbatim, no paraphrasing):**

- AC-11.1.1: GIVEN a DPO training run launched via `finetune-gemma.py --dpo`, WHEN the training loop processes a batch, THEN the loss computation calls the mlx-lm-lora API with padding tokens excluded from the log-probability sum (verified by asserting the per-token mask tensor has 0.0 at pad positions in a unit test that inspects the loss-computation call with a fixture batch containing known pad positions).

- AC-11.1.2: GIVEN a batch where the chosen sequence is 10 tokens and the rejected sequence is 50 tokens (simulating pad-inflation of the rejected), WHEN the length-normalized loss is computed, THEN the per-token log-probability of each sequence is divided by its non-pad token count before the margin is taken (verified numerically: normalized_loss == unnormalized_loss / nonpad_count within 1e-5 tolerance, in `tests/test_dpo_pad_masking.py`).

- AC-11.1.3: GIVEN a training run on the Sprint 8 fixture dataset (30-prompt held-out set), WHEN pad masking + length normalization are active, THEN the post-training evaluation produces a pad-leakage rate strictly less than the Sprint 8 best (12/30 = 40%); verified by running `scripts/pareto_picker.py` against the fixture sweep output and asserting the best checkpoint does not score REJECT solely due to pad rate.

- AC-11.1.4 (regression guard): GIVEN a training configuration identical to Sprint 8 iter-60 but WITHOUT pad masking (the Sprint 8 broken adapter), WHEN evaluated through `scripts/pareto_picker.py`, THEN the verdict is DEFER or REJECT (pad_rate >= 40%), demonstrating the old config still fails the gate and the new masking config is a genuine improvement.

- AC-11.1.5: GIVEN `HU_IS_TEST` is defined, WHEN the test suite runs, THEN no real model weights are loaded; all NLL computations use the `hu_ml_nll_compute_fn_t` mock seam registered in test setup.

**Test command:** `HU_IS_TEST=1 python3 -m pytest tests/test_dpo_pad_masking.py tests/test_pareto_pad_regression.py -v`

**Commit message format:** `feat(scripts,tests): US-11.1 pad-token masking + length norm in DPO loss`

**Wave 0 — no dependencies.** May start immediately once worktree is set up.

---

### US-11.2 prompt

**Story:** US-11.2 (P0, Wave 0) — Add DoRA training mode to finetune-gemma.py

**Design doc:** `sprints/sprint-11/designs/US-11.2.md` (primary source)

**AC (verbatim):**

- AC-11.2.1: GIVEN `finetune-gemma.py` is called with `--train-type dora`, WHEN the training subprocess is constructed, THEN the mlx-lm-lora CLI invocation contains `--train-type dora` (not `--train-type lora`); verified by `tests/test_finetune_gemma_dora.py::test_dora_flag_propagated_to_mlx_cmd` using `subprocess.run` mock asserting the argv shape.

- AC-11.2.2: GIVEN `--train-type` is not specified (default), WHEN `finetune-gemma.py` runs, THEN the default is `dora` (upgrading the prior default of `lora`); verified by `tests/test_finetune_gemma_dora.py::test_default_train_type_is_dora`.

- AC-11.2.3: GIVEN `--train-type lora` is explicitly passed, WHEN the script runs, THEN the CLI invocation contains `--train-type lora` and DoRA is NOT used; verified by `tests/test_finetune_gemma_dora.py::test_explicit_lora_flag_respected`.

- AC-11.2.4: GIVEN a DoRA training run completes on the fixture dataset, WHEN `scripts/check-lora-baseline.sh` is run against the resulting adapter, THEN the script exits 0; verified by the baseline gate in CI.

- AC-11.2.5: GIVEN the `train_config.json` written by the versioning function, WHEN it is inspected, THEN the `train_type` field records `"dora"` when DoRA was used; verified by `tests/test_finetune_gemma_dora.py::test_train_config_records_dora`.

**Test command:** `python3 -m pytest tests/test_finetune_gemma_dora.py -v`

**Commit message format:** `feat(scripts,tests): US-11.2 DoRA flag in finetune-gemma.py (default dora)`

**Wave 0 — no dependencies.**

**Note on open question OQ-11.2.1:** Before step 3, verify `python3 -m mlx_lm_lora.train --help | grep train-type` shows `dora` as an option. If not, bump the mlx-lm-lora pin and note in PR body.

---

### US-11.3 prompt

**Story:** US-11.3 (P0, Wave 0) — Persona-vector projection as an early-stopping signal

**Design doc:** `sprints/sprint-11/designs/US-11.3.md` (primary source)

**AC (verbatim):**

- AC-11.3.1: GIVEN a DPO training run with `--early-stopping-signal chosen_r`, WHEN `train_chosen_r` drops below 50% of its trailing-5-window mean for two consecutive evaluation steps, THEN the training loop sets `should_stop=True` and saves the adapter from the prior window as the final artifact; verified by `tests/test_early_stopping.py::test_chosen_r_plateau_break_fires` with a fixture log containing the known Sprint 8 trajectory.

- AC-11.3.2: GIVEN the Sprint 8 iter 60-80 training log as a fixture, WHEN the early-stopping callback processes it, THEN it would have halted at iter 65 (the cliff iteration), saving the iter-60 checkpoint as the promoted artifact; verified by replaying the fixture log through the callback in `tests/test_early_stopping.py::test_sprint8_trajectory_stops_at_iter65`. NOTE per design: emit both `first_breach_iter=65` and `stopped_iter=70`; AC-11.3.2 asserts against `first_breach_iter`.

- AC-11.3.3: GIVEN a training run where `train_chosen_r` stays within 20% of its plateau mean throughout all iterations, WHEN the callback processes the run, THEN no early stopping fires and training runs to the configured `--iters` limit.

- AC-11.3.4: GIVEN the early-stopping callback fires, WHEN the training log is inspected, THEN a structured log line is emitted containing `"early_stop"`, `"reason": "chosen_r_plateau_break"`, `"stopped_iter"`, and `"promoted_iter"` fields.

- AC-11.3.5: GIVEN `--early-stopping-signal none` (disabled), WHEN the training loop runs, THEN the plateau-break callback is never invoked and training runs to `--iters`; backward-compatible with existing CI scripts.

**Test command:** `python3 -m pytest tests/test_early_stopping.py -v`

**Commit message format:** `feat(scripts,tests): US-11.3 chosen_r plateau-break early-stopping`

**Wave 0 — no dependencies.** Uses Sprint 8 fixture log from `/tmp/dpo80.log` (see design §2 step 2 for the fixture derivation approach).

---

### US-11.4 prompt

**Story:** US-11.4 (P0, Wave 1) — DPOP loss head (Smaug positive-clipping)

**Design doc:** `sprints/sprint-11/designs/US-11.4.md` (primary source)

**AC (verbatim):**

- AC-11.4.1: GIVEN `finetune-gemma.py --dpo --variant dpop --dpop-lambda 0.1`, WHEN the training subprocess is constructed, THEN the CLI contains `--variant dpop` and `--dpop-lambda 0.1` passed to mlx-lm-lora (translated internally to `--dpo-cpo-loss-type dpop` and `--delta 0.1`); verified by `tests/test_finetune_gemma_dpop.py::test_dpop_flag_propagated` using `subprocess.run` mock.

- AC-11.4.2: GIVEN the DPOP loss function with a fixture batch where `log_pi_ref(y_chosen) > log_pi(y_chosen)` (the DCR failure condition), WHEN `compute_dpop_loss` is called, THEN the returned loss is strictly greater than the corresponding vanilla DPO loss; verified numerically in `tests/test_dpop_loss.py::test_dpop_penalty_fires_on_dcr_condition` within 1e-5 tolerance.

- AC-11.4.3: GIVEN the DPOP loss function with a fixture batch where `log_pi(y_chosen) >= log_pi_ref(y_chosen)` (the non-DCR case), WHEN `compute_dpop_loss` is called, THEN the penalty term is exactly 0 and the loss equals vanilla DPO loss.

- AC-11.4.4 (regression guard): GIVEN the Sprint 8 iter-80 training scenario (chosen_r went to -8.867 under vanilla DPO), WHEN the same training configuration runs with `--variant dpop`, THEN `train_chosen_r` must NOT drop below 0 at any sampled checkpoint in the fixture simulation.

- AC-11.4.5: GIVEN `--variant dpo` (vanilla DPO, the prior default), WHEN the script runs, THEN behavior is identical to pre-story DPO behavior; no regression; verified by existing `tests/test_finetune_gemma_dpo.py` tests passing without modification.

**Test command:** `python3 -m pytest tests/test_dpop_loss.py tests/test_finetune_gemma_dpop.py tests/test_finetune_gemma_dpo.py -v`

**Commit message format:** `feat(scripts,tests): US-11.4 DPOP loss head via mlx_lm_lora --delta`

**Wave 1 dependency — MANDATORY pre-flight:** Confirm US-11.1 has merged before starting:
```
git log sprint-11-sota-twin ^8528cc55 --oneline | grep -i "US-11.1"
```
If that returns nothing, STOP and surface to scrum-master.

**Critical Risk 1 (upstream `--delta` default is 50.0, NOT 0.1):** Always pass `--delta <args.dpop_lambda>` explicitly; NEVER fall through to upstream default. The unit test MUST assert the default path emits `--delta 0.1`.

---

### US-11.5 prompt

**Story:** US-11.5 (P0, Wave 1) — Wire ORPO train_step (finish Sprint 7 US-7.10 stub)

**Design doc:** `sprints/sprint-11/designs/US-11.5.md` (primary source — includes absolute file paths at §10)

**AC (verbatim):**

- AC-11.5.1: GIVEN `human ml rl-train --algorithm orpo --lambda-orpo 0.1` with the fixture dataset, WHEN the command runs, THEN it exits 0 (not exit code 2 "not yet implemented"), and the `hu_rl_trainer_orpo_t` factory is selected; verified by `tests/test_rl_trainer_orpo.c::test_orpo_train_exits_0` with `HU_IS_TEST` guards on file writes.

- AC-11.5.2: GIVEN a fixture batch `{prompt, chosen}` (no `rejected` field required by ORPO), WHEN `compute_loss` is called on the ORPO trainer, THEN the returned loss equals `NLL(chosen) + lambda * OR_penalty(chosen)` within 1e-4 absolute tolerance; verified analytically in `tests/test_rl_trainer_orpo.c::test_orpo_loss_golden`.

- AC-11.5.3: GIVEN the ORPO loss is computed on a batch where `log_pi(chosen)` is already high (model has learned the preference), WHEN the OR penalty is computed, THEN the penalty term approaches 0; verified in `tests/test_rl_trainer_orpo.c::test_orpo_or_penalty_diminishes_at_high_log_prob`.

- AC-11.5.4: GIVEN `--algorithm simpo` (Sprint 7 US-7.10 golden factory), WHEN `human ml rl-train` is invoked, THEN behavior is identical to Sprint 7 baseline (no regression); verified by `tests/test_rl_trainer_simpo.c` passing without modification.

- AC-11.5.5: GIVEN `--algorithm grpo2`, WHEN `human ml rl-train` is invoked, THEN it still exits 2 with "not yet implemented"; verified by existing `tests/test_ml_cli_rl_train.c::test_rl_train_unimplemented_algorithms`.

- AC-11.5.6: All new C code compiles with `-Wall -Wextra -Wpedantic -Werror` and zero ASan errors under `cmake --preset dev`.

**Test command:** `cmake --build --preset dev && ./build/human_tests --filter=rl_trainer_orpo`

**Commit message format:** `feat(ml,tests): US-11.5 ORPO train_step — wire hu_rl_trainer_orpo_t factory`

**Wave 1 dependency:** US-11.1 must have merged (ORPO training should use pad-masked NLL by default). Sprint 7 US-7.10 vtable is already shipped; this story extends it.

**After committing, file `sprints/sprint-11/followups.md` entry for FU-11.5.a** (production NOT_SUPPORTED gap, mirroring FU-7.10.a).

---

### US-11.6 prompt

**Story:** US-11.6 (P1, Wave 1) — Held-out next-utterance log-likelihood evaluator

**Design doc:** `sprints/sprint-11/designs/US-11.6.md` (primary source — HIGH risk; read fully)

**AC (verbatim):**

- AC-11.6.1: GIVEN `human ml twin-eval --protocol yntp --holdout-file tests/fixtures/yntp_holdout_30.jsonl --adapter <adapter_path>`, WHEN the command runs, THEN it outputs a JSON object with fields `{"base_nll": float, "adapter_nll": float, "delta_nll": float, "n_prompts": int, "pad_rate": float}` where `delta_nll` is `base_nll - adapter_nll` (positive = improvement).

- AC-11.6.2: GIVEN a fixture adapter that returns lower NLL than base on all holdout prompts, WHEN `twin-eval` runs, THEN `delta_nll > 0` and `pad_rate == 0.0`.

- AC-11.6.3 (regression guard): GIVEN a fixture that simulates the Sprint 8 iter-200 broken adapter (high NLL on coherent continuations, many pad tokens), WHEN `twin-eval` runs, THEN `delta_nll <= 0` OR `pad_rate >= 0.5`, confirming the lexical fingerprint's false positive cannot recur on this metric.

- AC-11.6.4: GIVEN `HU_IS_TEST` is defined, WHEN `twin-eval` runs, THEN no real model weights are loaded; all NLL values are injected via the `hu_ml_nll_compute_fn_t` mock seam (same seam pattern as US-7.6).

- AC-11.6.5: GIVEN the holdout fixture with 30 prompts, WHEN the evaluator runs against two fixture adapters, THEN the output JSON is machine-parseable by `scripts/pareto_picker.py` as the `fidelity_delta` and `pad_failure_rate` inputs; verified by a round-trip test in `tests/test_twin_eval_integration.sh`.

- AC-11.6.6: The holdout fixture `tests/fixtures/yntp_holdout_30.jsonl` contains >= 30 real-format (PII-scrubbed) `{prompt, continuation}` pairs; verified by a fixture-validation test asserting field presence and PII-scrub patterns.

**Test command:** `cmake --build --preset dev && ./build/human_tests --filter=twin_eval && bash tests/test_twin_eval_integration.sh`

**Commit message format:** `feat(ml,scripts,tests): US-11.6 held-out YNTP NLL evaluator (twin-eval --protocol yntp)`

**Wave 1 dependencies:** US-11.1 (pad masking must be active before pad_rate metric is meaningful), US-11.3 (early stopping ensures plateau-window checkpoint, not collapsed checkpoint, is evaluated).

**Implementation note (design §5 step 7):** The `derive_yntp_holdout.py` step requires Seth's machine for real `memory.db` access. Implement steps 1-6 (C framework + mock seam) in the worktree; flag step 7 (fixture derivation + Seth review) as needing Seth's attention before the story is fully closed. Proceed with the hybrid approach (option C in OQ-11.6.1): synthetic public fixture for CI, private real-data fixture for the actual headline number.

**US-11.6 is the linchpin story** — US-11.7, 11.8, 11.9, and 11.10 all depend on it. Prioritize it within Wave 1.

---

### US-11.7 prompt

**Story:** US-11.7 (P1, Wave 2) — 4-stage Pareto gate cascade

**Design doc:** `sprints/sprint-11/designs/US-11.7.md` (primary source — HIGH risk)

**AC (verbatim):**

- AC-11.7.1: GIVEN an adapter whose held-out PPL is greater than 3 times the base model's PPL on the persona dev set, WHEN the 4-stage gate runs, THEN the gate exits with code 2 (REJECT) at Stage 1 without proceeding to Stage 2.

- AC-11.7.2: GIVEN an adapter that passes the PPL floor (Stage 1) but produces pad-token-containing outputs on >= 50% of Stage 2 coherence prompts, WHEN the coherence judge runs, THEN the gate exits with code 2 (REJECT) at Stage 2.

- AC-11.7.3 (regression guard): GIVEN the Sprint 8 iter-200 adapter scenario (pad_rate = 80%, delta = +0.046 on lexical fingerprint), WHEN the 4-stage gate processes it, THEN the gate REJECTS at Stage 1 or Stage 2, despite the positive lexical delta.

- AC-11.7.4: GIVEN an adapter that passes Stages 1-2, WHEN Stage 3 is invoked, THEN the stub returns a configurable fixture score (enabling testing of Stage 4 logic without a trained PRM); the stub is gated by `--stage3-stub` CLI flag, and the real PRM path returns NOT_IMPLEMENTED with a clear log message.

- AC-11.7.5: GIVEN 3 orthogonal judge scores (lexical, coherence, NLL-based) at Stage 4, WHEN ensemble aggregation runs with min-aggregation, THEN the final verdict is no better than the worst individual judge's verdict.

- AC-11.7.6: The 4-stage gate is integrated into `scripts/check-lora-ab.sh` as a `--staged-gate` flag (NOTE: design uses `--cascade`; implement as `--staged-gate` per AC text or confirm with scrum-master); verified by `tests/test_check_lora_ab_staged.sh`.

**Test command:** `python3 -m pytest tests/test_pareto_gate.py -v && bash tests/test_check_lora_ab_staged.sh`

**Commit message format:** `feat(scripts,tests): US-11.7 4-stage Pareto gate cascade`

**Wave 2 dependency:** US-11.6 must have merged. The US-11.7 orchestrator calls `human ml twin-eval --protocol yntp` from US-11.6 for Stage 1 input.

**Critical Stage 3 dormancy rule (D3 pattern):** Stage 3 ALWAYS emits `[cascade] stage3: SKIP (PRM not trained — Sprint 12)`. Its score is `null` in the ensemble. A null score CANNOT advance a verdict from DEFER to PROMOTE. This is non-negotiable; the AC tests check this explicitly.

**US-11.7 gate-verdict JSON contract (pinned for US-11.8 integration):**
```json
{
  "verdict": "PROMOTE | DEFER | REJECT",
  "stages": {
    "ppl_floor":  {"passed": bool, "value": float, "threshold": float},
    "coherence":  {"passed": bool, "pad_rate": float, "threshold": float},
    "prm":        {"passed": bool, "score": float, "stub": bool},
    "ensemble":   {"passed": bool, "aggregation": "min"}
  },
  "delta_nll": float,
  "pad_rate": float
}
```
US-11.8 reads `verdict` as authoritative. Do not deviate from this shape without scrum-master sign-off.

---

### US-11.8 prompt

**Story:** US-11.8 (P1, Wave 2) — Dual fast/slow LoRA + EMA promotion for W14 cron

**Design doc:** `sprints/sprint-11/designs/US-11.8.md` (primary source — MEDIUM risk; large story)

**AC (verbatim):**

- AC-11.8.1: GIVEN the W14 idle scheduler fires and new correction pairs are available, WHEN the nightly retrain runs, THEN it produces two artifacts: `fast.safetensors` (tonight's batch only) and `slow.safetensors.v{N}` (the current promoted slow adapter).

- AC-11.8.2: GIVEN the Pareto gate returns PROMOTE for tonight's fast adapter, WHEN the EMA update runs, THEN `slow.safetensors` is updated as `slow = 0.95 * slow + 0.05 * fast` and a new `v{N+1}` versioned copy is saved; the `current` symlink advances to `slow.safetensors.v{N+1}`.

- AC-11.8.3: GIVEN the Pareto gate returns REJECT for tonight's fast adapter, WHEN the cron observes the failure, THEN `fast.safetensors` is moved to `quarantine/{date}.safetensors`, the slow adapter is NOT updated, and a `nightly_retrain_rejected` event is emitted with the gate verdict.

- AC-11.8.4: GIVEN the dual-LoRA cron has run for at least 3 nights (simulated via fixture), WHEN `human adapter rollback` is called, THEN the `current` symlink is moved back to `slow.safetensors.v{N-1}` and the current night's version is preserved as `quarantine/{date}.safetensors`.

- AC-11.8.5: The `~/.human/scheduler.status` JSON `lora_retrain` block gains `fast_version`, `slow_version`, `last_ema_alpha`, and `last_gate_verdict` fields; `human doctor scheduler` parses and displays them.

**Test command:** `cmake --build --preset dev && ./build/human_tests --filter=w14_dual_lora && ./build/human_tests --filter=lora_ema`

**Commit message format:** `feat(ml,cli,tests): US-11.8 dual fast/slow LoRA + EMA promotion (W14 cron)`

**Wave 2 dependency:** US-11.7 MUST have merged before starting. The US-11.7 gate-verdict JSON contract (pinned in §3 US-11.7 prompt above) is the interface US-11.8 parses. Do not start US-11.8 in parallel with US-11.7 within Wave 2.

**Critical Risk #3 (US-11.8 design):** DEFER is treated the same as REJECT for the cron in Sprint 11 (quarantine, no EMA update). The distinction exists only for analytics in `pareto_picker.py`.

---

### US-11.9 prompt (Wave 3 / stretch)

**Story:** US-11.9 (P2, Wave 3, stretch) — POPI summarizer baseline

**Design doc:** `sprints/sprint-11/designs/US-11.9.md` (primary source)

**Pre-flight gate (mandatory first step):** Verify US-11.6 has merged:
```
git log sprint-11-sota-twin ^8528cc55 --oneline | grep -i "US-11.6"
```
If nothing returned, STOP. This story does not start until US-11.6 is committed.

**AC (verbatim):**

- AC-11.9.1: GIVEN `human ml popi-summarize --corrections-db dpo_pairs.db --max-pairs 50 --max-tokens 100`, WHEN the command runs, THEN it produces a plain-text preference summary of <= 100 tokens (whitespace-split) that captures at least 3 distinct style preferences evidenced in the corrections.

- AC-11.9.2: GIVEN the POPI summary is injected into the system prompt alongside the existing persona context, WHEN `human ml twin-eval --protocol yntp --baseline popi` runs, THEN the JSON output includes `popi_nll` alongside `base_nll` and `adapter_nll`, enabling a three-way comparison.

- AC-11.9.3: GIVEN `HU_IS_TEST` is defined, WHEN `popi-summarize` runs, THEN no real LLM calls are made; the summarizer uses a fixture correction set and a deterministic template-based compression.

- AC-11.9.4: GIVEN the POPI summarizer is run with 0 correction pairs (cold start), WHEN `popi-summarize` runs, THEN it exits 0 and returns the empty string (not an error), and `twin-eval` falls back to base+system-prompt persona.

**Test command:** `python3 -m pytest tests/test_popi_summarize.py tests/test_twin_eval_popi.py -v`

**Commit message format:** `feat(scripts,tests): US-11.9 POPI summarizer baseline (rule-based v1)`

---

### US-11.10 prompt (Wave 3 / stretch)

**Story:** US-11.10 (P2, Wave 3, stretch) — Twin-2K-500 forced-choice secondary metric

**Design doc:** `sprints/sprint-11/designs/US-11.10.md` (primary source)

**Pre-flight gate (mandatory first step):**
1. Verify US-11.6 has merged (provides `twin-eval` CLI and `hu_ml_nll_compute_fn_t` seam).
2. Resolve Open Question #1: has Seth committed time to curate the 50-question fixture? If no, ship code + 10-question synthetic demo fixture and mark US-11.10 PARTIAL per the design's recommendation (b). Do NOT block on labelling; code ships regardless.

**AC (verbatim):**

- AC-11.10.1: GIVEN `human ml twin-eval --protocol twin2k --holdout-file tests/fixtures/twin2k_seth_50q.jsonl`, WHEN the command runs, THEN it outputs `{"n_questions": int, "adapter_accuracy": float, "base_accuracy": float, "delta_accuracy": float, "stderr": float}`.

- AC-11.10.2: GIVEN a fixture where the adapter's log-probability distribution correctly predicts 4 of 5 held-out answers vs base's 2 of 5, WHEN `twin-eval` runs, THEN `delta_accuracy = +0.40` (2/5) and `adapter_accuracy = 0.80`.

- AC-11.10.3: GIVEN the fixture `tests/fixtures/twin2k_seth_50q.jsonl` contains 50 Seth-specific behavioral questions in the Twin-2K-500 format (`{question, options: [A, B, C, D], seth_answer}`), WHEN the evaluator loads it, THEN it validates structure and reports an error on malformed entries.

- AC-11.10.4: GIVEN `HU_IS_TEST` is defined, WHEN `twin-eval` runs in twin2k mode, THEN no real model inference occurs; forced-choice probabilities are injected via the `hu_ml_nll_compute_fn_t` mock seam.

**Test command:** `cmake --build --preset dev && ./build/human_tests --filter=twin_eval_twin2k`

**Commit message format:** `feat(ml,tests): US-11.10 Twin-2K-500 forced-choice secondary metric`

---

## §4 Quality Gate Sequence Per Story

The following sequence applies to EVERY story before it may be reported DONE.
No exceptions. No story advances past step N until step N passes.

**Step 1 — Implementer commits to sprint branch**

The implementer runs:
```
git add <paths>
git commit -m "feat(<scope>): US-<N> <description>"
```

**Step 2 — Scrum-master verifies commit landed**

```
git log sprint-11-sota-twin ^8528cc55 --oneline | grep -i "US-<N>"
```
If nothing returned: working-tree-only DONE report. Story re-opens. Re-dispatch.

**Step 3 — /verify (verifier agent)**

Verifier spawns against the story's test command (see §3 per-story sections).
Must return `RESULT_verifier=PASS` with captured test output showing all AC
tests green and 0 failures. FAIL or INCONCLUSIVE: story stays in flight.

**Step 4 — /critic (per-story, NOT batched)**

A critic agent reviews the committed diff immediately after verifier PASS.
Do NOT defer critic runs to sprint end. Findings tagged `CRITIC-` become new
tasks. A story is DONE only when the critic returns CLEAN or only LOW/INFO.
HIGH or CRITICAL findings block closure.

Sprint 7 retro established: batched critic at sprint end shipped a regex bug
into the publish path. Per-story critic is mandatory.

**Step 5 — /aspect-panel (for MEDIUM+ risk stories)**

Stories exempt from aspect-panel: **US-11.2 only** (LOW risk, flag plumbing).
All other stories (US-11.1, 11.3, 11.4, 11.5, 11.6, 11.7, 11.8, 11.9, 11.10)
require aspect-panel to return PASS or CLEAN before closure.

If aspect-panel returns ESCALATE: halt story. Surface to scrum-master.

**Step 6 — Mark DONE**

Only after ALL of steps 1-5 pass (or are exempt per above). Update the
sprint evidence directory: `sprints/sprint-11/evidence/US-<N>/`.

---

## §5 Cross-Story Coordination

### Dependency tree

```
Wave 0 (parallel; start immediately):
  US-11.1  (none)
  US-11.2  (none)
  US-11.3  (none)

Wave 1 (start after ALL Wave 0 stories are committed to sprint-11-sota-twin):
  US-11.4  → requires US-11.1 (pad masking precondition for DPOP)
  US-11.5  → requires US-11.1 (pad-masked NLL for ORPO)
  US-11.6  → requires US-11.1 + US-11.3 (pad_rate metric + plateau-window checkpoint)

Wave 2 (start after ALL Wave 1 stories are committed):
  US-11.7  → requires US-11.6 (NLL evaluator provides Stage 1 PPL input)
  US-11.8  → requires US-11.7 (gate verdict), requires US-11.4 (DPOP default)
             NOTE: US-11.7 must be DONE before US-11.8 starts — sequential within Wave 2

Wave 3 / stretch (start after ALL Wave 1+2 P0/P1 stories are committed):
  US-11.9  → requires US-11.6 (twin-eval CLI + NLL seam)
  US-11.10 → requires US-11.6 (twin-eval --protocol dispatch)
             NOTE: US-11.9 and US-11.10 may run in parallel within Wave 3
```

### Critical coordination points

**US-11.6 is the linchpin.** It gates 4 downstream stories (11.7, 11.8, 11.9,
11.10). Treat it as the highest-priority Wave 1 work. If it is delayed, Wave 2
and Wave 3 delay proportionally. Scrum-master monitors US-11.6 progress actively
and escalates blockers immediately.

**US-11.7 gates US-11.8 within Wave 2.** Do NOT dispatch US-11.8 in parallel
with US-11.7. The US-11.7 gate-verdict JSON contract (pinned in §3) is the
interface US-11.8 will parse. Any mid-flight change to that contract requires
explicit scrum-master sign-off and US-11.8 re-baseliining.

**US-11.1 pad masking is the Wave 0 dependency for three Wave 1 stories.**
If US-11.1 has a blocker (mlx-lm-lora version assertion fails, monkey-patch
breaks upstream format), escalate immediately — it blocks the whole sprint.

**US-11.4 `--delta` default is 50.0 upstream, NOT 0.1.**
US-11.4 must ALWAYS pass `--delta <args.dpop_lambda>` explicitly (never fall
through to upstream). This is the single highest-probability bug in the sprint
(design Risk 1, probability HIGH, impact LARGE). The unit test asserts the
default path emits `--delta 0.1`. If this assertion is not present in the
merged commit, the story re-opens.

**US-11.3 `first_breach_iter` vs `stopped_iter` semantics.**
AC-11.3.2 asserts against `first_breach_iter=65`. The design emits both fields.
Implementer MUST emit `first_breach_iter` (not only `stopped_iter`) in the
structured log line, or AC-11.3.4 will fail.

**US-11.7 `--staged-gate` vs `--cascade` naming.**
The AC text says `--staged-gate`; the design doc says `--cascade`. The
implementer should implement `--staged-gate` per the AC text (which has
contractual priority) unless the scrum-master confirms otherwise. The test
filename `tests/test_check_lora_ab_staged.sh` is preserved in both cases.

**US-11.10 Seth fixture curation gate.**
US-11.10 code (steps 1-6) is unblocked from the moment US-11.6 merges. The
50-question Seth fixture (step 7) requires Seth's labelling time (OQ-11.10.1).
Resolve OQ-11.10.1 BEFORE dispatching the implementer, even if the answer is
"ship code + 10-question demo fixture, defer 50-question to Sprint 12."

**US-11.6 fixture derivation gate.**
The `derive_yntp_holdout.py` step requires Seth's `memory.db` on Seth's
machine. The implementer completes all CI-runnable steps (1-6 of design §5)
in the worktree; Seth runs step 7 (fixture derivation + review + commit).
This is a BLOCKING open question (OQ-11.6.1): determine before Wave 1
whether we commit the real-data fixture (option A), the synthetic-only (B),
or the hybrid (C). Recommendation is option C (hybrid).

---

## §6 Budget and Timing Estimate

| Category | Estimate |
|---|---|
| ~10 implementer agents (parallel within waves) | $30-50 |
| Verifiers × 10 stories | ~$15 |
| Critics × 10 stories (per-story, not batched) | ~$12 |
| Aspect-panels × 9 stories (US-11.2 exempt) | ~$22 |
| Sprint review + audit + retro | ~$10 |
| **Total** | **~$90-110** |

**Timing estimate (4-wave sequential structure):**
- Wave 0 (3 parallel stories): ~2-3h wall clock
- Wave 1 (3 parallel stories + linchpin US-11.6): ~3-4h (US-11.6 is L-estimate, critical path)
- Wave 2 (sequential US-11.7 then US-11.8): ~4-5h (both are L-estimate)
- Wave 3 / stretch (2 parallel stories): ~2h
- Quality gates per story (verifier + critic + aspect-panel): ~1h per story
- **Total wall clock:** ~18-22h across 4 waves

---

## §7 Open Questions

All open questions are collected from US-11.1 through US-11.10 design docs.
Each is classified NON-BLOCKING (proceed with design default) or BLOCKING
(resolve before implementer starts).

| ID | From | Question | Classification | Default / Recommended action |
|---|---|---|---|---|
| OQ-11.1.1 | US-11.1 §7 | Should the new `compute_score` length-normalize BOTH `sigmoid` and `dpop` paths, or only `sigmoid`? | NON-BLOCKING | Both sigmoid AND dpop AND hinge (design §1c recommendation). Implementer proceeds with this default. |
| OQ-11.1.2 | US-11.1 §7 | `train_config.json` schema bump: treat missing `length_normalize` in Sprint 8 files as `false`? | NON-BLOCKING | Yes; document as JSON field default. Defer doc update to Sprint 12. |
| OQ-11.1.3 | US-11.1 §7 | `--length-normalize` as config-file-only (via `-c` YAML) rather than CLI flag? | NON-BLOCKING | Keep CLI-first with YAML override via existing mechanism. No additional code required. |
| OQ-11.2.1 | US-11.2 OQ 1 | Does `mlx-lm-lora>=2.1.0` support `--train-type dora`? | BLOCKING (implementer resolves at step 1) | Run `python3 -m mlx_lm_lora.train --help \| grep train-type` before step 3. If dora not listed, bump pin and note in PR body. |
| OQ-11.2.2 | US-11.2 OQ 2 | Does `check-lora-baseline.sh` accept DoRA adapters? | BLOCKING (implementer resolves at step 1) | Read script before claiming AC-11.2.4 passes; surface as blocker if it pattern-matches LORA. |
| OQ-11.2.3 | US-11.2 OQ 3 | Sprint 11 changelog note that default training mode changed? | NON-BLOCKING | Yes; add to PR body. Scrum-master propagates to release notes. |
| OQ-11.3.1 | US-11.3 OQ 1 | Anthropic persona-vector projection scope? | NON-BLOCKING | Kept deferred to Sprint 12. `chosen_r` is the correct shipping signal for Sprint 11. |
| OQ-11.3.2 | US-11.3 OQ 2 | `stopped_iter` vs `first_breach_iter` as headline in structured log? | NON-BLOCKING | Emit both; AC-11.3.2 asserts against `first_breach_iter`. |
| OQ-11.3.3 | US-11.3 OQ 3 | `save_every` coupling: pass into detector constructor? | NON-BLOCKING | Yes; pass `save_every` into detector constructor; `finetune-gemma.py` forwards from its own args. |
| OQ-11.3.4 | US-11.3 OQ 4 | `--early-stopping-mode {confirm,first-breach}` flag? | NON-BLOCKING | Ship `confirm` only; revisit if evidence shows cliff window is being missed. |
| OQ-11.4.1 | US-11.4 §6 | Should `--variant dpop` become the default in Sprint 12? | NON-BLOCKING | No for Sprint 11 (preserve back-compat). Revisit after US-11.7 eval. |
| OQ-11.4.2 | US-11.4 §6 | `--delta` vs `--dpop-lambda` naming? | NON-BLOCKING | Confirmed: expose `--dpop-lambda` to users; translate to upstream `--delta` at subprocess boundary. |
| OQ-11.5.1 | US-11.5 §9 | Should `--lambda-orpo` also be accepted as `--beta`? | NON-BLOCKING | No; keep `--lambda-orpo` spelling only, matching AC and research doc. |
| OQ-11.5.2 | US-11.5 §9 | Should this story file FU-11.5.a? | NON-BLOCKING | Yes; file in `sprints/sprint-11/followups.md` documenting the NOT_SUPPORTED-in-production gap. |
| OQ-11.6.1 | US-11.6 §6.1 | Commit real-data fixture, synthetic-only, or hybrid? | BLOCKING (resolve before Wave 1 dispatch) | Recommendation (C) hybrid: synthetic fixture for CI, 10-row redacted real-data for reviewer audit, full private fixture for headline number on Seth's machine. Seth must confirm before Wave 1. |
| OQ-11.6.2 | US-11.6 §6.2 | Should evaluator also emit `raw_base_nll` (no persona prompt) as a third column? | NON-BLOCKING | Yes; extend JSON schema additively with `raw_base_nll`. This costs one extra inference pass. |
| OQ-11.6.3 | US-11.6 §6.3 | Pad-token detection: by tokenizer id, literal substring, or both? | NON-BLOCKING | Both (option iii); defensive against different tokenizer decode behaviors. |
| OQ-11.7.1 | US-11.7 §6 | `--staged-gate` vs `--cascade` shell flag name? | BLOCKING (scrum-master resolves) | AC text says `--staged-gate`; implement per AC text. Design doc was advisory. |
| OQ-11.7.2 | US-11.7 §6 | Stage 1 PPL source: derived from NLL via `exp(NLL/n_tokens)` from US-11.6? | NON-BLOCKING | Yes; Stage 1 derives PPL from US-11.6's NLL output. No separate PPL evaluator. |
| OQ-11.7.3 | US-11.7 §6 | Stage 4 min-aggregation policy on null Stage 3 score? | NON-BLOCKING | Min is taken over non-null subset. Null scores excluded from aggregation; never treated as REJECT or PROMOTE. |
| OQ-11.7.4 | US-11.7 §6 | Stage 2 coherence threshold: 0.7 score AND pad_rate >= 0.5 = REJECT? | NON-BLOCKING | Both applied: pad_rate >= 0.5 OR mean_coherence < 0.7 triggers REJECT at Stage 2. |
| OQ-11.8.1 | US-11.8 OQ 1 | Matrix-EMA vs delta-EMA? | NON-BLOCKING | Ship matrix-EMA (simpler) for Sprint 11; evaluate numerically in Sprint 12. |
| OQ-11.8.2 | US-11.8 OQ 2 | OLD-pairs auto-curation script? | NON-BLOCKING | Defer `scripts/promote_old_pair.py` to Sprint 12; Sprint 11 ships with 30-pair manual fixture. |
| OQ-11.8.3 | US-11.8 OQ 3 | DEFER semantics for Sprint 12? | NON-BLOCKING | Sprint 11: DEFER = REJECT for cron. Sprint 12: revisit shadow-slot promotion. |
| OQ-11.8.4 | US-11.8 OQ 4 | Quarantine retention / GC? | NON-BLOCKING | Sprint 11: quarantine grows monotonically. Sprint 12: `human adapter prune`. |
| OQ-11.9.1 | US-11.9 §6 | `--use-llm` flag: should Sprint 12 follow-up support both local and cloud summarizers? | NON-BLOCKING | Yes; provider-agnostic with Gemini 3.1 as default. Sprint 12 backlog item. |
| OQ-11.9.2 | US-11.9 §6 | Whitespace-tokens vs BPE-tokens for the 100-token budget? | NON-BLOCKING | V1: whitespace only (per AC-11.9.1). Add BPE upper bound in evidence report commentary; don't change the AC. |
| OQ-11.9.3 | US-11.9 §6 | POPI summary in addition to or in place of personal-model block? | BLOCKING (Seth/PO resolves before Wave 3) | Recommendation: in addition. Confirm with Seth before dispatching US-11.9. |
| OQ-11.10.1 | US-11.10 §OQ | Does Seth want to invest 2-3h curating 50-question behavioral fixture? | BLOCKING (Seth resolves before US-11.10 dispatch) | Recommendation (b): ship code + 10-question synthetic demo fixture; defer 50-question labelling to Sprint 12. Confirm before dispatch. |
| OQ-11.10.2 | US-11.10 §OQ | Public repo vs git-ignored for Seth's behavioral choices fixture? | BLOCKING (Seth resolves, tied to OQ-11.10.1) | Recommendation: local + git-ignored; add synthetic public fixture for CI. |
| OQ-11.10.3 | US-11.10 §OQ | Ties in forced-choice scoring: count as wrong or drop? | NON-BLOCKING | Count as wrong (conservative, matches Twin-2K-500 §4.3). |

---

## §8 Scrum-Master Monitoring Checkpoints

**Wave 0 → Wave 1 gate:** All three of US-11.1, US-11.2, US-11.3 must show
commits in `git log sprint-11-sota-twin ^8528cc55 --oneline` before Wave 1
is dispatched.

**Wave 1 → Wave 2 gate:** US-11.4, US-11.5, AND US-11.6 must all have
committed. US-11.6 is the critical-path gating item.

**Wave 2 internal sequencing:** US-11.7 must complete its full quality gate
sequence (commit + verify + critic + aspect-panel) before US-11.8 is
dispatched.

**Wave 2 → Wave 3 gate:** US-11.7 and US-11.8 both committed. OQ-11.9.3
and OQ-11.10.1/2 resolved by Seth.

**Sprint close gate (from stories.md Sprint success metric):**
1. `human ml twin-eval --protocol yntp` on 30-prompt fixture reports
   `delta_nll > 0` with `pad_rate == 0` for DPOP+DoRA adapter.
2. `scripts/pareto_picker.py` classifies best Wave-1-trained adapter
   as PROMOTE or DEFER (not REJECT).
3. Sprint 8 broken adapter (iter-200, pad=80%) FAILS YNTP (AC-11.6.3) and
   4-stage gate (AC-11.7.3). Both regression guards committed.
4. US-11.3 early-stopping rule reproduces Sprint 8 iter-60 trajectory from
   fixture log without manual inspection (AC-11.3.2).
5. All P0 stories pass verifier + critic + aspect-panel and are committed
   to `sprint-11-sota-twin`.
6. Sprint-auditor sign-off with adversarial standard matching Sprint 7.

---

RESULT_scrum-master=PLAN_READY
