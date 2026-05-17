---
title: "Sprint 2a — Review (Definition of Done)"
created: 2026-05-11
sprint: "sprint-2a-hygiene-baseline"
result: PASS
---

# Sprint 2a Review — Definition of Done

Sprint goal: address the three Sprint 1 retro action items that have to land BEFORE another full sprint runs.

## Definition of Done — checklist per story

### Story 0 — SCRUM protocol fix (commit-before-handoff + isolated branch + per-story critic)

- [x] AC-0.1: `~/.claude/skills/scrum/SKILL.md` adds Phase 0 (branch isolation) + three new Hard rules (sprint-isolated branch, implementer commits before handoff, critic per-story)
- [x] AC-0.2: `~/.claude/agents/scrum-master.md` adds Phase 0, Phase 4 DoD additions, and three new anti-patterns
- [x] AC-0.3: Both rule sources cite Sprint 1's failure modes verbatim
- [x] AC-0.4: Evidence captured in `sprints/sprint-2a/evidence/0/scrum-skill-diff.md`

### Story B — Live-provider end-to-end test for `lora-runner-ab.sh`

- [x] AC-B.1.a: Happy path — orchestrator exits 0 on non-empty responses + writes canonical AB JSON atomically + canonical content has expected delta + no leftover tmp files (atomic mv intact)
- [x] AC-B.1.b: Empty BEFORE — orchestrator exits 2, canonical file NOT written
- [x] AC-B.1.c: Empty AFTER — orchestrator exits 2, canonical file NOT written
- [x] AC-B.1.d: `--no-publish` — orchestrator exits 0, canonical file NOT written
- [x] AC-B.2: Test runs without `HU_IS_TEST` infra (heredoc'd shim, no real network, no ml inference)
- [x] AC-B.3: shellcheck clean on driver and heredoc'd shim body
- [x] AC-B.4: Evidence saved in `sprints/sprint-2a/evidence/B/run-log.txt`
- [x] Bonus: Sentinel test for BSD-grep regex regression — would catch any future revert of the Sprint 1 fix

### Story C — DROPPED (de-conflicted with concurrent sprint-2 security-hardening sprint)

The schema-fix follow-up for `human ml fidelity-status` is owned by the concurrent `sprints/sprint-2/` Sprint, AC-D.3/D.4. Removed from Sprint 2a scope at planning.

## Sprint-level DoD

- [x] All in-scope ACs satisfied (12 ACs total: 4 for Story 0, 8 for Story B)
- [x] All tests pass: 11/11 in `run-orchestrator-e2e.sh`
- [x] Per-story commits to `sprint-2a-hygiene-baseline`: 2 commits (Story 0, Story B)
- [x] Sprint runs on dedicated branch `sprint-2a-hygiene-baseline` (Phase 0 protocol applied)
- [x] No real network, no process spawning beyond shellcheck
- [x] `git log sprint-2a-hygiene-baseline ^sprint-1-fidelity-followthrough` shows 2 fresh commits

## Sprint result: **PASS**

2 stories DONE, 1 dropped at planning to de-conflict with concurrent sprint-2.
