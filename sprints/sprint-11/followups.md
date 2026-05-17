# Sprint 11 — Follow-up tasks

Findings from critic + panel that don't block story close but should land in Sprint 12.

## P0 — must land before Sprint 11 close

### FU-11.6.a — AC-11.6.5 unmet: `pareto_picker.py --input-schema yntp` not wired
Source: US-11.6 critic HIGH-1.
The design spec requires `scripts/pareto_picker.py` to accept `--input-schema yntp` and map `delta_ll → fidelity_delta`, `pad_rate → pad_failure_rate`. The `86d886d3` commit did not touch `pareto_picker.py`. The round-trip test `test_twin_eval_integration.sh` does not exist. **This is binding because Wave 2 US-11.7 (Pareto gate) depends on this contract.** Status: re-opened US-11.6, dispatching implementer.

## P0 inline-resolved (this commit)

- **US-11.5 cherry-pick attribution:** `dpo_miner.c` was incidentally added to the `NOT_HU_ENABLE_ML` test-extra-modules block in `CMakeLists.txt:2219` during Wave 1 conflict resolution. The addition is a forward-compatible fix (the minimal build was latently missing it for `test_dpo_miner.c` linkage), but the attribution belongs to neither US-11.5 (ORPO) nor US-11.6. Recorded here for sprint-auditor context.
- **US-11.6 critic MED #1 — pre-commit guard not wired:** `.githooks/pre-commit` now calls `scripts/check_no_yntp_holdout_staged.sh` and exits non-zero on failure. D1 fixture policy is now enforced at staging time, not "if a contributor remembers."
- **US-11.6 critic MED #2 — mock log row-count silent drop:** `scripts/yntp_eval.py:327` now uses `!=` instead of `<` and rejects mismatched fixture+log lengths. The previous `<` allowed `zip` to silently truncate — exactly the silent-failure mode the gate is supposed to refuse.

## P1 — Sprint 12 (US-11.4 follow-ups)

### FU-11.4.a — AC-11.4.2/3/4 numerical-golden tests missing (HIGH)
Source: US-11.4 critic HIGH.
Story AC requires THREE tests with numerical correctness against the upstream `dpop` loss formula:
- AC-11.4.2: loss reduces to standard DPO when `delta = 0` AND chosen log-prob ≥ reference chosen log-prob.
- AC-11.4.3: penalty term fires when `log π(chosen) < log π_ref(chosen)` (the DCR signature).
- AC-11.4.4: penalty term is zero when chosen log-prob is above reference (healthy regime).
Delivered: only argv-shape tests in `tests/test_dpop.py`. The design doc designates `tests/test_dpop_loss.py` with mlx-runtime golden fixtures (skip-on-no-mlx guard). Fix: add `tests/test_dpop_loss.py` per design §3.2.

### FU-11.4.b — `getattr` bypasses argparse `choices=` guard (MED)
Source: US-11.4 critic MED.
`scripts/finetune-gemma.py:540` uses `getattr(args, "dpo_cpo_loss_type", "sigmoid")` which allows callers constructing a Namespace directly (e.g. `run_speculative_draft_training`, `run_train_all`) to pass an unvalidated string. The opaque mlx-lm-lora error is worse than an early `ValueError`. Fix: add `VALID_LOSS_TYPES = frozenset({"sigmoid","dpop","ipo","cpo"})` guard at line 540.

### FU-11.4.c — Subprocess test CI brittleness (MED)
Source: US-11.4 critic MED.
`tests/test_dpop.py:test_argparse_exposes_dpop_flags` spawns a real `subprocess.run` against `scripts/finetune-gemma.py`. Will fail in CI environments where import-time side effects break without optional deps. Fix: replace with direct `argparse.ArgumentParser` reconstruction, OR `pytest.mark.integration` guard.

### FU-11.4.d — Flag naming AC drift comment (LOW)
Source: US-11.4 critic LOW.
Story AC says `--variant dpop --dpop-lambda`; implementation delivers `--dpo-cpo-loss-type dpop --dpop-delta` (matches upstream mlx-lm-lora). Add comment in `tests/test_dpop.py` referencing US-11.4.md §1.2 to spare a future auditor the diff hunt.

### FU-11.4.e — `tests/fixtures/dpop_golden.json` not yet created (LOW)
Source: US-11.4 critic incomplete check (followup landed via re-prompt).
Design doc §2 file plan designates `tests/fixtures/dpop_golden.json` as the human-readable derivation anchor for FU-11.4.a's numerical tests. Lands together with `test_dpop_loss.py`.

## P1 — Sprint 12 (US-11.5 follow-ups)

### FU-11.5.a — Production ORPO `train_step` returns NOT_SUPPORTED (HIGH per design)
Source: US-11.5 design doc §7 step 10; raised by US-11.5 critic MED #2 as missing-from-followups.
`src/ml/rl_trainer_orpo.c` `train_step` is mocked under `HU_IS_TEST` and returns `HU_ERR_NOT_SUPPORTED` in production builds because the full forward-pass + tokenizer wiring is not yet shipped (symmetric to FU-7.10.a for SimPO). Sprint 12 must wire the production path so `human ml rl-train --algorithm orpo` actually trains.

### FU-11.5.b — `orpo_log1mexp` does not validate positive logp (MED)
Source: US-11.5 critic MED.
`src/ml/rl_trainer_orpo.c:68-75` silently clamps `logp > 0` to `-1e-12`. Positive log-probabilities are physically impossible; a caller upstream sign-error would produce a finite-but-wrong loss with no diagnostic. Fix: `assert(logp <= 0.0)` under `HU_IS_TEST` OR return `HU_ERR_INVALID_ARGUMENT` from `orpo_compute_loss` when inputs violate.

### FU-11.5.c — `--lambda-orpo` parser style inconsistency (LOW)
Source: US-11.5 critic LOW.
`src/ml/cli.c:~3183` uses `i++; continue;` inside the parser loop. Correct given `get_opt` stride, but diverges from SimPO `--beta` parser pattern. Fix: add comment OR refactor to match SimPO.

## P1 — Sprint 12 (US-11.6 follow-ups)

### FU-11.6.b — Real MLX path is `NotImplementedError` (HIGH, scope-honest)
Source: US-11.6 critic HIGH-2.
`scripts/yntp_eval.py:_real_compute_logprob` raises `NotImplementedError` unconditionally — the gate has never run end-to-end against a real Gemma-4 adapter. Mock fixtures validate gate logic but not MLX behavior. **Required before sprint close**: Seth runs `--self-test` mode on his machine and commits the golden output to `sprints/sprint-11/evidence/US-11.6/` per design doc §4 Risk 1 mitigation.

### FU-11.6.c — `delta_ll <= 0` gate semantics undocumented (LOW)
Source: US-11.6 critic LOW.
`scripts/yntp_eval.py:decide_gate` uses `<=` so the honest base-vs-base baseline (delta == 0) correctly FAILs. Add a one-line comment `# exact zero is not improvement — intentional` at the call site to spare future readers.

## P1 — Sprint 12

### FU-11.2.a — `--resume` + `--train-type dora` preflight guard
Source: US-11.2 critic MED.
The `run_sft` resume path silently proceeds with a DoRA training run on top of a LoRA-shaped `adapters.safetensors`. mlx-lm-lora will surface an error at runtime, not at argparse time. Add a preflight check that warns when `--resume` is combined with a different `train_type` than the prior adapter's metadata records.

### FU-11.2.b — AC-11.2.4 doesn't actually verify DoRA loadability
Source: US-11.2 critic MED.
`check-lora-baseline.sh` runs `human ml lora-baseline` against a persona fixture — does not load the safetensors adapter at all. So AC-11.2.4 ("DoRA adapter passes the gate") is vacuously satisfied. Need a separate test that loads the DoRA adapter via mlx_lm and confirms shape.

### FU-11.3.a — `_on_fire` silently swallows OSError on sentinel/log write
Source: US-11.3 critic HIGH-1.
If the adapter directory is read-only or filesystem full, the stop is never durably recorded but `_terminate` still fires. Downstream Pareto picker / US-11.6 reads no `.early_stop` and treats the run as un-stopped. Fix: propagate OSError OR write to stderr + return non-zero from `run_with_early_stop`.

### FU-11.3.b — `proc.stdout is None` silent exit
Source: US-11.3 critic HIGH-2.
If the subprocess is created but `stdout` is somehow falsy (broken mock or future Popen subclass), wrapper silently exits with `fired=None` without parsing any line. Subprocess not terminated. Fix: raise RuntimeError if `proc.stdout is None` immediately after `_popen()` returns.

### FU-11.3.c — Missing 20%-iter sanity assertion
Source: US-11.3 panel FLAG + critic MED. (Also documented in design Risk 2.)
If `mlx_lm_lora` ever reformats output, `parse_iter_line` silently returns None forever; detector never observes; no early stop ever fires. Add: after a configurable `parse_warn_after` number of lines with zero successful parses, write a warning to `out_stream` and optionally abort.

### FU-11.3.d — Asymmetric sentinel (w) vs event log (a) write modes
Source: US-11.3 critic MED.
On re-run of the same adapter dir, event log accumulates while sentinel holds only the latest. Downstream parsers may mis-count. Fix: document the asymmetry OR clear log at start of `run_with_early_stop`.

### FU-11.3.e — `assert fb is not None` in production firing path
Source: US-11.3 critic MED.
`assert` is a no-op under `python -O`. Replace with `if fb is None: raise RuntimeError(...)`.

### FU-11.3.f — Missing test for `run_train_all` / `run_speculative_draft_training` forwarding of `early_stopping_signal`
Source: US-11.3 critic LOW.
Add one test that asserts the synthesized Namespace correctly forwards the flag through these internal callers.

### FU-11.3.g — Design doc trailing-mean off-by-one
Source: US-11.3 critic LOW.
Design says window mean = 8.848, actual is 8.4776 (iter 60 is already in history by the time iter 65 is observed). Both thresholds trigger the breach so tests pass, but design doc is wrong. Add `pytest.approx` assertion on `trailing_mean` + `threshold` to lock the correct values.

## Cross-cutting risk for Wave 1 merge review

US-11.3 restructured `run_dpo`'s subprocess call into a branch (es_signal == "chosen_r" vs else). Both US-11.1 (env=child_env) and US-11.4 (DPOP) need to keep BOTH branches in sync. The Wave 1 merge MUST verify cmd construction, env, and cwd are identical in both branches.
