---
title: SOTA Wave B+C Execution Plan
description: Finish the Wave B measurement gates and the Wave C ship-path slices on top of Wave A.
---

# SOTA Wave B+C Execution Plan

**Goal:** Finish measurement gates (B) and ship-path slices (C) on top of Wave A.

## Wave B tasks

1. Publish `docs/evaluation/locomo-method.md` + `longmemeval-method.md`; update SOTA_BENCHMARK external table to cite them.
2. Add `contact_id` / `session_id` to `hu_retrieval_options_t`; fail-closed hybrid/semantic when contact namespace required; filter results; tests.
3. Strengthen capability-gate CI: LIVE + required_gate=pass + human ABSENT or effective≠PASS → fail (already mostly true); add explicit ABSENT+LIVE unit fixture; leave capabilities OFF.
4. Document blind A/B: human ABSENT is honest; CI blocks LIVE activation.

## Wave C tasks

1. Onboard: prefer local provider (Ollama/MLX) as default when available.
2. One-command preference train: `human ml train-from-reactions` (or wire existing DPO/KTO).
3. Thin personal-model wiki: CLI `human memory wiki` listing index/log from personal model.

## Commits

- Wave A containment+honesty (existing dirty tree, exclude `docs/proof/rl-loop-proof.md`)
- Wave B measurement
- Wave C ship slices
