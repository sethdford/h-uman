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
| 1 | llama.cpp Metal inference | `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md` (TBA at phase start) | Phase 0 | 5–7 days |
| 2 | Real DPO + reaction wiring | `docs/plans/2026-05-11-rl-loop-phase-2-dpo-reactions.md` (TBA) | Phase 1 | 7–10 days |
| 3 | KTO + reward model | `docs/plans/2026-05-11-rl-loop-phase-3-kto-rm.md` (TBA) | Phases 0, 1 | 5–7 days |
| 4 | GRPO + multi-rollout | `docs/plans/2026-05-11-rl-loop-phase-4-grpo.md` (TBA) | Phases 1, 3 | 10–14 days (50% timeline padding per spec risk #5) |
| 5 | Eval gate + competitive harness | `docs/plans/2026-05-11-rl-loop-phase-5-eval-competitive.md` (TBA) | All prior | 7–10 days |
| 6 | E2E proof + demo | `docs/plans/2026-05-11-rl-loop-phase-6-proof.md` (TBA) | All prior | 3–5 days |

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
| 0 | ✅ 2026-05-11 | ⏸ pending user kickoff | — | — |
| 1 | ⏸ TBA at phase start | — | — | — |
| 2 | ⏸ TBA at phase start | — | — | — |
| 3 | ⏸ TBA at phase start | — | — | — |
| 4 | ⏸ TBA at phase start | — | — | — |
| 5 | ⏸ TBA at phase start | — | — | — |
| 6 | ⏸ TBA at phase start | — | — | — |
