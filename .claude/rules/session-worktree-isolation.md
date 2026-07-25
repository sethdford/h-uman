# Each Session Works In Its Own Worktree — Creating One Isn't Enough, You Must Stay In It

Every Claude session in this repo already gets a worktree at
`.claude/worktrees/<session-name>` on branch `claude/<session-name>`. The failure
mode is **not** a missing worktree — it's that sessions abandon it and do their
real work in the shared main checkout at `/Users/sethford/Projects/h-uman`.

## The hazard

When two sessions commit in the shared checkout at once, three things break, and
none of them announce themselves:

1. **HEAD moves under you mid-operation.** You read `git log`, start a push, and
   by the time it runs the branch tip is someone else's commit.
2. **You can push another session's in-flight work.** Their merge lands on *your*
   checkout's `main`, so `git log origin/main..HEAD` shows commits you never
   wrote — and a `git push` publishes them.
3. **The working tree is shared.** Anything a hook regenerates (the pre-push
   stats sync rewrites `AGENTS.md`, `CLAUDE.md`, `README.md`, `CONTRIBUTING.md`,
   `PROJECT_STATUS.md`, `human/STUBS.md`) shows up dirty in the other session's
   `git status`.

### Evidence (2026-07-25)

Two sessions ran concurrently. **Both had worktrees**
(`.claude/worktrees/intelligent-edison-720d01`, `.../modest-agnesi-369cac`) and
**both worked in the shared main checkout anyway.** Result: three consecutive
lost push races. Each `git push` runs the pre-push hook (full rebuild + 13,905
tests, several minutes), and the other session pushed during every single window:

| Attempt | Outcome |
|---|---|
| 1 | Succeeded, but remote had already moved `64d50a4a0 → f66863e15` underneath |
| 2 | `! [remote rejected] cannot lock ref 'refs/heads/main'` — suite passed, ref raced |
| 3 | Nothing to push; the authoring session had already pushed it |

Between attempts, the *other* session's Binoculars commits merged into the shared
checkout's `main`, so they appeared as "my" unpushed work. Publishing another
session's mid-work commits was one `git push` away and nearly happened.

This is the session-level twin of the subagent leak in
`~/.claude/rules/verify-worktree-isolation-before-fanout.md` (third confirmation,
2026-05-31): a worktree was created, then work landed in the main repo anyway.

## Why the obvious fixes are wrong

❌ **"Create a worktree per session."** Already happens — 22 exist, 16 GB of them
(most carry a ~1 GB `build/`). Creating #23 adds a gigabyte and changes nothing.

❌ **"Hard-block commits to main in the shared checkout."** Would have blocked the
same day's three urgent red-CI fixes, which legitimately belonged on `main`. The
gate must not be more expensive than the collision.

❌ **"Just remember to use the worktree."** Two independent sessions, each with a
worktree already provisioned, both forgot on the same day. This is the exact
"CLAUDE.md is suggestions, hooks are guarantees" case from `~/.claude/CLAUDE.md`.

## The right shape

1. **Start file-editing work with `EnterWorktree`.** Commit there, then merge to
   `main` from the shared checkout in one short window — merging is seconds, so
   the race window is seconds instead of the multi-minute pre-push build.
2. **Use absolute paths / `git -C <worktree>`** once you're in one; every Bash
   call gets a fresh shell and `cd` does not persist
   (`.claude/rules/worktree-cwd-resets-in-bash.md`).
3. **Confirm a reported commit is really on your branch** before trusting it:
   `git -C <worktree> log --oneline -1`. A SHA that isn't your tip means the work
   leaked to the main checkout.
4. **Read-only sessions don't need a worktree.** Analysis, audits, and
   log-reading collide with nothing — the cost is only justified when you write.

## Enforcement

`scripts/check-session-worktree.sh`, wired into `.githooks/pre-commit`. It fires
only when **all** of these hold: you are in the shared main checkout (not a linked
worktree), on `main`, and at least one `.claude/worktrees/*` HEAD is not yet an
ancestor of `origin/main`.

Advisory by default — it prints the colliding worktrees and exits 0. Set
`HU_WORKTREE_STRICT=1` to make it refuse the commit.

## Housekeeping this rule does NOT do

The 22 worktrees / 16 GB backlog (oldest 2026-05-29) is **not** cleaned up here.
Removal requires the verification ladder in
`~/.claude/rules/worktree-merge-before-cleanup.md` — and because this repo
squash-merges, an ancestry check reports safely-merged branches as unmerged, so
**PR state is the only valid oracle**:

```bash
gh pr list --head <branch> --state all --limit 1 --json number,state
```

## Related

- `~/.claude/rules/worktree-merge-before-cleanup.md` — never delete a worktree
  before confirming its work landed; squash-merge makes topology lie
- `~/.claude/rules/verify-worktree-isolation-before-fanout.md` — the subagent-level
  version of this same leak
- `.claude/rules/worktree-cwd-resets-in-bash.md` — how to operate once inside one
