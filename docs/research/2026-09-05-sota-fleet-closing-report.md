---
title: SOTA fleet closing report — contracts C1–C7 and what production runs now (2026-09-02 → 09-05)
date: 2026-09-05
status: closed
---

# SOTA fleet closing report (2026-09-02 → 2026-09-05)

Fleet spawned from `docs/research/2026-09-02-august-2026-sota-gap-analysis.md` to give every ranked gap a
measured number rather than an assumption. Seven contracts, one coordinator, critic and verifier passes
per closure, every merge preceded by the coordinator re-running the tests in the agent's own worktree.
Numbers below are the ones in the committed result files; nothing here is estimated.

## Contract outcomes

| # | Contract | Result | Measured | State in prod |
|---|---|---|---|---|
| C1 | Semantic recall SHADOW→LIVE over-reliance gate | First paired run **HOLD** (EI 4.45→4.13, 9/40 empty LIVE replies). After byte budget (PR #375) + content filter: 40/40 paired, 0 empties, composite 0.919→0.908, EI 4.275→4.175 → **PROMOTE** | `semantic-live-gate-2026-09-03-content-filter.json` | **LIVE since 2026-09-03 18:15** (daemon a28d7c9b0); live index purged of 576 `experience:` rows; weekly re-check job in place |
| C2 | Reconstructive retrieval | **Gate FAILED**: 0.667 vs 1.000 plain hybrid (LongMemEval-S n=30). Ablation: no single stage; disabling neighbour expansion recovers most (0.833); coverage-first scene-select changed nothing. Confound: fallback path wrote content into the result key — fixed by a follow-up chip; re-measure pending | `memory-benchmarks-c2.json`, `memory-benchmarks-c2-ablation.json` | opt-in only (`--hybrid`), not on the read path |
| C3 | Agent-authored facts with provenance | Shipped; recall 12/12 (self-selected upper bound), precision 0.60→0.71 with the courtesy filter. **Critic found the call site unreachable in prod** (behind `reaction_collection.enabled`, which is off); fixed with a router-path test that fails on the old code | `agent-facts-results.json` | `HU_AGENT_FACTS=shadow` since 09-03 04:00 |
| C4 | Classifier + LLM-judge nightly tiers | Shipped as `nightly-watchdog.sh` jobs. LUAR (n≈36): ceiling 0.70–0.71, twin 0.625–0.633, floor 0.62–0.63 — twin at or below the other-humans floor. Gemini inverse-Turing judge: 09-02 acc 0.667 / AUC 0.651 (adapter inert); **09-04 acc 0.324 / AUC 0.196 with the adapter bound** — the judge picked the model as Seth more often than Seth (n=37, one run) | `~/.human/logs/authorship-gap-*.json`, `llm-judge-*.json` | nightly |
| C5 | When-to-speak MIR/FIR + reply-delay model | MIR 0.613 / FIR 0.670 on the fallback source (inflated). Delay model **lost to a global median** on held-out gaps (MAE +155 s, CI [72, 249]) | `reply-delay-heldout-2026-09-02.json` | decision log wired; model `off`; shadow logger in place |
| C6 | mlx-tune SimPO/KTO trainer path | Built, dry-run verified on glm4_moe, scale pinned 2.0, registry accepts `mlx_tune`. No real run yet | `v6-orpo-execution-record-20260727.md` | candidate stage enabled for the 09-06 window |
| C7 | daemon.c batch-reply carve-out | **Measured, not cut**: no contiguous ≥1,200-LOC slice with ≤25 crossing locals (best 27 at lines 5700–7100). Two-slice plan filed as a chip | — | daemon.c 14,055 LOC, ratchet holds |

Gap #9 (persona evolution) was also measured: **not measurable** from chat.db (30-day retention; both events predate it). The pass found the persona's style numbers contradicting each other (lowercase-start 4% vs 17.3% vs measured 8.6%); the style card is now the single source (landed by a chip session, 8fc8a022d).

## What changed in production during the fleet

- Semantic recall LIVE (above). Adapter-side: `:8741` restarted 09-04 05:25 by another session with the fix that makes the persona adapter actually bind — every earlier `:8741`-side number measured the raw base.
- Nightly retrain **trains** as of 2026-09-05 03:07 (first real adapter since ≤08-08: 556 MB, 80/80 `lora_b` non-zero). Four defects fell first: SIGTERM/KeepAlive relaunch; missing `--adapter-out`; launchd running a shared checkout 123 commits behind main; a 120 s exit wait a 54 GB server cannot meet. Plus `check-no-resident-model.sh` before training.
- Outage 09-03 04:31: `nightly_eval.sh` loaded a second model beside the server; server died as a `?E` zombie; reboot. Guarded (`FIDELITY_SKIP` while `:8741` serves), and the fidelity step now generates through the server.
- Casing: with the adapter bound, replies are 86% lowercase-start vs Seth's 6%. Cause: preference corpus chosen side 77.5% lowercase. The rebalancer now equalises both sides (margins 0.715→0.005 lowercase, 0.338→0.000 punctuation) and a casing probe gates candidates at ≤10%.

## Measured-and-negative, kept as such

Delay model (C5), reconstructive retrieval (C2), first LIVE recall run (C1), persona evolution (#9). None was tuned until it passed; each is recorded with its number and its cause.

## Open

- Gap #8: the 09-06 window's mlx-tune candidate and its offline LUAR + casing score vs v6.
- Task 10 admission queue: built and tested on `task10-admission-queue` in gemma-realtime-1, undeployed (needs a `:8741` restart).
- Allowlist: gates met; opening is a product decision.
- Chips: key propagation re-measure (C2), pre-August history (#9), daemon.c two-slice plan (C7), release-size doc claims.
- Two Seth punctuation numbers exist (25% same-context vs 18.3% style card); the style card is the source.

## Process notes worth keeping

Critic caught a real unreachable call site; the coordinator's own verification caught an unpaired-arms bug and a harness join bug (LoCoMo 0.000 on every arm). Both were "identical numbers / zeros everywhere" tells — see `.claude/rules/no-number-without-a-measurement.md`. A green push proved nothing about the script launchd executed; the shared checkout must track origin/main (`.claude/rules/session-worktree-isolation.md`).
