---
title: "Sprint 2a — Story 0 evidence: SCRUM skill protocol fix"
created: 2026-05-11
sprint: 2a
story: 0
---

# Story 0 evidence — SCRUM skill protocol fix

## Files changed (outside repo)

These edits live in the home-directory Claude Code skill/agent registry, not in this repo:

- `~/.claude/skills/scrum/SKILL.md`
- `~/.claude/agents/scrum-master.md`

This evidence file captures the rationale and the rule additions, so a future reader of the sprint can reconstruct exactly what changed even if the home-directory copy diverges.

## Why these rules

Sprint 1 ("fidelity-followthrough") shipped 3 stories DONE + 1 DESCOPE_OK, but the path to that result was much harder than it needed to be:

1. **Wiped-tree event ×2.** While Sprint 1 was running on the shared `feat/sota-m1-infra` branch, two concurrent agents on the same branch ran `git reset --hard HEAD` to clean their own state. Both events wiped Sprint 1's uncommitted working-tree progress. The mid-sprint sprint-auditor caught this (FAIL verdict), forcing a manual re-application of every story onto a dedicated `sprint-1-fidelity-followthrough` branch.
2. **Batched-critic shipped a broken regex.** The Story B implementer's hermetic test driver papered over a BSD-grep bug in `empty_response_set()` — `grep -q '[^"\[\] ,]'` mis-parses on macOS BSD grep, so the publish block in `scripts/lora-runner-ab.sh` was literally unreachable in production. Critic was run at sprint end (after all stories closed), caught it, and we fixed it as part of close. If critic had run per-story (immediately after the implementer's DONE report), the fix would have landed before the next implementer built on top of broken code.
3. **Working-tree-only DONE reports.** Implementers reported DONE based on the working-tree state at their run. Without a commit, the next implementer's `git reset` (or rebase, or stash drop) wiped the work. The SCRUM protocol did not require commit-before-handoff.

## Rule additions

### `~/.claude/skills/scrum/SKILL.md`

**Added Phase 0 (Branch isolation):**

```
### Phase 0: Branch isolation (mandatory pre-step)
Before invoking product-owner, ensure the sprint has its own branch.

- If `git branch --show-current` returns a `sprint-<N>-*` branch: continue.
- Otherwise: create one. Default name is `sprint-<N>-<short-slug-of-goal>`. Use `git checkout -b sprint-<N>-<slug>` from a stable base (usually `main` or the previous sprint's tip).
- If the working tree has unstaged changes that aren't the sprint's: `git stash push -m "pre-sprint-<N>"` first.
- Verify there are no concurrent agents already running on the same branch: a quick check is `ls .git/worktrees/` and confirming no other agent's `sprint-<N>` matches.

The sprint-master writes the chosen branch name to `<project>/sprints/sprint-<N>/plan.md` and surfaces it in every standup. If the branch name later changes, that's a process violation — flag it.
```

**Added three new "Hard rules":**

- Sprint runs on a dedicated branch from planning. Citing Sprint 1's wiped-tree event.
- Implementer commits before handoff. Citing the next-implementer-reset failure mode.
- Critic runs immediately after each story closes, not at sprint end. Citing the BSD-grep regex bug that batched-critic shipped.

**Tightened Phase 2 (Execute):**

- Implementer prompts MUST include "commit before reporting DONE" instruction.
- Scrum-master verifies the commit landed via `git log <sprint-branch> ^<sprint-base>` before advancing.
- Critic spawn is per-story, immediately after verifier PASS, BEFORE aspect-panel.
- HIGH/CRITICAL critic findings re-open the story; aspect-panel does not run on un-criticked code.

### `~/.claude/agents/scrum-master.md`

**Added Phase 0** mirroring the skill, with explicit cite to Sprint 1's `feat/sota-m1-infra` shared-branch incident.

**Added implementer prompt requirement** in Phase 2:
> "Commit your work to `<sprint-branch>` via `git add <paths> && git commit -m \"feat(...): ...\"` BEFORE reporting DONE. Working-tree-only DONE reports will be rejected and the story will re-open."

**Added DoD checks** in Phase 4:
- Commit existence check (`git log <sprint-branch> ^<sprint-base> --oneline`)
- Per-story critic check (run immediately after DONE, not batched)

**Added three new anti-patterns:**
- Closing stories without an implementer commit
- Running the sprint on a shared feature branch
- Batching critic at sprint end

## Verification

```
$ rg -l "Phase 0: Branch isolation" ~/.claude/skills/scrum/ ~/.claude/agents/scrum-master.md
~/.claude/skills/scrum/SKILL.md
~/.claude/agents/scrum-master.md

$ rg -l "Implementer commits before handoff" ~/.claude/skills/scrum/SKILL.md
~/.claude/skills/scrum/SKILL.md

$ rg -l "commit-existence" ~/.claude/agents/scrum-master.md
~/.claude/agents/scrum-master.md
```

All three rule additions verified to land. AC-0.1, AC-0.2, AC-0.3, AC-0.4 satisfied.

`RESULT_implementer=DONE story=0 commit=<this-file-+-stories.md>`
