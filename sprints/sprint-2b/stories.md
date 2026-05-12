---
title: "Sprint 2b — Personal model honesty"
created: 2026-05-11
status: closed
sprint: 2b
branch: sprint-2b-personal-model-honesty
program: docs/plans/2026-05-10-master-follow-through-program.md
---

# Sprint 2b — Personal model honesty

## Sprint goal

Push two follow-through items from the Sprint 1 retro and the master program: enrich the starter persona with Tier-1 example banks (M4-adjacent), and prove the Track B `hu_memory_query_t.variant` scanner can actually catch a regression (master program B2.2).

## Branch

`sprint-2b-personal-model-honesty` (created from `sprint-2a-hygiene-baseline` tip).

## Stories

### Story A — DROPPED

The originally proposed Story A (M2 typed fact extraction migrating heuristic regex → typed `(claim, confidence, source_turn_id)`) is already shipped. `hu_fact_extract` exists in `include/human/memory/fact_extract.h` with subject/predicate/object/confidence/decay/dedup/provenance/trust-tier and is wired into `hu_personal_model_ingest`. The CLAUDE.md M2 status row is out of date.

### Story A' — Starter persona Tier-1 example banks

**As a** fresh user with no learned-from-history examples
**I want** the starter persona to ship concrete tone/length anchors per Tier-1 channel
**So that** the prompt builder has channel-shaped examples to anchor on for my first conversations, instead of falling back to a content-free generic prompt.

**Acceptance Criteria:**
- [x] AC-A.1: `hu_starter_persona_json` (in `src/onboard.c`) ships an `example_banks` array with entries for `imessage`, `telegram`, `discord`, `slack`.
- [x] AC-A.2: Each Tier-1 bank contains at least one complete example (non-empty `context` + `incoming` + `response`).
- [x] AC-A.3: Examples are neutral — no proper nouns, PII, politics, or proper names.
- [x] AC-A.4: Examples reflect the channel's overlay style (formality, avg_length, emoji_usage).
- [x] AC-A.5: A new test `persona_directive_starter_persona_ships_tier1_example_banks` verifies the JSON parses and exposes 4 Tier-1 banks each with ≥1 complete example.
- [x] AC-A.6: A new test `persona_directive_tier1_overlay_bank_coherence` verifies overlay ↔ bank symmetry (every Tier-1 channel exposes both).

### Story D — Track B negative test for `check-memory-query-variant.sh`

**As a** maintainer relying on `verify-all.sh`
**I want** a test that proves the `hu_memory_query_t.variant` scanner actually catches a regression, not just that the current tree is clean
**So that** if a future commit reverts the inventory fix or the scanner regex breaks, the gate doesn't silently pass.

**Acceptance Criteria:**
- [x] AC-D.1: `scripts/check-memory-query-variant.sh` accepts a `HU_VARIANT_SCAN_ROOT` env override (test-only, fully backward compatible).
- [x] AC-D.2: A negative-test driver under `sprints/sprint-2b/evidence/D/` synthesizes good and bad fixture trees and asserts the scanner exits non-zero on bad and zero on good.
- [x] AC-D.3: Driver also re-asserts the live tree's inventory remains clean (sanity).
- [x] AC-D.4: Driver shellcheck clean; scanner shellcheck clean after override.
- [x] AC-D.5: 4/4 PASS in `sprints/sprint-2b/evidence/D/run-log.txt`.

## Process improvements observed

- Sprint ran on dedicated `sprint-2b-personal-model-honesty` branch (Phase 0 protocol).
- Each story committed before the next began (commit-before-handoff).
- No working-tree drift between handoffs.

`RESULT_sprint=CLOSED N=2b stories=2/2 audit=PASS`
