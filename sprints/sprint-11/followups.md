# Sprint 11 — Follow-up tasks

Findings from critic + panel that don't block story close but should land in Sprint 12.

## P0 — must land before Sprint 11 close

### FU-11.6.a — AC-11.6.5 unmet: `pareto_picker.py --input-schema yntp` not wired
Source: US-11.6 critic HIGH-1.
The design spec requires `scripts/pareto_picker.py` to accept `--input-schema yntp` and map `delta_ll → fidelity_delta`, `pad_rate → pad_failure_rate`. The `86d886d3` commit did not touch `pareto_picker.py`. The round-trip test `test_twin_eval_integration.sh` does not exist. **This was binding because Wave 2 US-11.7 (Pareto gate) depends on this contract.** STATUS: RESOLVED — landed in commit `ab34a488`.

### FU-11.7.a (RESOLVED inline) — Stage 1 ABSTAIN bypass enabled PROMOTE on zero evidence
Source: US-11.7 critic CRITICAL #1.
When Stage 1 had no PPL source (no env mock, no `adapter_ppl`/`base_ppl` in the fixture), it returned `ABSTAIN`. The orchestrator's short-circuit at `stage_cascade.py:103` only fired on `REJECT`. The ABSTAIN cap at line 154 only checked Stage 2. Therefore a malformed fixture with no PPL fields but valid `coherence_scores ≥ 0.80` produced a final verdict of PROMOTE with Stage 1 having never observed the adapter — exactly the silent-failure mode this gate is supposed to refuse. **FIX (inline this commit):** Stage 1 ABSTAIN is now treated as a hard short-circuit equivalent to REJECT. New regression test `test_stage1_abstain_rejects_no_ppl_evidence` pins this contract.

### FU-11.7.b (RESOLVED inline) — `_CASCADE_ORDER` tuple was dead code (false Risk 1 mitigation)
Source: US-11.7 critic HIGH #2.
The design doc Risk 1 mitigation said "reordering requires editing the `_CASCADE_ORDER` tuple, which lights up the AC test." This was a lie — the tuple was never used; execution order was hardcoded imperatively. **FIX (inline this commit):** the tuple is removed; the in-code comment on the imperative call sequence now explicitly cites AC-11.7.3 as the load-bearing test.

### FU-11.7.c (RESOLVED inline) — `test_stage2_abstain_caps_at_defer` assertion was too loose
Source: US-11.7 critic HIGH #1.
The test asserted `verdict in ("DEFER", "REJECT")` which would silently accept a regression mapping Stage 2 ABSTAIN to REJECT. **FIX (inline this commit):** tightened to exact equality with DEFER + `exit_code == 1`, with a docstring explaining why DEFER is the contract (REJECT would mis-punish operator for a judge crash).

### FU-11.8.a (RESOLVED inline) — KL drift gate silently disabled in production
Source: US-11.8 critic CRITICAL #1.
`scripts/compute_kl_drift.py` returns `{"kl_nats": 0.0, "source": "stub"}` whenever torch is unavailable (every production deployment until M3 frontier bridge). The C runner's `lora_ema_parse_kl` ignored the `"source"` field; with `kl_tau_nats = 0.5`, a stubbed 0.0 always satisfied `kl > tau == false` and recorded `last_kl_drift_nats = 0.0` in the status JSON — indistinguishable from a real clean run. The KL safety gate was a guaranteed no-op in production. **FIX (inline this commit):** added `int *out_is_stub` parameter to `hu_lora_compute_kl_drift`; runner now sets `last_kl_drift_nats = -1.0` (gate disabled sentinel) and emits `lora_retrain_kl_gate_stubbed` event when stub detected, so dashboards/operators see the gate is not actually running. Promotion proceeds (Sprint 11 does not gate production on KL availability), but the silent backdoor is closed.

### FU-11.8.b (RESOLVED inline) — Cross-FS quarantine fallback missing `fflush + fsync`
Source: US-11.8 critic HIGH #1.
`retrain_quarantine_move` (and the identical pattern in `cli_adapter_rollback.c`) did `fread → fwrite → fclose(out) → unlink(src)` with no `fflush + fsync` between writes and close. A crash before the OS flushed dirty pages would leave the quarantine file truncated AND the source already unlinked, losing the fast adapter silently. Regressed the Phase 0 personal_model atomic-save lesson. **FIX (inline this commit):** explicit `fflush + fsync` on destination before unlinking source; on copy failure, partial destination is `unlink`'d for cleanup.

### FU-11.8.c (RESOLVED inline) — KL subprocess error silently allowed promotion
Source: US-11.8 critic HIGH #2.
The runner's KL block had a comment "On KL subprocess error, log but don't reject" but emitted no log. Combined with the stub issue (FU-11.8.a), there were two independent silent backdoor paths. **FIX (inline this commit):** on `kl_err != HU_OK`, set `last_kl_drift_nats = -1.0` and emit `lora_retrain_kl_gate_error` event with the error code.

### FU-11.8.d (RESOLVED inline) — Warm-path EMA write not fsync'd before rename
Source: US-11.8 critic HIGH #3.
`scripts/lora_ema.py` warm path did `save_file → os.rename` with no `os.fsync` between (despite the cold-start `_atomic_copy` doing it correctly). A crash after rename succeeds but before page flush would corrupt the next night's `slow.safetensors.v{N+1}`. **FIX (inline this commit):** explicit `os.fsync` on the tmp file descriptor before rename, matching the cold-start pattern.

### FU-11.8.e (RESOLVED inline) — `HU_ERR_PRECONDITION` header doc / `last_ema_alpha` no-op ternary
Source: US-11.8 critic MED #1 + LOW.
Header `lora_ema.h` documented `HU_ERR_PRECONDITION` return code that doesn't exist in `error.h` (impl returns `HU_ERR_TOOL_VALIDATION`). `last_ema_alpha = ema.out_was_cold_start ? alpha : alpha` was a no-op ternary. **FIX (inline this commit):** corrected header doc; cold-start now records `last_ema_alpha = 0.0` (no blend happened) so status JSON distinguishes cold-start from warm EMA.

## P0 inline-resolved (this commit)

- **US-11.5 cherry-pick attribution:** `dpo_miner.c` was incidentally added to the `NOT_HU_ENABLE_ML` test-extra-modules block in `CMakeLists.txt:2219` during Wave 1 conflict resolution. The addition is a forward-compatible fix (the minimal build was latently missing it for `test_dpo_miner.c` linkage), but the attribution belongs to neither US-11.5 (ORPO) nor US-11.6. Recorded here for sprint-auditor context.
- **US-11.6 critic MED #1 — pre-commit guard not wired:** `.githooks/pre-commit` now calls `scripts/check_no_yntp_holdout_staged.sh` and exits non-zero on failure. D1 fixture policy is now enforced at staging time, not "if a contributor remembers."
- **US-11.6 critic MED #2 — mock log row-count silent drop:** `scripts/yntp_eval.py:327` now uses `!=` instead of `<` and rejects mismatched fixture+log lengths. The previous `<` allowed `zip` to silently truncate — exactly the silent-failure mode the gate is supposed to refuse.

## P1 — Sprint 12 (US-11.4 follow-ups)

### FU-11.4.a — AC-11.4.2/3/4 numerical-golden tests **NOT_DELIVERED** (HIGH; sprint-auditor correction)
Source: US-11.4 critic HIGH, sprint-auditor Section B finding #1.

**CORRECTION from prior framing** (sprint-auditor Phase 4): I previously framed this as "deferred per design §1.4". That framing was **INCORRECT**. Design §1.4 is titled "Why no custom Python loss" — it argues against writing a Python wrapper for the loss; it does NOT defer the numerical tests. Design §3.2 (line 86, line 114) **EXPLICITLY MANDATES** `tests/test_dpop_loss.py` with three named golden tests:

- `test_dpop_penalty_fires_on_dcr_condition` (AC-11.4.3)
- `test_dpop_penalty_zero_on_healthy_chosen` (AC-11.4.4)
- `test_sprint8_iter80_dcr_prevented_by_dpop` (Sprint 8 regression guard, AC-11.4.4)

These tests were NOT WRITTEN. AC-11.4.2/3/4 are **NOT_DELIVERED**, not DEFERRED. There is no D-entry in `sprints/sprint-11/decisions.md` approving a deferral. The implementer's "out of scope per design §1.4" claim in their commit message + my review.md echo of it are **both wrong**.

**Sprint 12 entry condition**: land `tests/test_dpop_loss.py` per design §3.2 OR add an explicit D-entry to `decisions.md` retroactively approving the deferral with stakeholder (Seth) sign-off. The sprint-auditor lists this as condition #1 for clean Sprint 12 entry.

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

## P1 — Sprint 12 (US-11.7 follow-ups)

### FU-11.7.d — `pad_rate` silently 0.0 with array-form `HU_CASCADE_STAGE2_MOCK` (HIGH)
Source: US-11.7 critic HIGH #3.
`scripts/cascade_stages/stage2_coherence.py:61-62` accepts the array-form env mock (`HU_CASCADE_STAGE2_MOCK='[0.8, 0.8, 0.8]'`) but always returns `pads = []`, meaning the pad gate is silently disabled. A test using the bare array form will report `pad_rate=0.0` regardless of what the adapter actually outputs. Fix: either remove the array form (require the dict form `{"scores": [...], "pads": [...]}`) or have it default-set `pads` to all-False with a clear log warning at use.

### FU-11.7.e — `_CASCADE_ORDER` removal opens room for stronger order-pinning (MED)
Source: US-11.7 critic HIGH #2 alternate remediation.
The inline fix removes the dead tuple. A stronger long-term fix is to drive stage execution from a single dispatch dict (`STAGES = {"stage1_ppl": _STAGE1.run, ...}`) and iterate. That would make reordering truly require editing the dict structure, not the imperative sequence. Deferred to Sprint 12 as a defense-in-depth follow-up.

### FU-11.7.f — Stage 1 short-circuit assertion uses JSON marker, not call counter (MED)
Source: US-11.7 critic MED #1.
`test_stage1_short_circuits_stage2_not_invoked` checks `stages[1]["status"] == "skipped_due_to_short_circuit"`. A buggy implementation could call `stage2_coherence.run()` and then replace the result with `_skipped_stage(...)`, and the current test would pass. Fix: use `unittest.mock.patch` on `_STAGE2.run` and assert `call_count == 0`.

### FU-11.7.g — `check-lora-ab.sh --cascade` mktemp leak (MED)
Source: US-11.7 critic MED #2.
`check-lora-ab.sh:88` does `$(mktemp -t ...).json`, appending `.json` to the mktemp output. The trap removes the `.json` path; the underlying mktemp-created file leaks in `/var/folders/...`. Fix: `CASCADE_TMP=$(mktemp -t human-cascade-XXXXXX); mv "$CASCADE_TMP" "${CASCADE_TMP}.json"; CASCADE_TMP="${CASCADE_TMP}.json"`.

### FU-11.7.h — `--cascade` vs `--staged-gate` AC drift (MED)
Source: US-11.7 critic MED #3.
Story AC-11.7.6 names `--staged-gate`; design doc §2 and implementation use `--cascade`. Currently the shell test hardcodes `--cascade`. If a future auditor compares AC text against the test, they'll see a contradiction. Fix: add an alias `--staged-gate` → `--cascade` in `check-lora-ab.sh` argparse, OR rename to `--staged-gate` and update the test.

### FU-11.7.i — `_classify_score` coherence threshold asymmetry undocumented (MED, cross-agent)
Source: US-11.7 critic cross-agent regression risk.
`pareto_picker._classify_score` uses coherence PROMOTE=0.80 (inline literal) while `stage2_coherence.py` uses REJECT=0.70. The asymmetry is intentional (creates a DEFER band 0.70-0.80) but undocumented. A reader "fixing" the inconsistency could collapse the DEFER band. Fix: comment at `_classify_score` explaining the asymmetry.

### FU-11.7.j — `details["source"]` could emit `fixture:None` (LOW)
Source: US-11.7 critic LOW.
`scripts/cascade_stages/stage2_coherence.py:186-188` constructs source string from caller-supplied `fixture_path` without None check. Not reachable today but a refactor could expose it. Fix: assign `source` at resolution time in `_call_judge` and thread through.

## P1 — Sprint 12 (US-11.8 follow-ups)

### FU-11.8.f — Real KL drift inference (MED, blocks effective gate enforcement)
Source: US-11.8 critic CRITICAL #1 (long-term fix).
`scripts/compute_kl_drift.py` currently returns `source: "stub"` whenever torch is unavailable, and the C runner accepts that as "gate disabled, proceed." This makes the entire KL gate observability-only until real KL inference lands. Sprint 12 must implement real `KL(base || candidate)` computation against the 200-prompt probe set so production deployments actually enforce the `0.5 nats` tau.

### FU-11.8.g — AC-11.8.5 status test self-asserts, doesn't exercise `hu_w14_scheduler_status_save` (MED)
Source: US-11.8 critic MED #2.
The status JSON test in `tests/test_w14_dual_lora.c` constructs the expected JSON manually and asserts `strstr` on itself — verifies nothing about the actual writer in `world_model_bridge.c`. A `%.4f` format change, field rename, or escaping bug would silently break the writer while all tests pass. Fix: drive `hu_w14_scheduler_status_save` with a populated context, read output, assert field values.

### FU-11.8.h — `human ml adapter-rollback` has no PID lock (MED)
Source: US-11.8 critic MED #3.
Two concurrent `human ml adapter-rollback` invocations race on the same `slow.safetensors.v{N}` target. The second `rename` fails, the cross-FS fallback opens a non-existent path, `in == NULL` lacks a guard, code proceeds to `fclose(NULL)` — undefined behavior. The W14 cron itself holds a PID lock but the rollback CLI is a separate process. Fix: `flock` on `slow_dir` directory fd at entry; add `if (!in) return HU_ERR_IO;` guard.

### FU-11.8.i — Cross-merge risk note (Sprint 12 hygiene)
Source: US-11.8 critic cross-agent.
`lora_retrain_runner.{c,h}` was modified by both US-11.7 (cascade_script field) and US-11.8 (dual-lora fields). Future cherry-picks or merges that touch `hu_lora_retrain_ctx_t` must reconcile both ancestors. Sprint 12 should normalize the struct layout if both stories grow further.

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
