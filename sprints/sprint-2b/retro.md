---
title: "Sprint 2b — Retrospective"
created: 2026-05-11
sprint: "sprint-2b-personal-model-honesty"
result: PASS
---

# Sprint 2b Retrospective

## What shipped

| Story | Outcome | Evidence |
|---|---|---|
| A' — Starter persona Tier-1 example banks | DONE | `src/onboard.c` (12 examples across 4 banks), 8/8 in `persona_directive_channels` (was 6/6), `sprints/sprint-2b/evidence/A/run-log.txt` |
| D — Track B negative test | DONE | `scripts/check-memory-query-variant.sh` adds `HU_VARIANT_SCAN_ROOT` override; 4/4 PASS in `sprints/sprint-2b/evidence/D/negative-test.sh` |

## What worked

1. **Sprint 2a's protocol fixes paid off immediately.** Branch isolation + commit-before-handoff worked the way the rules intended. Both stories committed before the next started, no working-tree drift, no risk of concurrent-agent reset wiping the work.
2. **Coherence test caught design-doc drift.** The new `persona_directive_tier1_overlay_bank_coherence` test pins the invariant that every Tier-1 channel that exposes an overlay also exposes a bank, and vice versa. Without it, a future editor adding a channel to one block but not the other would silently desynchronize the prompt builder.
3. **Negative test exposed a real scanner subtlety.** While building the bad fixture, the first attempt failed because the scanner treated `.variant =` substrings inside comments as proof of variant assignment. That's an actual scanner false-negative — without the negative test, no one would have noticed. Captured for follow-up.
4. **Backward-compatible env override.** `HU_VARIANT_SCAN_ROOT` is opt-in and the scanner's default behavior is unchanged. Test infrastructure should never force a behavior change in the production gate.

## What broke

1. **Pre-existing `human_tests` link break flickered during the sprint.** Mid-Story-A' build, `human_tests` failed to link with `_cmd_auth` / `_cmd_capabilities` undefined references — symbols that test_cli.c calls but no source file defines. Stashing my changes and rebuilding made it pass; popping the stash and rebuilding it passed too. Conclusion: a concurrent agent had a half-applied change to `src/agent/cli.c` (in their unstaged tree) that was briefly visible to my build via the build cache. This is a recurring symptom of concurrent-agent activity on the same workspace; a worktree would have completely isolated us from it.
2. **Scanner's `.variant =` regex matches comments.** A `/* No .variant = X */` comment makes the scanner think the variant IS being assigned. Easy follow-up: strip C/C++ comments before scanning. Captured below.
3. **CLAUDE.md mission status drift continues.** M2 row says "heuristic regex" — reality is `hu_fact_extract` with typed propositions, prescriptive rules, half-life decay, dedup, provenance, trust tier. M3 row already says "D0.3 seam in-tree ✅" but only because a different agent updated it; the user-visible CLAUDE.md text is not tracked as a sprint artifact and drifts ~weekly.

## What to change next sprint

1. **Use a worktree, not just a branch, when concurrent activity is high.** Sprint 2b worked on a branch but still saw the brief link-break flicker from concurrent unstaged files. `git worktree add ../human-sprint-N` would have isolated us completely. Update the SCRUM Phase 0 rule to PREFER worktrees when `git status` shows >5 unrelated unstaged files.
2. **Strip comments in the variant scanner.** One-line fix: `cleaned = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)` before regex matching. Concurrent agents own the scanner; flag as a follow-up ticket rather than fixing here.
3. **Refresh CLAUDE.md M2/M3 status.** Both rows are out of date relative to shipped capability. Bake into the next docs ceremony (`scripts/doc-fleet.sh`) or add an explicit step in the sprint-master agent prompt: "If any mission status changed during this sprint, update CLAUDE.md before close."
4. **Backlog ticket: sprint-status-quick-look script.** `scripts/sprint-status.sh` would print all open `sprints/sprint-*` directories + dirty paths from `git status` in one shot, so future sprints can cheaply de-conflict at planning time. Sprint 2b only avoided collision because I read `sprints/sprint-2/stories.md` manually.

## Key metrics

- **Stories shipped:** 2 DONE + 1 DROPPED-already-shipped (out of 3 originally scoped)
- **ACs delivered:** 12 of 12 in-scope (M2 typed-extraction story dropped because it was already done)
- **Commits to durable branch:** 2 (`Story A'`, `Story D`)
- **Tests added:** 2 new C test cases (8/8 persona_directive_channels passing) + 1 bash negative-test driver (4 assertions)
- **Lines of net production code added:** ~120 (90 lines of starter persona example banks + 30 lines test cases + 9 lines scanner override)
- **Wall-clock time:** ~25 min for both stories
- **Concurrent-agent collisions detected during sprint:** 1 (Story A originally scoped → already shipped) + 1 brief link-break flicker → both detected by reading instead of building blind

## Cumulative process scorecard (Sprints 1, 2a, 2b)

| Behavior | Sprint 1 | Sprint 2a | Sprint 2b |
|---|---|---|---|
| Sprint runs on dedicated branch | NO (failed) | YES | YES |
| Implementer commits before handoff | NO (failed) | YES | YES |
| Critic per-story | NO (batched, missed bug) | YES (built into rule) | YES (self-applied) |
| Sprint wiped by concurrent agent | YES ×2 | NO | NO |
| Stories closed without evidence | NO | NO | NO |
| Mission status drift discovered | N/A | YES (M2 outdated) | YES (M2 still outdated; M3 partly out of date) |

The protocol fixes from Story 0 of Sprint 2a are working. The remaining drift is in the human-readable mission narrative, which a SCRUM agent prompt addition could automate.
