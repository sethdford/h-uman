---
plan: docs/plans/2026-04-11-strategic-missions.md
auditor: group-6-hula-platform-strategic
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Red-teamed strategic plan with six missions (M1 Persona-First, M2 Personal
Model, M3 Private Learning, M4 Ship to Users, M5 HuLa as Platform, M6 Channel
Focus). Each mission has phased deliverables, quarterly milestones, and an
honest difficulty assessment.

## Key Claims (from the plan)
- M1: Persona unconditional in `hu_agent_t`; 100+ `HU_HAS_PERSONA` guards
  removed; `human init` creates starter persona with channel overlays.
- M2: `hu_personal_model_t` defined in `include/human/memory/personal_model.h`
  and `src/memory/personal_model.c`; ingested every user turn; 6 tests.
- M3: Track A (prompt personalization) done — personal model rendered into
  system prompt; few-shot persona examples loaded. Track B (on-device LoRA)
  is quarter-scale; `--checkpoint` currently `(void)`.
- M4: `human init` and `human onboard` exist; first-run path; provider
  auto-detect.
- M5: HuLa SDK header at `include/human/hula_sdk.h` with version macros and
  ergonomic helpers; 79 HuLa tests passing.
- M6: Channel tiering in `docs/plans/2026-04-11-channel-tiers.md`; Tier 1 =
  Telegram, Discord, iMessage, Slack.

## Evidence

### Implemented? (code exists)
- **M1**: `grep -rn "HU_HAS_PERSONA" src/ include/` returns only **1 hit**
  (`src/app/main.c:2212`) — plan's "100+ removed, 2 remain" claim matches reality.
  `src/persona/*.c` is 20+ files. `src/onboard/onboard.c` is 499 LOC as CLAUDE.md
  states.
- **M2**: `include/human/memory/personal_model.h` and
  `src/memory/personal_model.c` (2365 LOC) both present.
  `src/memory/fact_extract.c` still uses heuristic patterns — consistent
  with plan's "brittle pattern matching" honest finding.
- **M3**: `src/ml/cli.c:1261-1287` now loads checkpoint via
  `hu_ml_checkpoint_load` (no longer `(void)checkpoint_path`); however
  `src/ml/cli.c:905` still has `(void)checkpoint_path;` in the legacy code
  path. The honest-gap caveat doc is referenced at `cli.c:862`. Bridge plan
  `docs/plans/2026-05-10-m3-frontier-model-bridge.md` exists.
- **M4**: `src/app/main.c:952,2883` wires `hu_onboard_run_with_args` and the
  first-run-no-config check. `human onboard` is a real CLI subcommand.
- **M5**: `include/human/hula_sdk.h` exists with `HU_HULA_SDK_VERSION_STRING
  "0.1.0"` at line 70 and helpers at lines 82, 123, 185.
- **M6**: `docs/plans/2026-04-11-channel-tiers.md` exists; `src/channels/`
  contains 43 channel `.c` files (CLAUDE.md says 31; actual is 43, more than
  plan claims).

### Proven? (tests exist)
- M1: persona reaches the system prompt via `tests/test_personal_model.c::
  personal_model_reaches_system_prompt_via_config` (cited in CLAUDE.md, file
  exists at `tests/test_personal_model.c`).
- M2: `tests/test_personal_model.c`, `tests/test_personal_model_atomic_save.c`
  (with the named atomic-save test from CLAUDE.md at line 40),
  `tests/test_personal_model_contradicts.c` — all three exist.
- M3: `tests/test_provider_all.c:3071` — the regression guard
  `test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` exists
  and is registered at line 3369 (commit 028f4544 as CLAUDE.md claims).
- M5: HuLa tests = 69 (test_hula.c) + 5 (test_hula_golden.c) = 74. Plan
  claims "79 HuLa tests passing" — close but not exact; minor drift.
- M4, M6: No A/B harness or per-channel naturalness eval suites found —
  these are future phase items per the plan and are not yet built.

### Wired? (called in runtime path / dispatch)
- M1 persona wired: `src/agent/agent_turn.c` references persona unconditionally
  (no `HU_HAS_PERSONA`); `human init` creates starter persona on first run.
- M2 personal model wired: `src/agent/agent_turn.c:962` calls
  `hu_personal_model_ingest` per turn; `:999` calls
  `hu_personal_model_save`; `:3506` calls `hu_personal_model_build_prompt`
  to inject into system prompt.
- M3 Track A wired (prompt path), Track B NOT wired into production
  inference — `lora-persona` is a separate CLI, not invoked by the agent
  runtime.
- M4 onboard wired at `src/app/main.c:952` (subcommand) and `:2883` (first-run
  auto-redirect).
- M5 SDK helpers (`hu_hula_sdk_call/sequence/run_json`) defined but
  **zero callers in tests/ or examples/** — surface ships unexercised.
- M6 channel tiering exists as a doc but no runtime gating by tier was
  found; tiers are an editorial classification at this stage.

## Gaps
- **M2.2-2.6**: LLM-based preference extraction, pattern aggregation,
  adaptive prompting, evaluation framework, correction UX — all unimplemented;
  fact extraction is still heuristic patterns.
- **M3 Track B**: Frontier model bridge has a plan
  (`docs/plans/2026-05-10-m3-frontier-model-bridge.md`) but no MLX/ggml LoRA
  integration in production code; `src/ml/cli.c:905` still has the legacy
  `(void)checkpoint_path` annotation in the test-mode branch.
- **M4.2-4.6**: No "day 2" hook, no cost controls, no closed alpha — claims
  in CLAUDE.md state these remain.
- **M5.2-5.5**: HuLa SDK helpers exist but are not exercised by tests or
  examples; no gateway endpoint, no playground, no public docs.
- **M6.2-6.4**: Per-channel persona overlay audits, channel-specific tuning,
  and naturalness eval suites are not present.

## Notes
- The plan accurately self-describes status in its Quarterly Milestones
  table (Q2 items mostly DONE; Q3+ open). It is not stale.
- Plan's frontmatter `status: active` is correct.
- Mission claims cross-check tightly with `CLAUDE.md` Product Thesis section;
  both documents describe the same reality.
- Drift detected: plan claims "9,063/9,063 tests passing" (M1 section) and
  CLAUDE.md claims "10,000+ tests"; the live test count is between these
  values — minor staleness in the M1 phase prose, not a strategic concern.
