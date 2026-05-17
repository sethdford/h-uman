---
plan: docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md
auditor: group-9-adversarial-rl
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: PARTIAL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Design spec for the closed-loop RL & neural improvement loop: reaction → DPO/KTO/GRPO → adapter hot-swap → measurably-different response, locally on Apple Silicon with adversarial audit trail. Win condition is the scorecard in `docs/proof/rl-loop-proof.md` (persona fidelity ≥ +5% absolute, MT-Bench within 1%, IFEval within 2%, p95 latency ≤ +50ms).

## Key Claims (from the plan)
- Claim 1: 14-item Definition-of-Done — DPO/KTO/GRPO/RM CLIs all produce valid `.safetensors` adapters; `test_e2e_rl_loop.c` passes; `docs/proof/rl-loop-shipcontract.md` indexed.
- Claim 2: Adapter evidence dir `~/.human/proofs/<final-adapter-id>/` exists with 9 evidence files.
- Claim 3: All trainer code consumes Track D Phase 1 primitives (`hu_communication_style_fidelity_score`, `hu_personal_model_*`) without duplication.
- Claim 4: 6 phases (0/1/2/3/4/5/6) sequenced and each ends with a sprint-auditor verdict.

## Evidence

### Implemented? (code exists)
- `src/ml/dpo_real_huml.c`, `src/ml/dpo_real_mlx.c`, `src/ml/kto.c`, `src/ml/kto_mlx.c`, `src/ml/grpo.c`, `src/ml/grpo_mlx.c`, `src/ml/reward_model.c`, `src/ml/reward_model_mlx.c`, `src/ml/reference_model.c`, `src/ml/policy_logprobs.c`, `src/ml/kl_divergence.c`, `src/ml/molora.c` all exist on origin/main.
- Headers: `include/human/ml/{dpo_real,kto,grpo,reward_model,reference_model,policy_logprobs,rl_trainer,rollout,reward_source,value_head}.h`.
- CLIs: `src/ml/{cli_dpo,cli_kto,cli_grpo,cli_rm,cli_demo}.c`.
- Eval harness: `src/eval/{competitive_harness,bootstrap_ci,persona_rollout,leaderboard,eval_gate,eval_judge_external,turing_*}.c`.
- Demo: `scripts/demo-rl-loop.sh`, `human demo rl-closed-loop` CLI subcommand.

### Proven? (tests exist)
- `tests/test_e2e_rl_loop.c` — closed-loop E2E.
- `tests/test_dpo_real_{e2e,loss,mlx}.c`, `tests/test_kto_loss.c`, `tests/test_grpo_{huml,loss,mlx,e2e}.c`, `tests/test_reward_model_{train,inference}.c`.
- `tests/test_reaction_event.c`, `tests/test_reaction_handler_e2e.c`, `tests/test_imessage_reactions.c`, `tests/test_slack_reactions.c`.
- `tests/test_daemon_reaction_poll_{wiring,production}.c`, `tests/test_persona_rollout.c`, `tests/test_cli_eval_phase5.c`.
- `tests/fixtures/persona_rollout_prompts_20.txt`, `e2e_reaction_signals.json`, `synthetic_grpo_prompts.jsonl`.
- Ship Contract verdict: 10330/10332 PASS, 0 ASan, 0 UBSan, 0 leaks at tag `rl-sota-phase-6-complete`.

### Wired? (called in runtime path / dispatch)
- `validate-rl-sota.sh` runs the full RL preset suite; demo-rl-loop.sh runs the user-visible demo CLI.
- iMessage tapbacks + Slack reactji → `reaction_event.c` → `reaction_handler.c` → `dpo_pairs` SQLite.
- Daemon polling via `src/daemon_reaction_poll.c` integrated in `src/daemon.c:+43`.
- LoRA training runner wired to eval gate at `src/agent/lora_training_runner.c`.
- **Gap (PARTIAL):** No dedicated `rl_nightly.yml` GitHub workflow. The eval.yml weekly cron and validate-rl-sota.sh exist; nightly RL training is not automated in CI.

## Gaps
- Apple FM / Gemini Nano factories return `HU_ERR_NOT_SUPPORTED` (honest fallback per §14) — declared as PASS_WITH_NOTES.
- Sanity gate `PASS_BAR=18/20`, not 20/20 ceiling (DoD-3 PASS_WITH_NOTES).
- E2E "AND eval_gate passed" half satisfied by separate suite, not the 4 E2E tests themselves.
- No nightly CI job dedicated to RL training; validation is on-demand via `validate-rl-sota.sh`.

## Notes
Ship Contract verdict: **11 PASS + 3 PASS_WITH_NOTES = 14/14 structurally met** per `docs/proof/rl-loop-shipcontract.md`. Tag `rl-sota-phase-6-complete` (commit `3a17a528`). Phase D carry-forwards CF-1..CF-7 closed in commit `ebb56bf3`.
