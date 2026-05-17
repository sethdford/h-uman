---
plan: docs/plans/2026-05-10-sota-roadmap-6mo.md
auditor: group-8-behavior-m3-master-sota
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: MEDIUM
---

## Plan Summary
Six-month portfolio plan with three concurrent tracks (A personalization, B
performance, C UX) plus a cross-cutting eval track E. Steered by 9 north-star
metrics including persona preference vs Gemini PI/Claude Cowork (N1), retrieval
relevance (N2), per-channel naturalness (N3), decode TPS at each tier (N4/N5),
TTFT (N6), adapter regression (N7), DAU/retention (N8), and binary/RSS budget (N9).
Status: `draft`.

## Key Claims (from the plan)
- A1 (Month 1) — training-data pipeline from conversation history with PII +
  quality filters + per-channel persona example banks
- A2 (Month 2) — `lora-persona` against the actually-served frontier model
  (MLX Gemma-4-31B), not the toy HUML GPT
- A3 (Month 3) — persona-aligned E4B draft adapter (single shared milestone
  with Track B's spec decode)
- A4–A6 (Month 4–6) — personal-model upgrade, continuous-learning loop, "feels
  like me" public eval suite
- B1–B6 — multi-tier routing, spec-decode prep, TTFT < 200 ms, Apple-deep
  optimizations, cross-platform parity, public bench
- C1–C6 — onboarding gate, Tier-1 channel hardening, per-channel naturalness
  evals, cross-channel continuity, retention infra, 100 DAU launch
- E1–E5 — unified eval CLI, self-hosted M-series CI runner, regression gates,
  longitudinal eval, external benchmark drop

## Evidence

### Implemented? (code exists)
- **A1 training-data pipeline:** `src/ml/training_data.c`,
  `training_data_extractor.c`, `training_data_quality.c` present;
  `hu_ml_cli_prepare_conversations` lives in `src/ml/cli.c:594`
- **A1.3 banks-from-history:** `hu_persona_banks_extract_from_history` in
  `src/persona/examples.c:519` — the A.0.5 milestone in the M3 plan
- **A2 frontier LoRA bridge:** llama.cpp provider real impl exists
  (`src/providers/llamacpp.c` + decode/kvcache/sampling sub-modules) when
  `HU_ENABLE_LLAMACPP=ON`. MLX provider is scaffold-only (`src/providers/mlx.c`,
  all NOT_SUPPORTED) — A2's "via mlx_lm.lora" path is NOT shipped.
- **A4 personal-model:** symmetric signal aging, v3→v4 migration, idle decay,
  goal pipeline, recently-completed scratchpad, EWMA style decay, per-turn
  maintenance helper all landed (see master follow-through audit)
- **B1 multi-tier routing:** `src/agent/model_router.c` enumerates the four
  tiers (REFLEXIVE/CONVERSATIONAL/ANALYTICAL/DEEP) at lines 180–203, but
  no E4B-local-first routing path verified
- **C1 onboarding:** `src/onboard.c` present (499 LOC per CLAUDE.md);
  starter-persona presets keyword search returned zero matches — preset
  library not yet authored
- **B2 bench scripts:** `scripts/bench-gemma-perf.py`, `bench-all-backends.py`
  present
- **Eval CLIs:** `lora-baseline`, `lora-ab`, `lora-runner`, `fidelity-status`
  all in `src/ml/cli.c`; fidelity scoring under `src/ml/fidelity.c` +
  `src/eval/persona_fidelity.c`

### Proven? (tests exist)
- A1: `tests/test_training_data*.c` files present (per CLAUDE.md test count
  baseline); explicit per-quality eval tests claimed in plan
- A2: `tests/test_llamacpp_provider.c` + sibling files. No `tests/test_mlx_lora*`
- A4: `tests/test_personal_model.c` (extensive — confirmed in M3 audit)
- B1: `tests/test_model_router.c` exists; plan flagged this as risk-tier HIGH
- Eval: `tests/test_longmemeval.c`, `tests/test_persona_eval.c`, ToM pack

### Wired? (called in runtime path / dispatch)
- A1 + A4 pipelines wired into `agent_turn.c` (M3 audit confirmed
  `hu_personal_model_per_turn_tick` invoked + telemetry surfaced)
- A2: chat-time LoRA merge real for HUML provider (Phase 4.0 done); for
  llama.cpp real when `HU_ENABLE_LLAMACPP=ON` + system libllama; for cloud
  providers explicitly NOT_SUPPORTED
- B1: model router selects tiers but I did not verify the E4B-local-first
  policy claimed in B1.1
- C1 onboarding wired as first-run fallback per CLAUDE.md
- B2 bench rig not yet wired into nightly CI per the plan's claim;
  scripts exist but no `.github/workflows/perf-bench.yml` confirmed

## Gaps
- **A2 is the load-bearing milestone and is only partially shipped.** The plan
  says "trains against the actual served model (Gemma-4-31B via mlx_lm.lora)";
  in code, the MLX provider returns NOT_SUPPORTED everywhere. llama.cpp path
  is real on opt-in builds but is the cross-platform fallback, not the
  Apple-Silicon hero path.
- **A3 (persona-aligned draft adapter) — the single highest-leverage milestone
  in the whole plan — is NOT started.** No `tests/test_*draft*spec_decode*`
  or aligned-draft training pipeline exists.
- **C2–C6 channel hardening, naturalness eval, retention, DAU launch — not
  visible.** `eval_suites/channel/<tier1>/` directory does not exist.
- **B3 prompt-cache extension for persona-examples prefix:** no
  `eval_suites/perf/` directory; B3 work not visible.
- **B4 Apple-deep .metal kernels, AMX spike, 3-bit weight QAT** — not started
- **E2 per-track eval suites:** `eval_suites/persona/`, `eval_suites/channel/`,
  `eval_suites/perf/`, `eval_suites/cross-channel/` directories are MISSING.
  The eval rig stands up partially via individual JSON files in `eval_suites/`,
  but the structured per-track layout is not created.
- N1 through N6 metrics have not been measured to baseline; only N7-style
  fidelity scoring (offline) is in place.

## Notes
- The plan is `status: draft` and predates execution. PARTIAL is the correct
  verdict — Month 1 work (A1 data pipeline, A4 personal-model upgrade, B1
  tier enum, C1 onboarding scaffold, eval-rig fragments) is largely in place;
  Month 2+ work (A2 real frontier LoRA, A3 aligned draft, B3 TTFT, C2 channel
  hardening, E2 per-track suites) is largely not.
- The convergence point (A3) is the load-bearing risk. The plan calls it the
  single point that unlocks both A and B back-halves — and it is not yet
  in motion.
- The plan IS being partially followed: the offline persona-fidelity
  evaluation infrastructure landed exactly as scoped (D2.2 in the master
  follow-through), and the personal-model upgrades match A4's targets.
- Cross-reference: the M3 plan's "Bridge B" is the MLX path A2/A3 depend on;
  Bridge B is currently scaffold-only, so the SOTA roadmap's critical-path
  milestone is blocked behind M3 Bridge B execution.
