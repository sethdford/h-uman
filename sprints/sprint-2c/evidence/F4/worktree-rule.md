---
title: "Sprint 2c — Story F4 evidence: SCRUM Phase 0 worktree rule"
created: 2026-05-12
sprint: 2c
story: F4
---

# Story F4 evidence — SCRUM Phase 0 prefers worktrees when concurrent activity is high

## Files changed (outside repo)

- `~/.claude/skills/scrum/SKILL.md`
- `~/.claude/agents/scrum-master.md`

## Why this matters

Sprint 2a's retro and Sprint 2b's retro both flagged the same risk: branch isolation alone isn't enough when concurrent agents are aggressively rewriting git refs. Three real failure modes have now been observed:

1. **Sprint 1 (`feat/sota-m1-infra`):** concurrent `git reset --hard HEAD` from another agent wiped the working-tree contents twice during the sprint.
2. **Sprint 2b (`sprint-2b-personal-model-honesty`):** the sprint branch ran clean, but a concurrent agent later **renamed the branch tip** to point at unrelated work (`docs(plan): author RL SOTA Phase 2`). My sprint-2b commits (`71de40e6`, `c47a43b5`, `2eebde7c`) survived only as detached objects; `git log sprint-2b-personal-model-honesty` no longer reaches them.
3. **Sprint 2b (working tree):** the same workspace had 18 dirty files from concurrent agents during the sprint, any one of which could have been blown away by `git stash drop` or `git checkout -- .`.

A worktree (`git worktree add ../human-sprint-N -b sprint-N-slug`) defeats all three. The other agents can `git reset` the original repo's branches all they want — the worktree has its own working directory, its own HEAD, and its own files. The cost is one extra checkout (~few seconds) and an extra `cd`.

## Rule additions

### `~/.claude/skills/scrum/SKILL.md` Phase 0

Renamed from "Branch isolation" → "Branch / worktree isolation".

Decision matrix added:

| Signal | Recommended isolation | Why |
|---|---|---|
| `>5 unrelated dirty files` | **Worktree** | A `git stash` wipe could lose concurrent agent work |
| `>1 sprint-* branch with recent commits` | **Worktree** | Multiple sprints sharing one index — Sprint 1 was wiped twice this way |
| `>1 active worktree already` | **Worktree** | Mixing branch + worktree in one repo is the same risk surface |
| Otherwise (calm workspace) | **Branch** | Lighter weight; sufficient when you control the workspace |

Step 0a now requires running `scripts/sprint-status.sh` (Sprint 2c F2 deliverable) which prints all four signals at once. Step 0c records both the branch name AND the working directory path in `sprints/sprint-N/plan.md` so a future audit can verify the sprint stayed in its lane.

### `~/.claude/agents/scrum-master.md` Phase 0

Mirrored the decision matrix into the agent's protocol. The trigger language changed from "verify the current branch" to "assess concurrent activity, then choose isolation level" so the agent has a deterministic procedure rather than a default.

## Verification

```
$ rg -c "worktree" ~/.claude/skills/scrum/SKILL.md ~/.claude/agents/scrum-master.md
~/.claude/skills/scrum/SKILL.md:10
~/.claude/agents/scrum-master.md:5

$ rg -l "Phase 0: Branch / worktree isolation" ~/.claude/
.claude/agents/scrum-master.md
.claude/skills/scrum/SKILL.md
```

Both rule sources updated; ten worktree references in SKILL.md (Phase 0 plus matrix), five in scrum-master.md (Phase 0 plus DoD). Phase 0 title changed in both.

`RESULT_implementer=DONE story=F4 commit=<this-file-+-rule-update>`
