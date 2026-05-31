---
title: "RL & Neural Improvement Loop — Baseline Audit"
created: 2026-05-11
status: archived
scope: src/ml, src/agent, src/memory, src/eval, src/persona, src/providers, src/daemon
audit_method: 5-explorer concurrent review of committed main + Track D Phase 1 in-flight tree
authored_for: docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md
---

# RL & Neural Improvement Loop — Baseline Audit, May 11 2026

**Status:** Historical record. This audit was the baseline against which `docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md` was authored.

**Audit method:** 5-explorer concurrent review of `src/ml/`, `src/agent/`, `src/memory/`, `src/eval/`, `src/persona/`, `src/providers/`, `src/daemon.c`, plus committed `main` baseline + Track D Phase 1 in-flight uncommitted tree.

## Findings — what was real (committed `main`)

- Hand-derived backward + Muon+AdamW optimizer in `src/ml/gpt.c`, `train.c`, `muon_adamw.c`. Tests do real finite-difference grad checks.
- Real LoRA primitives in `src/ml/lora.c` (low-rank A/B, apply, backward, save/load).
- Real preference-data layer in `src/ml/dpo.c` (SQLite `dpo_pairs`, JSONL export).
- Real hot-swap adapter loading at daemon startup and post-train (`src/daemon.c:2465-2532`, `src/agent/lora_training_runner.c:90-93`).
- Real eval harness with LLM-as-judge, bootstrap regression detection, nightly W16 bench (`src/eval/eval.c`, `evaluation.yml`).

## Findings — what Track D Phase 1 (in-flight) was shipping

- Personal Model v4 with symmetric signal aging + decay + goals lifecycle + daemon hourly tick.
- Offline persona-fidelity scorer (3-axis `hu_communication_style_fidelity_score`) + A/B comparator + `human ml lora-baseline` and `human ml lora-ab` CLIs + `scripts/check-lora-baseline.sh` CI gate.
- Banks-from-history SFT data pipeline (`hu_persona_banks_extract_from_history` + `--from-history` + `--persist`).
- M3 adapter seam: kill switch (`hu_m3_adapter_should_disable`), chat-time hook (`hu_agent_m3_on_provider_success` in 11 sites), centralized caveat strings.
- Persona JSON example_banks round-trip (`hu_persona_creator_write`).

## Findings — what was still broken (re-verified against in-flight tree)

| # | Issue | Location | Severity |
|---|---|---|---|
| 1 | `hu_ml_train(alloc, &model, &optimizer, train_loader, NULL, &cfg.training, NULL, 0, &result)` — `vocab_size=0` and `token_bytes=NULL` | `src/ml/cli.c:190` (`hu_ml_cli_train`) | High — flagship subcommand silently no-ops |
| 2 | Same call shape | `src/ml/cli.c:2016` (`hu_ml_cli_train_feed_predictor`) | High |
| 3 | Same — `token_bytes=NULL` to `hu_ml_train` | `src/ml/experiment.c:300-302` (`run_single_experiment`) | High — defeats `val_bpb` |
| 4 | `hu_personal_model_save` is direct `fopen("wb")` with no temp file or rename | `src/memory/personal_model.c:1851` | High — crash during save corrupts state |
| 5 | `hu_dpo_train_step` is **not DPO** — calls external LLM, scores 0–100, aggregates synthetic loss. No policy log-probs, no reference model, no gradient on policy weights. | `src/ml/dpo.c` | High — name lies about behavior |
| 6 | `CLAUDE.md:53` claims `hu_personal_model_save` is "atomic-rename" save and that `hu_dpo_train_step` is DPO. Documentation drift. | `CLAUDE.md:53` | Medium — propagates the lie |
| 7 | `lora-persona` in-process backward passes raw logits where it should pass `softmax(logits) − one_hot(target)`, mathematically inconsistent with its documented NLL loss | `src/ml/cli.c` (lora-persona handler) | Medium — quietly trains the wrong objective; tracked in spec §1.5.2 / §2 background, fixed by spec Phase 2 |
| 8 | `m3_frontier_adapter.c` is a fixture file-format probe, not a frontier adapter | `src/ml/m3_frontier_adapter.c` | Low — caveats now centralized in Track D Phase 1; fixture nature unchanged but documented honestly |
| 9 | `llamacpp_chat_with_system` returns `HU_ERR_NOT_SUPPORTED` even when llama.cpp is linked | `src/providers/llamacpp.c:125-135` | High — fixed by spec Phase 1 |
| 10 | Channel reactions are outbound-only: inbound 👎 tapbacks/reactji do not reach `hu_dpo_record_from_feedback` | `src/channels/imessage_inbound.c`, `src/channels/slack_inbound.c` | Medium — fixed by spec Phase 2 |
| 11 | No reward model, no value head, no policy-gradient RL anywhere in the binary | binary-wide | High — added by spec Phases 2-4 |

## What this Phase 0 fixes (issues #1-6)

Issues 7-11 are deferred to subsequent phases per the spec. Phase 0 covers issues 1-6 only.

## What this Phase 0 explicitly does NOT touch

- Track D Phase 1 in-flight work (defer to its own track; this audit is a snapshot)
- Any new vtable surfaces (those land in Phases 2-5)
- Any provider integration (Phase 1)
- Any CI workflow additions (Phase 1+)
