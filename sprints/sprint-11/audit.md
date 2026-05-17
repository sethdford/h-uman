# Sprint 11 Audit — Adversarial Sprint-Auditor Report

**Auditor:** sprint-auditor (Phase 4)
**Date:** 2026-05-17
**Branch:** `sprint-11-sota-twin`
**Range:** `ea02b08e..aaa50bf4` (22 commits)
**Standard:** Strict-mode; favor FAIL over PASS when evidence is incomplete.

---

## Section A — Per-AC verdict

| Story | AC | Verdict | Evidence / Finding |
|---|---|---|---|
| US-11.1 | AC-11.1.1 | DELIVERED | `tests/test_dpo_pad_masking.py` mask-tensor assertion; passes |
| US-11.1 | AC-11.1.2 | DELIVERED | Numerical len-norm assertion (10-tok vs 50-tok) passes within 1e-5 |
| US-11.1 | AC-11.1.3 | DELIVERED | `pareto_picker.py` on fixture sweep; best ckpt not REJECT-on-pad |
| US-11.1 | AC-11.1.4 | DELIVERED | `test_pareto_pad_regression.py` 4/4 — Sprint 8 broken config still REJECT/DEFER |
| US-11.1 | AC-11.1.5 | DELIVERED | Mock seam holds; no real-load path |
| US-11.2 | AC-11.2.1 | DELIVERED | `test_dora_flag_propagated_to_mlx_cmd` argv assertion green |
| US-11.2 | AC-11.2.2 | DELIVERED | `test_default_train_type_is_dora` passes |
| US-11.2 | AC-11.2.3 | DELIVERED | `test_explicit_lora_flag_respected` passes |
| US-11.2 | AC-11.2.4 | PARTIAL | `check-lora-baseline.sh` exits 0 but doesn't load adapter (FU-11.2.b vacuously satisfied — review admits this) |
| US-11.2 | AC-11.2.5 | DELIVERED | `test_train_config_records_dora` |
| US-11.3 | AC-11.3.1 | DELIVERED | `test_chosen_r_plateau_break_fires` |
| US-11.3 | AC-11.3.2 | DELIVERED | `test_sprint8_trajectory_stops_at_iter65` |
| US-11.3 | AC-11.3.3 | DELIVERED | `test_stable_chosen_r_no_early_stop` |
| US-11.3 | AC-11.3.4 | DELIVERED | `test_early_stop_log_format` |
| US-11.3 | AC-11.3.5 | DELIVERED | Backward-compat assertion |
| US-11.4 | AC-11.4.1 | DELIVERED-WITH-DRIFT | Argv translated to `--dpo-cpo-loss-type dpop --delta 0.1`; AC wording `--variant dpop --dpop-lambda` exists only at the Python argparse surface (`--dpo-cpo-loss-type` and `--dpop-delta`). Design §1.2 documents the divergence. |
| US-11.4 | AC-11.4.2 | **NOT_DELIVERED** | No `tests/test_dpop_loss.py`, no `test_dpop_penalty_fires_on_dcr_condition`. Design §3.2 mandated this test — deferral is NOT pre-approved by design §1.4 (§1.4 only rejects custom loss, it REQUIRES the golden test). |
| US-11.4 | AC-11.4.3 | **NOT_DELIVERED** | No `test_dpop_penalty_zero_on_healthy_chosen`. Same as above. |
| US-11.4 | AC-11.4.4 | **NOT_DELIVERED** | No `test_sprint8_iter80_dcr_prevented_by_dpop`. Sprint 8 regression guard absent. |
| US-11.4 | AC-11.4.5 | DELIVERED | Existing DPO tests pass unmodified |
| US-11.5 | AC-11.5.1 | DELIVERED | `test_orpo_train_exits_0` |
| US-11.5 | AC-11.5.2 | DELIVERED | `test_orpo_loss_golden` analytical numerical fixture |
| US-11.5 | AC-11.5.3 | DELIVERED | `test_orpo_or_penalty_diminishes_at_high_log_prob` |
| US-11.5 | AC-11.5.4 | DELIVERED | SimPO 7/7 unmodified |
| US-11.5 | AC-11.5.5 | DELIVERED | `test_rl_train_unimplemented_algorithms` for grpo2 |
| US-11.5 | AC-11.5.6 | DELIVERED | Build clean, 10383/10383 |
| US-11.6 | AC-11.6.1 | DELIVERED | Schema test green |
| US-11.6 | AC-11.6.2 | DELIVERED | Good-adapter integration arm: Δ=+0.4351, pad=0% |
| US-11.6 | AC-11.6.3 | DELIVERED | Integration sh: Sprint 8 broken arm Δ=-2.5234, pad=100%, REJECT, exit 2. Fixture genuinely simulates DCR (`<pad>` tokens, adapter logp -41..-55 vs base -7..-12). |
| US-11.6 | AC-11.6.4 | DELIVERED | Mock seam holds |
| US-11.6 | AC-11.6.5 | DELIVERED | `ab34a488` wires `pareto_picker.py --input-schema yntp`; round-trip green |
| US-11.6 | AC-11.6.6 | PARTIAL | Synthetic-5 fixture in repo per D1; the 30-row "production" file lives only on Seth's machine. D1 explicitly hybrid; acceptable scope. Real MLX path is `NotImplementedError` (FU-11.6.b) — flagged below as a sprint-claim concern. |
| US-11.7 | AC-11.7.1 | DELIVERED | `test_ppl_floor_rejects_high_ppl` |
| US-11.7 | AC-11.7.2 | DELIVERED | `test_coherence_judge_rejects_pad_outputs` |
| US-11.7 | AC-11.7.3 | DELIVERED | `test_sprint8_iter200_rejected_by_gate` PASS; `test_stage1_short_circuits_stage2_not_invoked` asserts `result["stages"][1]["status"] == "skipped_due_to_short_circuit"` — this is per-stage JSON marker, NOT just final verdict. Re-ordering tripwire is genuine. |
| US-11.7 | AC-11.7.4 | DELIVERED | Stage 3 stub gated on `--stage3-stub`; dormant path returns NOT_IMPLEMENTED |
| US-11.7 | AC-11.7.5 | DELIVERED | `test_ensemble_min_aggregation` |
| US-11.7 | AC-11.7.6 | PARTIAL | Implemented as `--cascade`; AC says `--staged-gate`; alias deferred FU-11.7.h. Flag-name drift not explicitly approved in decisions.md. |
| US-11.8 | AC-11.8.1 | DELIVERED | `test_dual_adapter_artifacts_created` |
| US-11.8 | AC-11.8.2 | DELIVERED | `test_ema_update_on_promote`, matrix EMA |
| US-11.8 | AC-11.8.3 | DELIVERED | `test_quarantine_on_reject`; event emitted |
| US-11.8 | AC-11.8.4 | DELIVERED | `test_adapter_rollback_cli` |
| US-11.8 | AC-11.8.5 | PARTIAL | Test self-asserts JSON shape rather than exercising the writer path (FU-11.8.g admitted in review). |
| US-11.9 | AC-11.9.1 | DELIVERED | `test_summary_under_token_limit` |
| US-11.9 | AC-11.9.2 | DELIVERED | `test_popi_baseline_in_yntp_output` |
| US-11.9 | AC-11.9.3 | DELIVERED | Deterministic template; no LLM calls |
| US-11.9 | AC-11.9.4 | DELIVERED | `test_empty_corrections_returns_empty` |
| US-11.10 | AC-11.10.1 | DELIVERED | `test_twin2k_output_schema` |
| US-11.10 | AC-11.10.2 | DELIVERED | Numerical accuracy assertion |
| US-11.10 | AC-11.10.3 | PARTIAL | Synthetic-10 fixture only per D2 (`tests/fixtures/twin2k_synthetic_10q.jsonl`); the 50Q Seth fixture is explicitly deferred and approved by D2 |
| US-11.10 | AC-11.10.4 | DELIVERED | Mock seam holds |

**Tally:** 49 ACs total. DELIVERED: 41. DELIVERED-WITH-DRIFT: 1 (US-11.4.1 flag-name). PARTIAL (with deferral tracking): 5. NOT_DELIVERED: 3 (US-11.4.2, US-11.4.3, US-11.4.4). DROPPED: 0.

---

## Section B — Cross-cutting findings (briefing items 1–8)

| # | Item | Verdict | Evidence |
|---|---|---|---|
| 1 | US-11.4 §1.4 deferral claim | **FAIL** | Review claims "deferred per design §1.4 to FU-11.4.a". §1.4 is titled "Why no custom Python loss" — it rejects writing a Python wrapper but EXPLICITLY commits to the numerical golden tests in §3.2 with `tests/test_dpop_loss.py` (file plan line 86, test enumeration line 114). The deferral was NOT pre-approved by design. The review's wording is misleading — three ACs were dropped without explicit decisions.md sign-off. |
| 2 | US-11.5 flag-name divergence (`--variant dpop` vs `--dpo-cpo-loss-type`) | CONCERN | Design §1.2 documents the translation layer. Story AC text not amended in decisions.md. Design-level approval exists but stakeholder approval does not. Drift, named. |
| 3 | US-11.6 regression-guard fixture not a strawman | PASS | Ran `bash tests/test_twin_eval_integration.sh`. Broken arm: Δ=-2.5234, pad=100%, exit 2 REJECT. Fixture has real `<pad>` tokens, adapter logp -41..-55 vs base -7..-12. Genuine DCR simulation. |
| 4 | US-11.7 cascade re-ordering guard (per-stage marker, not just final verdict) | PASS | `test_stage1_short_circuits_stage2_not_invoked` asserts `result["stages"][1]["status"] == "skipped_due_to_short_circuit"`. Per-stage JSON marker — load-bearing assertion is correct. |
| 5 | US-11.7 CRITICAL fix — Stage 1 ABSTAIN bypass | PASS | `test_stage1_abstain_rejects_no_ppl_evidence` PASSES. Fixture with no PPL evidence + good coherence now correctly REJECTs. Bypass closed. |
| 6 | US-11.8 CRITICAL fix — KL gate stub bypass | PASS | All three required elements present in `8d7a502f`: (a) `out_is_stub` param on `hu_lora_compute_kl_drift` (`lora_ema.h:120`); (b) `lora_retrain_kl_gate_stubbed` event emitted (`lora_retrain_runner.c:791`); (c) `last_kl_drift_nats = -1.0` sentinel (`lora_retrain_runner.c:696, 788, 817`). |
| 7 | US-11.10 D2 compliance — no real `twin2k_seth_50q.jsonl` write | PASS | No commit writes/references that path. Only synthetic-10 in fixtures. |
| 8 | Worktree-merge-before-cleanup discipline | PASS | All 22 commits ea02b08e..HEAD land on `sprint-11-sota-twin`; all 10 stories have implementation commits + (where applicable) inline-fix commits. No missing implementer work. |

---

## Section C — Sprint-level claim verification

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| 1 | Replace gameable lexical fingerprint with held-out NLL gate (US-11.6) | **CONCERN** | The gate exists and the regression guard fires correctly — but `scripts/yntp_eval.py:_real_compute_logprob` raises `NotImplementedError`. The gate has never run end-to-end against a real Gemma-4 adapter. Sprint goal §4 calls for a publishable claim with empirical evidence on real data; that claim cannot yet be made. Review acknowledges as FU-11.6.b. The metric is structurally sound on the mock seam but unproven on real weights. |
| 2 | 4-stage Pareto cascade wraps the NLL gate (US-11.7) | PASS | 25/25 tests; CRITICAL ABSTAIN bypass closed; AC-11.7.3 regression guard green; per-stage skip marker enforces fail-fast contract. |
| 3 | Cascade wired into W14 cron with dual fast/slow LoRA (US-11.8) | **CONCERN** | KL stub bypass CRITICAL is observability-only (FU-11.8.f): a stubbed-torch deployment now logs "stubbed" but does NOT block promotion. The "0.5 nats threshold" is never enforced in production until real KL inference lands. Stage-1/2 of Pareto gate is still in force — promotions are not actually unsafe — but the layer the review claims as "VERIFIED" is observability-only. |
| 4 | Three SOTA losses shipped (DoRA / DPOP / ORPO) | **FAIL** | DoRA: argv-only (acceptable per design). ORPO: full numerical golden coverage (`test_orpo_loss_golden`). DPOP: argv-only with three numerical ACs (11.4.2/.3/.4) silently dropped against design §3.2's explicit requirement. The DPOP loss math itself is unverified — we are trusting upstream blindly. This is the **same anti-pattern that produced Sprint 8's pad-leakage**: shipping a loss-config story with no loss-numerical test. |
| 5 | Sprint 8 broken adapter caught by both gates (AC-11.6.3 + AC-11.7.3) | PASS | Both regression guards green; fixtures genuine. |

---

## Section D — Process discipline observations

- **Aspect-panel skipped after Wave 1 (Waves 2 & 3):** Defensible — both CRITICALs were caught by per-story critic, not the panel; no evidence the skip masked anything. But the deviation should be retro-recorded; without it the auditor has no record that the gate was waived deliberately rather than forgotten. Review §6 P4 names it; acceptable.
- **2 CRITICALs caught by critic (US-11.7, US-11.8):** Validates per-story critic. But both were the kind of subtle short-circuit / silent-stub bug an implementer should self-catch when their AC text says "regression guard" — the implementer's quality bar slipped. US-11.7 implementer wrote the ABSTAIN code; US-11.8 implementer wrote the KL plumbing. Both are signals for `tune-agent`.
- **Critic truncation in Wave 1 (3 incidents) recovered via SendMessage:** Functional but unreliable. The recovery worked here; on a worse day it would silently miss a finding. Recommendation in review §6 P1 (chunk large diffs) is the right fix; it should be a hookify rule, not a retro note.
- **US-11.5 branched from `main` instead of `sprint-11-sota-twin`:** ~1,875 LOC inherit-state noise. Cherry-pick resolved cleanly but reviewer cost was ~20 min. Should be hookified as a pre-implementer base-ref check.
- **DPOP scope-shortfall framing:** The review uses "DEFERRED-WITH-FU" to describe AC-11.4.2/.3/.4. This is the auditor's main concern: a story whose `Test seam` block in `stories.md` names `tests/test_dpop_loss.py` and whose design §3.2 enumerates exact golden values, then ships argv-only tests and a tracking followup, is a **PARTIAL** at best — and the spirit of AC-11.4.4 (Sprint 8 regression guard) is **MISSED**, not deferred. Decisions.md contains no D-entry approving this.

---

## Section E — Final verdict

**Story-level re-open recommendation:**

- **US-11.4 (DPOP loss head)**: must re-open in Sprint 12 with explicit stakeholder sign-off that AC-11.4.2/.3/.4 are deferred-with-tracking, OR land `tests/test_dpop_loss.py` before close. The current status conflates "design rejected custom loss" (§1.4) with "design deferred numerical golden tests" (false — §3.2 requires them). AC-11.4.4 is the Sprint 8 regression guard — its absence means we have **no numerical proof DPOP prevents the DCR failure mode it was added to prevent**. This is exactly the kind of corner-cut the auditor exists to catch.

The remaining sprint-level claim concerns (FU-11.6.b real MLX NLL; FU-11.8.f real KL inference) are scope-honest deferrals named in the review and constitute Sprint 12's primary goal. They are NOT auditor blockers — they are tracked, named, and don't violate the AC literal text. The DPOP gap IS an auditor blocker because three AC lines were marked SATISFIED-with-followup when they should have been NOT_DELIVERED.

**Mitigation accepted for PASS_WITH_NOTES (not FAIL):** the sprint did ship the structural fix — DPOP's `--delta` default-override (the highest-probability bug per the design's Risk 1) IS pinned by AC-11.4.1's argv test. The numerical-correctness gap is real but is a downstream verification problem, not a "this code doesn't work" problem. Sprint goal #3 (regression-guard) is met by US-11.6 and US-11.7 even without US-11.4.4. Net: 41/49 DELIVERED, 5 PARTIAL-with-deferral-tracking, 3 NOT_DELIVERED (all from one story, all numerical-test gaps). Below the FAIL bar (≥1 AC missed with no path forward); above the clean-PASS bar.

`RESULT_sprint-auditor=PASS_WITH_NOTES`

**Required Sprint 12 entry conditions:**
1. `tests/test_dpop_loss.py` with the three §3.2 golden tests OR explicit decisions.md D-entry approving the deferral
2. FU-11.6.b (real MLX NLL) — close before publishable claim
3. FU-11.8.f (real KL inference) — close before W14 promotion gate is trusted
4. Review wording correction: AC-11.4.2/.3/.4 must be re-stated as NOT_DELIVERED rather than DEFERRED-WITH-FU; the design §1.4 citation is incorrect
