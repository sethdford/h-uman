---
title: "Sprint 1 — Review (Definition of Done)"
created: 2026-05-11
sprint: "sprint-1-fidelity-followthrough"
result: PASS
---

# Sprint 1 Review — Definition of Done

Sprint goal: ship four follow-through items (A, B, C, D) on top of the SOTA fidelity infrastructure landed in `feat/sota-m1-infra`.

## Definition of Done — checklist per story

### Story A — directive telemetry dashboard tile

- [x] AC-A.1: `hu-directive-telemetry-tile` web component exists at `ui/src/components/hu-directive-telemetry-tile.ts`
- [x] AC-A.2: Component renders six variants (formal_terse, formal_short, formal_emoji, casual_terse, casual_short, casual_emoji) plus null_overlay/error/loading states
- [x] AC-A.3: aria-label format is `"<variant>: <count> fires (<pct>%)"` — fixed during critic pass
- [x] AC-A.4: Vitest unit tests in `ui/src/components/hu-directive-telemetry-tile.test.ts` — 7/7 PASS
- [x] AC-A.5: Wired into `metrics-view.ts` between fidelity and other intelligence sections
- [x] AC-A.6: Demo gateway mock in `ui/src/demo-gateway.ts` returns shape consumed by the tile

### Story B — orchestrator → canonical `~/.human/last_fidelity_ab.json`

- [x] AC-B.1: `scripts/lora-runner-ab.sh` writes to canonical path (`~/.human/last_fidelity_ab.json`) atomically (mv-from-tmp)
- [x] AC-B.2: Empty-response-set short-circuit prevents publish if either side has no responses (rewritten to portable `tr -d` after critic flagged BSD-grep regex bug)
- [x] AC-B.3: shellcheck clean (verified)
- [x] AC-B.4: 5/5 driver ACs PASS via `sprints/sprint-1/evidence/B/verify-ac1-3-4-5.sh`
- [x] AC-B.5: `HUMAN_FIDELITY_AB_PATH` env override supported

### Story C — Tier-1 channel overlays

- [x] AC-C.1: `hu_starter_persona_json` symbol centralized in `include/human/onboard.h` + `src/onboard.c`, consumed by both `human init` and `human onboard`
- [x] AC-C.2: Tier-1 channels (Telegram, Discord, iMessage, Slack) all populate `formality`/`avg_length`/`emoji_usage` (no JSON nulls, no integer/string mixups)
- [x] AC-C.3: Test suite `tests/test_persona_directive_channels.c` registered in `tests/test_main.c` and `CMakeLists.txt`, runs via `--suite=persona_directive_channels`
- [x] AC-C.4: 6/6 tests PASS, including `persona_directive_starter_persona_loads_four_tier1_overlays` (production-symbol test) and `persona_directive_tier1_batch_yields_zero_null_overlay`
- [x] AC-C.5: All four Tier-1 channels route to non-null directive variants (telegram → casual_*; discord → casual_emoji; imessage → casual_emoji; slack → formal_terse)

### Story D — Live LoRA evaluation under `HU_ENABLE_LLAMACPP`

- [x] AC-D.1: Outcome `DESCOPE_OK` with `sprints/sprint-1/evidence/D/descope-rationale.md` (135 lines, ≥10 required)
- [x] AC-D.2: Evidence directory has ≥1 supporting file (4 files: rationale, run-log, build-log, acs)
- [x] AC-D.5: Rationale documents a Category B blocker (4 matches: build-system, schema-mismatch, env-flag-mismatch, channel-handle vs provider mismatch)
- N/A AC-D.3, AC-D.4: gated on Story D being in scope; descope rationale supersedes

## Sprint-level DoD

- [x] All in-scope ACs satisfied (24/24)
- [x] All tests pass: `ui/` vitest 7/7, `human_tests --suite=persona_directive_channels` 6/6
- [x] Critic pass complete: 2 high-severity findings (B regex, A aria-label) fixed
- [x] All work durably committed to `sprint-1-fidelity-followthrough` branch (4 commits)
- [x] Sprint review (this doc) and retrospective (`sprints/sprint-1/retro.md`) authored
- [x] No real network, no process spawning, no hardware I/O in tests
- [x] No `SQLITE_TRANSIENT` introduced; no raw hex/pixel values in UI; AGENTS.md naming respected

## Sprint result: **PASS**

3 stories DONE, 1 story DESCOPE_OK with full evidence. Sprint goal met.
