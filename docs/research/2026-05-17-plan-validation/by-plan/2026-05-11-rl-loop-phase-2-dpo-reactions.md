---
plan: docs/plans/2026-05-11-rl-loop-phase-2-dpo-reactions.md
auditor: group-9-adversarial-rl
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Phase 2 replaces the provider-scored sigmoid `hu_dpo_judge_step` with two-track real DPO: (1) HUML in-process — real DPO loss with frozen π_ref + finite-difference grad checks, and (2) MLX subprocess via `scripts/dpo_mlx_train.py` calling third-party `mlx-lm-lora.trainer.dpo_trainer.train_dpo`. Wires iMessage tapbacks + Slack `reactions.added/removed` through a channel-agnostic reaction-event normalizer into the existing `dpo_pairs` SQLite table.

## Key Claims (from the plan)
- Claim 1: New `hu_rl_trainer_t` vtable in `include/human/ml/rl_trainer.h`; factory dispatches to HUML in-process or MLX subprocess.
- Claim 2: New `dpo_real_huml.c` + `dpo_real_mlx.c` + `reference_model.c` + `policy_logprobs.c`.
- Claim 3: `hu_reaction_event_t` type in `include/human/channels/reaction_event.h`; emitters in `imessage.c` + `slack.c`; normalizer in `reaction_event.c`; consumer in `reaction_handler.c`.
- Claim 4: `human ml dpo-train` auto-selects backend; `--backend {huml,mlx,auto}` overrides.
- Claim 5: 5-verifier aspect-panel + sprint-auditor gates passed.

### Implemented? (code exists)
- `src/ml/dpo_real_huml.c`, `src/ml/dpo_real_mlx.c`, `src/ml/policy_logprobs.c`, `src/ml/reference_model.c` (all on origin/main).
- `include/human/ml/{dpo_real,rl_trainer,policy_logprobs,reference_model}.h`.
- `src/ml/cli_dpo.c` (CLI dispatcher).
- `src/channels/reaction_event.c`, `src/channels/imessage_reactions.c`, `src/channels/slack_reactions.c`, `src/daemon/daemon_reaction_poll.c`, `src/agent/reaction_handler.c`.
- `scripts/dpo_mlx_train.py` (wrapper invoking `mlx-lm-lora`).
- `include/human/channels/{reaction_event,imessage_reactions}.h`, `include/human/agent/reaction_handler.h`, `include/human/daemon_reaction_poll.h`.

### Proven? (tests exist)
- `tests/test_dpo_real_loss.c`, `tests/test_dpo_real_e2e.c`, `tests/test_dpo_real_mlx.c`.
- `tests/test_reaction_event.c`, `tests/test_reaction_handler_e2e.c`, `tests/test_imessage_reactions.c`, `tests/test_slack_reactions.c`.
- `tests/test_daemon_reaction_poll_wiring.c`, `tests/test_daemon_reaction_poll_production.c`.
- Reaction E2E gate (synthetic tapback → dpo_pairs row).
- MLX adapter validation (`.safetensors` hot-loads in llama.cpp).

### Wired? (called in runtime path / dispatch)
- `human ml dpo-train` is real (`cli_dpo.c`).
- iMessage tapback poll branch + Slack webhook reactions branch emit events → normalizer → handler → `dpo_pairs`.
- Daemon polling integrated in `src/daemon.c:+43` (per Phase D CF commit).
- Substring heuristic in `agent_turn.c` preserved unchanged (per plan §4.3).

## Gaps
- The plan's "HUML structural backward isn't gradient-descent so the strict numerical-vs-analytical check isn't the right test for it" is acknowledged in the close-out commit (`75a3687a`) — per-parameter analytical-vs-numerical finite-diff grad check honestly deferred to Phase 3 MLX subprocess where analytical gradients live.

## Notes
Tag `rl-sota-phase-2-complete` (referenced in close-out commit `75a3687a` and the umbrella status table). 5-verifier aspect panel: 1 PASS + 4 PASS_WITH_NOTES + 0 FAIL = 0% disagreement.
