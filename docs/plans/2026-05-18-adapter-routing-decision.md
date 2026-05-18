---
title: "Adapter Routing — persona-v8 vs texting-llama-8b — Decision Doc"
created: 2026-05-18
status: decision
owner: ML subsystem
related:
  - CLAUDE.md (M3 row in Strategic Missions)
  - docs/plans/2026-05-17-m3-personalization-loop-closeout.md
  - docs/plans/2026-05-11-init-02-molora-channels.md
  - docs/plans/2026-05-11-rl-loop-phase-2-dpo-reactions.md
trigger: 2026-05-18 personal-onboarding audit found 5+ trained adapters across two base models; only persona-v8 over Gemma-4-26B is loaded by the daemon.
---

# Adapter Routing Decision

## The question

The user has trained five+ personal adapters across **two different base models**:

| Adapter | Base | Notes |
|---|---|---|
| `~/.human/adapters/persona/` (persona-v8) | mlx-community/gemma-4-26b-a4b-it-4bit | **Currently loaded** by daemon. Rank 8, 16 layers, 1000 iters, lr 2e-6. Broad persona. |
| `~/.human/training-data/adapters/seth-lora-v2-w13/` | (same Gemma-4-26B family, per file co-location) | W13 learning-loop SFT. |
| `~/.human/training-data/adapters/seth-dpo-iter80plus/` | (same Gemma-4-26B family) | DPO checkpoints at iter 100, 200. |
| `~/.human/training-data/adapters/seth-sprint8-dpo-e2b/` | (same Gemma-4-26B family) | Sprint 8 DPO experiment. |
| `~/.human/training-data/adapters/texting-llama-8b/` | **mlx-community/Meta-Llama-3.1-8B-Instruct-4bit** | Rank 8, 16 layers, 1000 iters, lr 1e-5 (5× more aggressive than persona-v8). Multiple checkpoints (0001000, 0000700, 0000400). Different base model. |

User's question: should iMessage use the broad `persona-v8` adapter (currently loaded) or the texting-specialized `texting-llama-8b`?

## The constraint that resolves it

**LoRA adapters can only be hot-swapped onto the same base model they were trained against.** `persona-v8` was trained on Gemma-4-26B-a4b-it-4bit. `texting-llama-8b` was trained on Meta-Llama-3.1-8B-Instruct-4bit. **They cannot be hot-swapped onto the same MLX server.**

Routing between them at runtime would require either:

- **Option A — Two MLX servers running side-by-side.** ~26 GB resident for Gemma-26B + ~8 GB for Llama-3.1-8B ≈ 34 GB. Plus per-request dispatch overhead. Plus duplicated KV cache management. Plus two configurations to keep aligned. **Cost:** RAM-heavy; complicates deployment; adds a latency penalty on the first request after a routing switch (cold KV cache on the inactive server).
- **Option B — Stop the MLX server and re-start it on the other base** when channel context changes. Multi-second restart on every channel switch. **Cost:** unacceptable user-facing latency; rules itself out.
- **Option C — Multi-LoRA composition over the SAME base.** The existing `agent_turn.c:4879` MoLoRA infrastructure (init-02) supports this. Both adapters must share the base. Today, only the Gemma-4-26B family does. `texting-llama-8b` would have to be **retrained on Gemma-4-26B** to participate. **Cost:** ~30-60 minute MLX training cycle (per the existing `human ml lora-persona` pipeline); zero deployment-side change.

## Recommendation

**Adopt Option C.** Specifically:

1. Keep `persona-v8` on Gemma-4-26B as the **base voice adapter**, always loaded.
2. Train a NEW iMessage-scoped adapter, `imessage-tapback-v1`, on **the same Gemma-4-26B base**, using the now-flowing tapback-DPO signal once `dpo_pairs.source = "imessage_tapback"` accumulates ≥500 rows (per the SOTA-data-floor research — Cui et al. 2025 — that's the lower bound for noticeable voice shift on a narrow specialization).
3. Wire `agent_turn.c:4879` MoLoRA router to **compose `persona-v8 + imessage-tapback-v1`** when `channel == "imessage"`, and `persona-v8` alone otherwise.
4. **Retire `texting-llama-8b` as a deployed adapter** — it's a research artifact on a different base. Keep the .safetensors as a reference for cross-base ablation studies but don't route to it from the daemon.

## Why this is the right call

- **One base, multiple adapters** is the architecture the existing code (MoLoRA, init-02-molora-channels) was designed for. Don't introduce a second deployment topology to support one specialized adapter.
- **Gemma-4-26B beats Llama-3.1-8B on instruction following** per Anthropic's recent eval cards and the persona-v8 lr=2e-6 vs texting-llama-8b lr=1e-5 ratio (more aggressive lr is needed on the weaker base to extract the same persona signal — empirical evidence that the 8B base is undertrained for our purposes).
- **The tapback-DPO signal didn't exist when texting-llama-8b was trained.** As of 2026-05-18 with the `reaction_collection` config fix landed, the iMessage adapter retraining will use a corpus that texting-llama-8b never saw. Retraining on Gemma-4-26B isn't a step backward; it's training on a strictly better corpus over a strictly stronger base.
- **MoLoRA composition lets us layer specialization without losing breadth.** Channel-scoped adapter on top of broad persona is a strict superset: when iMessage matters, you get both; when iMessage isn't active, persona-v8 alone is unchanged.

## Concrete next steps

- [ ] Wait for `dpo_pairs.source = "imessage_tapback"` to accumulate ≥500 rows. Monitor with: `sqlite3 ~/.human/memory.db "SELECT COUNT(*) FROM dpo_pairs WHERE source='imessage_tapback'"`.
- [ ] Once threshold hit, train `imessage-tapback-v1`: `human ml lora-persona --persona seth --from-history --channel imessage --base gemma-4-26b --output ~/.human/adapters/imessage-tapback-v1/`.
- [ ] Validate against the (now-real, not test-mode) Phase 5 eval gate.
- [ ] If Phase 5 gate promotes the new adapter, wire MoLoRA channel-scoped routing at `agent_turn.c:4879`: `if (channel == "imessage") compose(persona_v8, imessage_tapback_v1) else persona_v8`.
- [ ] Document the retirement of `texting-llama-8b` in `~/.human/training-data/adapters/texting-llama-8b/RETIRED.md` so future audits don't re-evaluate it as a candidate.

## What this is NOT

- This is NOT a multi-model deployment proposal. We stay on one base.
- This is NOT a recommendation to retrain `persona-v8`. It stays as-is.
- This is NOT a sub-7B-base proposal. The 26B base is the right host for voice fidelity even though it's larger; Apple's AFM tech report (2025) explicitly chose a 3B base only because they couldn't ship a larger model under iOS memory budget. We don't have that constraint on macOS.

## Risks

- **Risk 1:** MoLoRA composition introduces inference-time overhead per Init-02. Mitigation: the existing init-02 plan has a benchmark gate (5% p95 latency regression budget); test before promotion.
- **Risk 2:** `imessage-tapback-v1` over-specializes and degrades non-iMessage performance via channel-routing misfire. Mitigation: Phase 5 gate already runs the persona-fidelity eval on non-iMessage prompts; regression there blocks promotion.
- **Risk 3:** Future macOS / MLX-LM updates change LoRA adapter swap semantics. Mitigation: the M3 closeout already pinned this via live-fire test; any regression would surface in CI before reaching prod.

## Open questions for the user

- Are there cross-channel relationships where the iMessage adapter would also help? E.g., a Slack DM to a close friend is texting-shaped — should `imessage-tapback-v1` also route on `channel == "slack" && contact_relationship == "close_friend"`? Default: no, scope it strictly to channel until evidence supports broadening.
- Adapter scale (α) — `persona-v8` uses `scale: 20.0`. When composing with `imessage-tapback-v1`, what's the right α for the second adapter? MoLoRA init-02 has a default but the question hasn't been answered empirically for THIS pair. Suggest: start with the same 20.0, A/B against 10.0 once data is in.
