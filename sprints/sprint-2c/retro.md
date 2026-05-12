---
title: "Sprint 2c — Retrospective"
created: 2026-05-12
sprint: 2c
---

# Sprint 2c retrospective

## What shipped

Five small, surgical follow-ups closing every open action item from the Sprint 1, 2a, and 2b retros:

- **F1**: CLAUDE.md M1 + M2 + competitive table refreshed to match shipped capability (Tier-1 example banks, typed fact extraction, half-life decay).
- **F2**: `scripts/sprint-status.sh` — one-shot view of sprint dirs + dirty paths + sprint branches + worktrees, with a worktree-vs-branch recommendation.
- **F3**: Variant scanner now strips C/C++ comments before regex matching; pinned by a new comment-defeat fixture in the Sprint 2b Story D negative test (4/4 → 6/6 PASS).
- **F4**: SCRUM Phase 0 in `~/.claude/skills/scrum/SKILL.md` and `~/.claude/agents/scrum-master.md` now requires running `sprint-status.sh` first and prefers a worktree over a branch when concurrent activity is high.
- **F5**: `scripts/check-test-time-symbol-availability.sh` — static lint catching the four shapes of the Sprint 1 Story C `hu_starter_persona_json` failure mode (extern symbol declared in include/ but defined behind a guard that excludes test builds). 9/9 negative-test scenarios PASS. Wired into `verify-all.sh`.

## What worked

1. **Sprint 2a's protocol fixes paid off again.** Branch isolation gave each story its own clean checkpoint. When the concurrent-agent stash event happened (see "What broke") the recovery was a five-minute cherry-pick, not a "redo Sprint 1 from scratch" event.

2. **Per-story commits made the recovery surgical.** When I had to reset `sprint-2c-followups` and rebuild its history, I cherry-picked five named commits — no diffing or guessing.

3. **Sprint 2c dogfooded its own deliverables.** F2's `sprint-status.sh` correctly identified its OWN failure mode mid-sprint (the >5 dirty files + multiple sprint branches case → "use a worktree"). The recommendation was right; the next sprint should follow it.

4. **Negative tests caught what manual testing wouldn't have.** F5's negative test exercised three positive scenarios + four negative-control scenarios with synthetic fixtures inside `mktemp -d` directories. Without the negative controls the lint would have shipped with subtle false positives (extern function declarations, platform guards).

## What broke / friction

1. **Concurrent agent ran `git stash` on the working tree mid-sprint.** Around the time of F2's commit, another agent ran `git stash push -u`, which scooped up `scripts/sprint-status.sh` along with their own WIP. The file disappeared from the working tree. Recovery was easy (`git checkout f7ff3644 -- scripts/sprint-status.sh`) because F2 was already committed, but if F2 had been mid-edit the work would have been lost.

2. **Concurrent agent merged W9 cells onto `sprint-2c-followups` mid-sprint.** Right after F2's commit (07:59:11), a `merge(sprint-2c): pull in sprint-2b W9 cells` commit (`b9809f92`) appeared on `sprint-2c-followups` at 07:59:28 — 17 seconds later. The merge was unrelated to my work and added 100+ files. My F3/F4/F5 commits ended up on a different branch (`sprint-2b-personal-model-honesty`). I recovered by resetting `sprint-2c-followups` back to F2's tip (`f7ff3644`) and cherry-picking F3/F4/F5.

3. **Concurrent agent switched the working-tree branch.** Mid-build, `git branch --show-current` quietly went from `sprint-2c-followups` to `sprint-2b-personal-model-honesty`. Build continued because the underlying repo was the same; the surprise was the branch label change.

4. **F4 commit accidentally captured a concurrent agent's `sprints/sprint-2/stories.md` shrink.** When I ran `git add sprints/sprint-2c/evidence/F4/`, the index ALSO picked up a working-tree change to `sprints/sprint-2/stories.md` (concurrent agent had simultaneously closed their security sprint and replaced the in-progress version with a 40-line "done" version). The F4 commit message didn't mention that side effect; the cherry-pick later raised the file as a "deleted in HEAD modify/delete" conflict and I dropped the file (correctly — it doesn't belong on this branch).

## What to change next sprint

1. **Use a worktree, not a branch, for ANY sprint while concurrent activity is high.** Sprint 2b said this. Sprint 2c said this. Sprint 2c then ignored its own advice and paid the recovery cost. Next sprint must default to `git worktree add ../human-sprint-N -b sprint-N-slug` whenever `sprint-status.sh` recommends it.

2. **Run `sprint-status.sh` BEFORE `git checkout -b`.** This is now the official rule (Story F4) but it needs to be the muscle memory. Sprint 2c started without running it; after F2 added the script, every subsequent decision (F3, F4, F5) had the data available.

3. **Always check `git add` scope.** The accidental sprint-2/stories.md grab in F4 came from `git add <directory>` picking up unrelated changes. Future commits should `git status --short` before `git add`, or use `git add -p` for non-trivial changes.

4. **`git branch --show-current` at the start of every commit.** A concurrent agent silently switched my branch mid-sprint. The check is one command and would have surfaced the slip immediately.

## Cumulative scorecard

| Sprint | Stories closed | Commits before handoff | Per-story critic | Branch hijacked? | Wiped-tree event? | Working-tree dirty count avg |
|---|---|---|---|---|---|---|
| 1 | 4 | NO (post-mortem) | NO | NO | YES (twice) | n/a |
| 2a | 2 | YES | YES | NO | NO | small (≤2) |
| 2b | 2 | YES | YES | NO | NO | medium (8-12) |
| 2c | 5 | YES | YES | YES (recovered) | YES (one file recovered) | high (>15) |

The pattern: as concurrent activity in the workspace grows (low → medium → high across these four sprints), the only stable recovery path is committing each story before handoff. Sprint 2c proved the protocol works even at high concurrent activity — but also proved that worktree isolation is the next step up, and Sprint 2c-style recovery, while feasible, is not a sustainable steady state.
