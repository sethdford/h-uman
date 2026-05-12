---
title: "Sprint 2a — Hygiene baseline"
created: 2026-05-11
status: in-progress
sprint: 2a
branch: sprint-2a-hygiene-baseline
parent_retro: sprints/sprint-1/retro.md
---

# Sprint 2a — Hygiene baseline

## Sprint goal

Address the three Sprint 1 retro action items that have to land BEFORE another full sprint runs, so the same wiped-tree / latent-bug failure modes can't recur.

## Branch

`sprint-2a-hygiene-baseline` (created at planning, isolated from concurrent `feat/*` workstreams).

## Stories

### Story 0 — Bake commit-before-handoff + isolated-branch + per-story critic into the SCRUM skill

**As a** maintainer running future sprints
**I want** the SCRUM orchestrator to enforce branch isolation, commit-before-handoff, and per-story critic at the agent-prompt level
**So that** Sprint 1's wiped-tree event (concurrent `git reset --hard HEAD` on a shared feature branch) and Sprint 1's batched-critic-shipped-broken-regex event cannot recur.

**Acceptance Criteria:**
- [x] AC-0.1: `~/.claude/skills/scrum/SKILL.md` adds a Phase 0 (branch isolation) and three new "Hard rules" (sprint-isolated branch, implementer commits before handoff, critic per-story not batched).
- [x] AC-0.2: `~/.claude/agents/scrum-master.md` adds Phase 0, Phase 4 DoD additions (commit-existence, per-story-critic), and three new anti-patterns.
- [x] AC-0.3: Both rule sources cite Sprint 1's failure modes as the rationale (so a future maintainer reading the rule understands WHY it's there).
- [x] AC-0.4: Evidence doc `sprints/sprint-2a/evidence/0/scrum-skill-diff.md` captures the diff and rationale.

**Out of scope:**
- Updating product-owner / tech-lead / verifier / sprint-auditor agent prompts (their existing protocol is sufficient).
- Adding a CI check to enforce branch-naming or commit-existence (would require a hook or workflow; deferred).

### Story B (Sprint 1 follow-up) — Live-provider end-to-end test for `lora-runner-ab.sh`

**As a** maintainer of the orchestrator
**I want** a deterministic end-to-end test that exercises `scripts/lora-runner-ab.sh` against the in-tree test provider stub, including the publish block we just unblocked
**So that** future regressions to the canonical-AB-path publish logic are caught locally before reaching production behavior.

**Acceptance Criteria:**
- [ ] AC-B.1: A new test driver under `sprints/sprint-2a/evidence/B/` invokes `scripts/lora-runner-ab.sh` end-to-end against a mock provider that returns deterministic non-empty response sets, and asserts:
  - `~/.human/last_fidelity_ab.json` (or `HUMAN_FIDELITY_AB_PATH`) is written atomically (`mv` from tmp).
  - The published JSON has `baseline.responses[]` and `candidate.responses[]` populated.
  - Re-running with empty response sets does NOT overwrite the canonical file (short-circuit holds).
- [ ] AC-B.2: The test runs in `HU_IS_TEST` mode (no real network, no real ML inference).
- [ ] AC-B.3: shellcheck remains clean.
- [ ] AC-B.4: Evidence saved under `sprints/sprint-2a/evidence/B/run-log.txt`.

**Out of scope:**
- Wiring this test into `scripts/verify-all.sh` (Sprint 2b candidate, deferred).
- Live provider runs against Anthropic / OpenAI / Vertex (cost + non-determinism).

### Story C — DROPPED

The schema-fix follow-up for `human ml fidelity-status` is owned by the concurrent `sprints/sprint-2/` security-hardening sprint (their Story D AC-D.3 / AC-D.4). De-conflicting by dropping it here.

## Process improvements (effective immediately)

1. Sprint runs on `sprint-2a-hygiene-baseline` from planning. No work lands on `feat/*` shared branches.
2. After each story closes, commit immediately. The next story does not start on a dirty tree.
3. Critic runs per-story, not batched at sprint close.

`RESULT_product-owner=READY`
