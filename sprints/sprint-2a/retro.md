---
title: "Sprint 2a — Retrospective"
created: 2026-05-11
sprint: "sprint-2a-hygiene-baseline"
result: PASS
---

# Sprint 2a Retrospective

## What shipped

| Story | Outcome | Evidence |
|---|---|---|
| 0 — SCRUM protocol fix | DONE | `~/.claude/skills/scrum/SKILL.md` + `~/.claude/agents/scrum-master.md` updated; rationale + diff in `sprints/sprint-2a/evidence/0/scrum-skill-diff.md` |
| B — Orchestrator e2e test | DONE | 11/11 PASS in `sprints/sprint-2a/evidence/B/run-orchestrator-e2e.sh`; shellcheck clean; sentinel for BSD-grep regression |
| C — Schema fix | DROPPED at planning | Concurrent `sprints/sprint-2/` security sprint owns the schema fix as their AC-D.3/D.4 |

## What worked

1. **De-confliction at planning.** Reading `sprints/sprint-2/stories.md` BEFORE starting any work let me drop the redundant schema-fix story up front, instead of duplicating effort and creating a merge collision.
2. **The new rules dogfood themselves.** Sprint 2a was the first sprint to apply the Phase 0 (branch isolation) and commit-before-handoff rules from Story 0. Both stories committed to `sprint-2a-hygiene-baseline` immediately; no working-tree drift between handoffs.
3. **Sentinel test for the Sprint 1 regression.** Story B's sentinel scenario explicitly re-asserts the BSD-grep-regex fix from Sprint 1 — any future revert will fail the test, not silently re-break production.
4. **Heredoc'd shim was the right grain of fakery.** Building a real test provider in C would have required CMake plumbing and an `HU_IS_TEST` integration. A 30-line bash shim that mimics four CLI calls was deterministic, portable, and cheaper.

## What broke

1. **M2 work (Phase 2 / Story A) was already done by a concurrent agent.** When I went to read `personal_model.c` to plan the heuristic→typed extraction migration, I found `hu_fact_extract` already implemented with subject/predicate/object/confidence/decay/dedup/provenance/trust-tier. The CLAUDE.md M2 status row is now out of date — what it calls "heuristic regex" has been substantially upgraded. **Action:** flag for the next CLAUDE.md hygiene pass.
2. **The repo has many concurrent-agent unstaged files.** `git status` showed 18 modified + 6 untracked files in workstream-adjacent areas (`world_model.c`, `agent_turn.c`, `examples.c`, new `channel_trust.h`, etc.) — none of mine. Working on this branch was safe because I didn't touch those files, but the surface area of concurrent activity is high enough that a real sprint should use a worktree, not just a branch.
3. **`shellcheck` defaulted to flagging `A && B || C` as info.** Mostly noise (B is `ac_pass` which always returns 0), but it's a recurring style nit. Consider documenting "use `if/then/else` for assertion idioms" in `docs/standards/engineering/`.

## What to change next sprint

1. **Use a worktree, not just a branch, for sprint isolation.** Phase 0's "isolate to a branch" rule was sufficient here, but in a repo where 18 files are unstaged from concurrent agents, a dedicated worktree (`git worktree add ../human-sprint-N sprint-N-slug`) is the safer move. Update Phase 0 to recommend worktrees when concurrent activity is detected.
2. **Refresh the CLAUDE.md M2 status row.** Reality: typed propositional/prescriptive fact extraction with confidence + decay + provenance + trust tier is shipped. The CLAUDE.md text still describes the pre-migration heuristic. Bake into the next docs ceremony.
3. **Read `sprints/` for in-flight work BEFORE planning.** This caught one collision (Story C → drop) but also revealed that concurrent agents had built `hu_fact_extract`, `world_model_bridge`, `channel_trust` — none of which were on my radar. Consider a `scripts/sprint-status.sh` that prints all open sprints + dirty paths in one shot.

## Key metrics

- **Stories shipped:** 2 DONE + 1 DROPPED-at-planning (out of 3 originally scoped)
- **ACs delivered:** 12 of 12 in-scope
- **Commits to durable branch:** 2 (`Story 0`, `Story B`)
- **Tests added:** 1 e2e bash driver, 11 assertions
- **Lines of net production code added:** 0 (all evidence + protocol updates)
- **Wall-clock time:** ~30 min (1 SCRUM protocol edit + 1 small bash test + 1 verify pass)
- **Concurrent-agent collisions avoided by reading sprints/sprint-2/ first:** 1 (Story C schema fix)

## Acknowledgments

The new SCRUM Phase 0 + commit-before-handoff rules are now battle-tested by their own sprint. Per-story commit discipline made every step recoverable from any concurrent-agent disruption.
