# Sprint 56 Backlog — "Gemma = Seth" Carryover + Operational Hardening

**Date:** 2026-05-26
**Branch:** sprint-56-gemma-as-seth (worktree at `/Users/sethford/Projects/human-sprint-56`)
**Base:** fe6dd1e2 (origin/main)

## Sprint goal

Now that `default_provider: "mlx_local"` routes every reply through the local LoRA-adapted Gemma (Sprint 55 + Mindy fix), prove the routing change actually moved the quality needle, close the continuous-learning loop, and harden the operational story.

## Stories

| # | Title | Size | Source | Dependencies |
|---|-------|------|--------|--------------|
| US-9 | Nightly fidelity eval harness + SOTA gate | M | Sprint 55 carryover (designs/US-9.md) | (none — measurement is independent) |
| US-8 | Training loop Phase C3 — `--source-jsonl` | L | Sprint 55 carryover (designs/US-8.md) | (none — but feeds US-9's data) |
| US-12 | Director context-bleed root cause + fix | M | NEW from Mindy diagnostic | director-bleed investigator |
| US-13 | Cloud-fallback policy for MLX-down | S | NEW from "Gemma = Seth" routing decision | cloud-fallback investigator |

## Story summaries (refs to existing designs)

### US-9 — Nightly fidelity eval harness + SOTA gate
See `/Users/sethford/Projects/h-uman/docs/plans/2026-05-25-m3-sota-sprint/designs/US-9.md` (109 lines, complete).
**Now load-bearing:** with all replies on MLX, the eval IS the proof of "Gemma = Seth."
**ACs (verbatim from US-9 design):**
- AC-9.1: Loads 20-30 held-out prompts from Seth's real conversation patterns
- AC-9.2: Two passes — (pre) base model alone, (post) base + trained adapter
- AC-9.3: Persona-fidelity scoring ∈ [0,1] per fidelity.c
- AC-9.4: Bootstrap CI over N=100 resamplings
- AC-9.5: SOTA gate: `post_mean > pre_mean + 1.96 × stderr` (α=0.025)
- AC-9.6: Gate PASS when post_delta ≥ 0.05; FAIL/SKIP when delta < 0.05

### US-8 — Training loop Phase C3 (`--source-jsonl` + real training)
See `/Users/sethford/Projects/h-uman/docs/plans/2026-05-25-m3-sota-sprint/designs/US-8.md` (240 lines, complete).
**Closes the loop:** DPO outcomes → JSONL → mlx_lm.lora subprocess → new adapter → hot-swap.
**ACs (verbatim from US-8 design):**
- AC-8.1: `scripts/training_loop.py --source-jsonl <path>` accepted
- AC-8.2: Prompt hashes resolved via conversation DB; unresolvable skipped with log note
- AC-8.3: Real `mlx_lm.lora` invocation (rank=8, scale=2.0 per `~/.claude/rules/lora-scale-default-or-die.md`, iters=500)
- AC-8.4: Output safetensors written to `--adapter-out <path>`
- AC-8.5: `scripts/test_training_loop_source_jsonl.py` integration test passes
- AC-8.6: Exit 0; safetensors ≥100 KB; adapter loads via `hu_mlx_provider_load_adapter`

### US-12 — Director context-bleed root cause + fix (NEW)
**Background:** Mindy turn 2026-05-25 16:41 produced director output `"...keep it brief so he can get back to the drink"` where "the drink" came from a different contact's conversation. Prior investigator's Gemini-cache hypothesis was dismissed (Gemini caching doesn't return responses, only speeds prefill).
**Investigation in flight:** background agent tracing `entries` and `combined` to their sources in `src/daemon.c` director call site.
**ACs (to be refined after investigator returns):**
- AC-12.1: Root cause identified with file:line citation
- AC-12.2: Fix lands at source (per-contact scope or proper contact filtering)
- AC-12.3: Regression test pins per-contact isolation: turn for contact A then contact B; assert B's director input does NOT contain content from A's channel history
- AC-12.4: Full test suite passes + ASan clean

### US-13 — Cloud-fallback policy for MLX-down (NEW)
**Background:** With `default_provider: "mlx_local"`, if mlx-server.py is restarting/slow/crashed, every reply attempt fails. Need a clean policy.
**Discovery in flight:** background agent identifying existing degradation infra + scoping the smallest viable fallback.
**ACs (to be refined after investigator returns):**
- AC-13.1: Policy chosen and implemented: either (A) cloud-Gemini fallback when MLX returns non-OK, OR (B) queue+retry until MLX recovers
- AC-13.2: Configurable via `personalization.mlx_down_policy = "cloud_fallback"|"queue"` (default: chosen by impl)
- AC-13.3: Operator log line on first MLX failure naming the policy in effect (per `~/.claude/rules/silent-config-gated-subsystems.md`)
- AC-13.4: Unit test pins the policy decision: simulate MLX returning HU_ERR_NETWORK; assert chosen-policy path fires
- AC-13.5: Manual test: stop mlx-server.py; send a fixture inbound; observe expected fallback behavior

## Realistic sprint shape — applying Sprint 55 retro lessons

Per the Sprint 55 retro (`docs/plans/2026-05-25-m3-sota-sprint/retro.md`):
- Cap parallel agent dispatches at 5
- Bundle stories that share files
- Pick up carryover FIRST (US-8 + US-9)

**This session targets a TIGHT wave:**

**Wave 1 (THIS SESSION — small/medium):**
- US-12 (director-bleed) — investigator returns; surgical fix at the leak site
- US-13 (cloud-fallback) — small policy + ~50 LoC wiring

**Wave 2 (FOLLOW-UP SESSIONS — large):**
- US-9 (eval harness) — needs ~30 held-out prompts curated + bootstrap CI infra
- US-8 (training loop) — Python + mlx_lm subprocess; LARGE per PO sizing

US-9 is the highest-value story but also genuinely the riskiest-to-rush. Splitting wave 1 from wave 2 honors the retro's "smaller batches" rule and lets us actually ship US-12+US-13 today rather than half-finishing all four.

## Non-goals
- New persona overlays (already in stories.md/2026-05-25-m3-sota-sprint/non-goals)
- Per-contact warmth routing (superseded by `default_provider: "mlx_local"`)
- HuLa IR work
- Initiative-layer extensions (parallel workstream)

## Open decisions for operator
- **US-13 policy default:** cloud-fallback (UX-first, loses personalization briefly) vs queue+retry (consistency-first, visible latency)?
- **US-9 fixture source:** real conversations from `~/.human/memory.db` with date-stratification (excluding last 30d to avoid training contamination) vs curated fixture JSONL?
