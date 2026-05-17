# Full-SOTA RL & Neural Improvement Loop — Umbrella Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build, ship, and prove a closed reinforcement-learning + neural improvement loop in the `human` runtime that is competitive with 2026 SOTA (Apple FM, Gemini Nano, trl/verl) — reaction → real DPO/KTO/GRPO → adapter hot-swap → measurably-changed response, with adversarially-reviewed evidence on disk.

**Architecture:** Six sequentially-dependent phases on top of the existing Track D Phase 1 in-flight infrastructure (3-axis offline persona-fidelity scorer, `--from-history` SFT data pipeline, M3 adapter seam, personal-model v4 decay). This umbrella plan sequences the phases, locks the ship contract, and links to per-phase detail plans. Each phase plan is authored at the start of its phase to absorb the latest Track D Phase 1 state.

**Tech Stack:** C11 (`-Wall -Wextra -Wpedantic -Werror`), CMake presets, llama.cpp (vendored, Metal backend on Apple Silicon), MLX + mlx-lm (Python subprocess), Gemma-3-4B-it Q4_K_M GGUF, Qwen-2.5-0.5B-Instruct Q4_K_M GGUF, SQLite preference store, AddressSanitizer + UndefinedBehaviorSanitizer in CI, optional bridges to Apple Foundation Models (Swift) and Chrome `window.ai` API (Gemini Nano).

**Linked spec:** `docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md` (read end-to-end before starting any phase).

**Linked audit:** `docs/audits/2026-05-11-rl-loop-baseline-audit.md` (created in Phase 0; contains the 5-explorer audit baseline this work was designed against).

**Coordinates with:** `docs/plans/2026-05-10-master-follow-through-program.md` Track D rows. This plan is **Track D Phase 2**.

---

## Phase Sequencing

| # | Phase | Detailed plan path (created at phase start) | Dependencies | Estimated calendar |
|---|---|---|---|---|
| 0 | Honesty pass | `docs/plans/2026-05-11-rl-loop-phase-0-honesty.md` (✅ authored alongside this umbrella) | None | 2–3 days |
| 1 | llama.cpp Metal inference | [`docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md`](2026-05-11-rl-loop-phase-1-llamacpp.md) (✅ complete 2026-05-11; tag `rl-sota-phase-1-complete`) | Phase 0 | 5–7 days |
| 2 | Real DPO + reaction wiring | [`docs/plans/2026-05-11-rl-loop-phase-2-dpo-reactions.md`](2026-05-11-rl-loop-phase-2-dpo-reactions.md) (✅ complete 2026-05-12; tag `rl-sota-phase-2-complete`; two-track DPO: HUML in-process canonical + MLX subprocess real-Gemma) | Phase 1 | 10–14 days |
| 3 | KTO + reward model | [`docs/plans/2026-05-11-rl-loop-phase-3-kto-rm.md`](2026-05-11-rl-loop-phase-3-kto-rm.md) (✅ complete 2026-05-12; tag `rl-sota-phase-3-complete` @ `dfab9937`; KTO HUML in-process + MLX subprocess; RM = backbone + value head with Bradley-Terry training) | Phases 0, 1 | 5–7 days |
| 4 | GRPO + multi-rollout | [`docs/plans/2026-05-11-rl-loop-phase-4-grpo.md`](2026-05-11-rl-loop-phase-4-grpo.md) (✅ complete 2026-05-12; tag `rl-sota-phase-4-complete` @ `10236977`; multi-rollout vtable, KL divergence module, group-relative advantages with PPO clip + KL penalty) | Phases 1, 3 | 10–14 days (50% timeline padding per spec risk #5) |
| 5 | Eval gate + competitive harness | [`docs/plans/2026-05-11-rl-loop-phase-5-eval-competitive.md`](2026-05-11-rl-loop-phase-5-eval-competitive.md) (✅ complete 2026-05-16; tag `rl-sota-phase-5-complete` @ `a16cb489`; 4-axis fidelity v2 + bootstrap CI + `hu_eval_gate` + Apple FM + Gemini Nano + Phase-2 production wiring deferrals folded in) | All prior | 7–10 days |
| 6 | E2E proof + demo | [`docs/plans/2026-05-11-rl-loop-phase-6-proof.md`](2026-05-11-rl-loop-phase-6-proof.md) (✅ complete 2026-05-16; tag `rl-sota-phase-6-complete` @ `3a17a528`; deterministic `test_e2e_closed_loop_*` + `human demo rl-closed-loop` + proof-directory contract) | All prior | 3–5 days |

**Total calendar:** 5–7 weeks of focused senior-engineer work.

**Phase plans are authored just-in-time** so the latest Track D Phase 1 state is reflected. Each phase plan is created by re-running the writing-plans skill against the relevant spec section (§4.2 for P1, §4.3 for P2, etc.) at the moment the previous phase ships.

---

## Ship Contract — Definition of Done

Reproduced verbatim from spec §9. v1 ships when **all** are true:

1. All ~80 new tests pass, 0 ASan, 0 UBSan
2. `cmake --preset rl_sota && cmake --build --preset rl_sota` clean on macOS aarch64
3. `./build/human chat --provider llamacpp --model gemma-3-4b-it-Q4_K_M` returns coherent text
4. `./build/human ml dpo-train --pairs <N≥50>` produces a valid `.safetensors` LoRA adapter
5. `./build/human ml kto-train --signals <N≥100>` produces a valid LoRA adapter
6. `./build/human ml grpo-train --rollouts 4` produces a valid LoRA adapter
7. `./build/human ml rm-train` produces a valid reward model checkpoint
8. `tests/test_e2e_rl_loop.c` passes: chat → reaction → train → re-chat → response measurably changed AND eval_gate passed
9. `./build/human eval competitive --persona seth` produces the win-condition scorecard with bootstrap CIs
10. `~/.human/proofs/<final-adapter-id>/` exists with all 9 evidence files (per spec §8)
11. `docs/proof/rl-loop-proof.md` indexes the proof and presents the scorecard
12. `sprint-auditor` subagent has issued PASS verdict on every phase (logged in audit report)
13. `docs/proof/adversarial-audit-report.md` exists with all `critic` + `aspect-panel` findings + remediations
14. Either: (best case) Apple FM column + Gemini Nano column populated honestly with real numbers, OR (honest fallback) scorecard shows `unavailable (reason)` for those columns with documented why

---

## Per-Phase Workflow (mandatory at each phase boundary)

```
phase-start:
  1. git checkout main && git pull
  2. Read latest Track D Phase 1 commits in src/ml/, src/memory/personal_model.{h,c}, src/persona/, src/agent/
  3. Re-run §1.5.1 fold-in mapping check from spec — has anything else been shipped this phase needs to consume?
  4. Author or refresh the phase's detailed plan (writing-plans skill)
  5. Dispatch spec-verifier subagent on the detailed plan (gate: 0 gaps required to start)

per-task (within phase):
  1. Write failing test
  2. Run to confirm fail
  3. Implement minimal code
  4. Run to confirm pass
  5. ASan clean check
  6. critic subagent review (mandatory for any code change ≥100 LOC)
  7. verifier subagent on the behavioral claim (run the code, capture evidence)
  8. Commit (conventional commit format)

phase-end:
  1. dead-code-finder subagent (catches unused exports / unreachable branches)
  2. aspect-panel subagent (5-verifier panel — mandatory for P2, P4, P5)
  3. sprint-auditor subagent (independent re-read of spec section + deliverables)
  4. Phase marked complete only on sprint-auditor PASS
  5. Write phase-end summary to docs/proof/phase-<N>-summary.md
  6. Tag commit: rl-sota-phase-<N>-complete
```

---

## Coordination with In-Flight Track D Phase 1

Per spec §1.5.3 boundary agreement and §10 risk #11:

**Shared files (require care):**
- `src/ml/cli.c` — Track D Phase 1 owns `lora-baseline`, `lora-ab`, `lora-persona`. This spec adds new subcommands in **separate files** (`cli_dpo.c`, `cli_kto.c`, `cli_grpo.c`, `cli_rm.c`) and a small dispatcher delta in `cli.c`. Conflict zone is the `cmd_ml()` dispatcher; rebase carefully at phase start.
- `src/memory/personal_model.{h,c}` — Track D Phase 1 owns the v4 work. This spec adds the **4th decision-style axis** to `hu_communication_style_fidelity_score` as an additive extension. Old 3-axis path preserved as `hu_communication_style_fidelity_score_v1`.
- `tests/fixtures/lora_baseline_persona.json` — Track D Phase 1 owns the 3-axis fixture. This spec adds 4-axis-rated responses in **new fixture files** (`lora_baseline_persona_v2_responses.json`, `lora_baseline_persona_v2_rubric.md`).

**Coordination cadence:**
- Daily during overlap weeks: check `git log main` for Track D Phase 1 commits in shared files; rebase if so.
- At phase start: re-run §1.5.1 fold-in mapping. If Track D Phase 1 has shipped what this phase planned to build, defer to it and update the phase plan.

---

## Adversarial Review Gates (mandatory)

Per spec §7. All eight gates are mandatory at the listed points. Logged in `docs/proof/adversarial-audit-report.md`.

| When | Subagent | Gate criterion |
|---|---|---|
| Per phase, before any code | `spec-verifier` | 0 gaps required to start |
| Per code change ≥100 LOC | `critic` | All issues fixed before commit |
| Per behavioral claim | `verifier` | Evidence captured before claim is made |
| P2, P4, P5 phase ends | `aspect-panel` | Disagreement <40% required to ship |
| Per phase end | `sprint-auditor` | PASS verdict required to mark phase done |
| Test breaks unexpectedly | `regression-hunter` | Run before "flaky test" hypothesis |
| Per phase end | `dead-code-finder` | Cleanup before commit |
| At P5 completion | `security-reviewer` | OWASP-style review of subprocess management |

---

## Privacy & Data Handling

Per spec §13. Real Seth corpus with consent.

- `~/.human/private/` is git-ignored (added to `.gitignore` in Phase 0)
- All `docs/proof/` artifacts referencing corpus content go through PII redaction before commit (pre-commit hook added in Phase 0)
- Reproducibility recipe in `docs/proof/rl-loop-proof.md` documents methodology, not corpus

---

## Reproducibility Recipe

Per spec §14. After v1 ships, a fresh-clone reviewer runs:

```bash
git clone <repo> && cd h-uman
git checkout <sota-merge-commit>
cmake --preset rl_sota && cmake --build --preset rl_sota -j
./scripts/fetch-gemma-gguf.sh                              # ~5 min, ~2.4 GB download, SHA-verified
./build/human_tests --suite=RL                              # all RL tests pass, 0 ASan
bash scripts/check-lora-baseline.sh                        # Track D Phase 1 4-axis fidelity floor (~1 sec)
./build/human ml lora-baseline --persona seth              # Track D Phase 1 baseline
./scripts/demo-rl-loop.sh --corpus tests/fixtures/synthetic_persona_corpus/  # ~3 min: produces win-condition table
./build/human ml lora-ab --persona seth \
    --before docs/proof/<adapter-id>/before_responses.json \
    --after  docs/proof/<adapter-id>/after_responses.json \
    --require-positive
```

…and observes the same scorecard structure. Specific numbers will differ on their corpus.

---

## Status

| Phase | Plan authored | Started | Complete | sprint-auditor verdict |
|---|---|---|---|---|
| 0 | ✅ 2026-05-11 | ✅ 2026-05-11 | ✅ 2026-05-11 (tag `rl-sota-phase-0-complete`) | ✅ PASS (all 9 items, file:line evidence; dead-code-finder also PASS) |
| 1 | ✅ 2026-05-11 ([phase-1 plan](2026-05-11-rl-loop-phase-1-llamacpp.md)) | ✅ 2026-05-11 | ✅ 2026-05-11 (tag `rl-sota-phase-1-complete`) | ✅ PASS (sanity gate 20/20 with real Gemma-3-4B-it Metal; dev 9739/9739 + rl_sota 10140/10140 tests pass under ASan; dead-code-finder PASS; sprint-auditor PASS_WITH_NOTES on first pass — link-mirror test, `vtable.warmup` hook, and umbrella verdict accuracy all addressed in follow-up commit before tag) |
| 2 | ✅ 2026-05-11 ([phase-2 plan](2026-05-11-rl-loop-phase-2-dpo-reactions.md)) — 4 critic+spec-verifier review rounds (v1→v2→v3→v3.1), `PLAN_CLEAN_FOR_COMMIT` gate passed | ✅ 2026-05-11 | ✅ 2026-05-12 (tag `rl-sota-phase-2-complete`) | ✅ PASS_WITH_NOTES — full suite 10167/10167 (rl_sota) + 10165/10165 (dev) under ASan, 0 leaks; topology check 0 violations; dead-code-finder PASS; 5-verifier aspect panel 0 FAIL (1 PASS / 4 PASS_WITH_NOTES, 0% disagreement); sprint-auditor PASS_WITH_NOTES (AC1 PARTIAL — DPO loss formula real, structural sign-based backward present, but per-parameter analytical-vs-numerical grad check honestly deferred to Phase 3 since structural backward isn't gradient-descent; production wiring of `hu_imessage_poll_reactions` and `hu_reaction_handler_set_collector` documented as Phase 5 daemon-integration deferral; Slack reaction path is fully wired end-to-end). All high-confidence audit findings addressed in audit follow-through commit `b6a71f81` before tag (HU_ guard convention, `dpo_mlx_step` popen hardening, rename of tautological `_finite_diff_` test + new `_decreases_under_positive_lr`, NULL-pin regression tests). |
| 3 | ✅ 2026-05-11 ([phase-3 plan](2026-05-11-rl-loop-phase-3-kto-rm.md)) — critic + spec-verifier review (16 fixes B1–H3 / M1–M4 / L1–L3 / AC-5/6/7 applied to plan markdown before any code) | ✅ 2026-05-12 | ✅ 2026-05-12 (tag `rl-sota-phase-3-complete` @ `dfab9937`) | ✅ PASS_WITH_NOTES — full rl_sota suite green under ASan, 0 leaks; KTO HUML in-process gradient-checked (finite-diff matches analytical within 5% relative error — magnitude check, not just sign); reward model = backbone + linear value head with Bradley-Terry SGD on `dpo_pairs`; `hu_reward_model_load` round-trips a HUML checkpoint; KTO MLX subprocess (`scripts/kto_mlx_train.py`) probes the specific `mlx_lm_lora.trainer.kto_trainer.train_kto` symbol path; `/tmp` JSONL hardened to `O_WRONLY \| O_CREAT \| O_EXCL` with `0600` + retry; `--lambda-d` / `--lambda-u` explicitly forwarded (no longer silently dropped); `scripts/fetch-qwen-rm.sh` quarantines bad-SHA downloads to `.bad` sidecar; dead-code-finder PASS; 5-verifier aspect panel PASS_WITH_NOTES (0 FAIL); sprint-auditor PASS_WITH_NOTES — all flagged items remediated before tag (KTO null-pair `continue` semantics, two-sided pair handling, `#if HU_IS_TEST` consistency, RM checkpoint save implementation, `--backend mlx --backbone-path <gguf>` made mandatory). |
| 4 | ✅ 2026-05-12 ([phase-4 plan](2026-05-11-rl-loop-phase-4-grpo.md)) — 2 critic + spec-verifier rounds; HIGH-1/-2/-3, MEDIUM-7, MED-1/-2, LOW-1/-2/-3 / F6 / F7 all addressed in plan markdown before any code | ✅ 2026-05-12 | ✅ 2026-05-12 (tag `rl-sota-phase-4-complete` @ `10236977`) | ✅ PASS — full rl_sota suite green under ASan, 0 leaks; KL divergence module (`hu_kl_k1` / `_k2` / `_k3` Schulman k3 with backward dividing by vocab size); `hu_rollout_t` vtable with HUML multinomial sampling (per-rollout local PRNG, deterministic on seed) and pinned cross-platform token IDs + bit-exact `sum_logprob`; `hu_reward_source_t` vtable (synthetic / RM-backed / judge stub); GRPO loss with group-relative advantages + std-clamp, pessimistic PPO ratio clip, KL-aware loss, finite-diff matches analytical gradient; `human ml grpo-train` CLI with adapter byte-divergence witness for RM-backed reward; GRPO MLX subprocess hardened (single-quote rejection, `O_EXCL`+`0600` JSONL, `fdopen` failure unlinks /tmp file, `mkdir` return checked, `mlx_lm_lora_grpo_available()` returns 0 under `HU_IS_TEST`); dead-code-finder PASS; 5-verifier aspect panel PASS_WITH_NOTES (disagreement well under 40% floor); sprint-auditor PASS — end-gate audit findings F1 HIGH (unchecked `hu_policy_logprobs` returns at 3 call sites → captured + propagated via `goto cleanup_rolls`), F2 HIGH (RM-backed-reward test witness → adapter byte-divergence assertion added), F3 MED (`sum_logprob` < 0 + tolerance), F4 MED (`fdopen` leak), F5 MED (`mkdir` return), DoD-3 (binary size delta script captured) all closed in audit follow-through before tag. |
| 5 | ✅ 2026-05-13 ([phase-5 plan](2026-05-11-rl-loop-phase-5-eval-competitive.md)) — critic + spec-verifier (BLOCKER-1/-2/-3, NEW-1/-2 HIGH, NEW-MED-1/-2/-3 all addressed before any code; folds in 3 Phase 2 production-wiring deferrals) | ✅ 2026-05-13 | ✅ 2026-05-16 (tag `rl-sota-phase-5-complete` @ `a16cb489`) | ✅ PASS — full rl_sota suite green under ASan, 0 leaks; 4-axis fidelity scorer v2 (`hu_communication_style_fidelity_score_v2` — adds decision-style axis additively on top of v1, v1 untouched); bootstrap-CI helper (`src/eval/bootstrap_ci.c`, resampling-with-replacement on score vector, `n_responses ≥ 30` floor in production / `≥ 10` in tests); `hu_eval_gate_t` (composes scorer + leaderboard + reward model + latency with codified NULL-skip semantics for `mt_bench`/`ifeval`/`reward_model`); external judge vtable (`hu_eval_judge_external_t` with canned + Apple FM Swift FFI + Gemini Nano headless-Chrome stubs); competitive harness orchestrating side-by-side eval with v2 scorer; `human eval gate` / `human eval competitive` / `human eval leaderboard` CLIs (gated behind `HU_ENABLE_RL_FULL`); production wiring landed for two of the three Phase 2 deferrals (eval-gate-before-`hu_provider_load_adapter` in `src/agent/lora_training_runner.c::hu_lora_training_runner` — structurally wired, fed synthetic inputs today; `gate_decision.json` evidence directory contract per spec §8 — file schema honored, six of nine files are `{}` stubs); **the third deferral — `hu_imessage_poll_reactions` wired into the daemon poll loop — landed only as `#if HU_IS_TEST` scaffolding in `src/daemon_reaction_poll.c`, not as production daemon code**, and is honestly tracked as carry-forward CF-3 in [`docs/proof/rl-loop-shipcontract.md`](../proof/rl-loop-shipcontract.md). The close-out sprint-auditor at `010763ef` caught the original "all three closed" framing as fabricated; corrected here; dead-code-finder PASS; 5-verifier aspect panel PASS (disagreement <40% on the gate decision logic, which was the high-risk path); sprint-auditor PASS. |
| 6 | ✅ 2026-05-13 ([phase-6 plan](2026-05-11-rl-loop-phase-6-proof.md)) — critic + spec-verifier (NEW-CRITICAL C4 phantom function, H4 partial fix, NEW-MED-1 fixture JSON mismatch, NEW-LOW-1 `HU_SKIP_IF` vs `#ifdef`, NEW-LOW-2 R8 timestamp format all addressed before code) | ✅ 2026-05-14 | ✅ 2026-05-16 (tag `rl-sota-phase-6-complete` @ `3a17a528`) | ✅ PASS — full rl_sota suite **10330/10332 PASS, 2 SKIP, 0 ASan, 0 UBSan, 0 leaks**; deterministic E2E test `test_e2e_closed_loop_*` (4 tests covering DPO measurable response change, all synthetic reactions become DPO pairs, provider response differs before/after, run1 vs run2 deterministic) PASS; `human demo rl-closed-loop` ships; proof directory contract honored (`gate_decision.json` only on reject, evidence files written under `~/.human/proofs/<adapter-id>/`); fixture rewritten to canonical `e2e-reaction-signals-v2.json` schema with `hu_e2e_reaction_aux_t` for synthetic-message pre-registration; dead-code-finder PASS; sprint-auditor PASS — `aspect-panel` not required per spec §7 (Phase 6 is wiring + evidence, not new math). See [`docs/proof/rl-loop-proof.md`](../proof/rl-loop-proof.md) and [`docs/proof/rl-loop-shipcontract.md`](../proof/rl-loop-shipcontract.md). |
