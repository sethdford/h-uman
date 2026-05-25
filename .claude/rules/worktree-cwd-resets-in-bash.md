# Worktree CWD Resets — Use Absolute Paths in Bash, Always

Every `Bash` tool call spawns a fresh shell. The previous call's
`cd <worktree>` does NOT persist. A command typed as `pwd && ls` after
a `cd` looks like it'll resolve in that directory but won't — it
resolves in the project root (or wherever the harness defaults to),
which is **probably not the worktree you're trying to operate in**.

This is responsible for an entire class of "I edited the worktree but
the build / test / script ran against main" failures.

## The hazard

Concrete symptom from the 2026-05-25 reactive-iMessage session (one of
four times the same trap fired in one session):

  1. Created worktree at `.claude/worktrees/reactive-imessage`
  2. Edited files in the worktree (good)
  3. Ran `cd .claude/worktrees/reactive-imessage && bash scripts/foo.sh`
     — script ran in the worktree (good)
  4. Next bash call: `bash scripts/foo.sh` (no cd prefix)
     — silently resolved against `/Users/sethford/Projects/h-uman`
     (the project root, NOT the worktree)
  5. Script invoked the MAIN checkout's version of foo.sh, against
     MAIN's state, while my edits sat in the worktree
  6. ~10 minutes wasted debugging why my changes "weren't running"

A second instance the same session: `cmake --build build --target
human_tests -j8` ran against the MAIN checkout's stale build dir
because PWD was main, not the worktree. Tests counted 11912 (matching
a concurrent session's branch state) instead of MY expected count.

## Why the obvious "fix" is wrong

❌ `cd worktree && command` — works for ONE bash call. The NEXT bash
call resets and the trap reappears.

❌ Setting an env var like `WORKTREE=/abs/path` — bash sessions don't
share env. Each `Bash` tool is a clean process.

❌ "I'll remember to cd every time" — five rounds in, I still fell
for it. Discipline isn't the answer; structure is.

## The right shape

**Always reference worktree paths as absolute paths.** Specifically:

  cd /Users/sethford/Projects/h-uman/.claude/worktrees/foo && cmd

becomes

  /Users/sethford/Projects/h-uman/.claude/worktrees/foo/build/human_tests
  bash /Users/sethford/Projects/h-uman/.claude/worktrees/foo/scripts/x.sh

For `git`-related commands, prefer `git -C <abs-path> <subcommand>`:

  git -C /Users/sethford/Projects/h-uman/.claude/worktrees/foo status
  git -C /Users/sethford/Projects/h-uman/.claude/worktrees/foo log -3

For `cmake`, use the absolute build dir explicitly when possible:

  cmake --build /Users/sethford/Projects/h-uman/.claude/worktrees/foo/build \
        --target human_tests -j8

This is the SAFE shape because no shell-state leakage matters. The
command names its directory.

## Detection signal

If a Bash command starts with `cd `, that's a YELLOW FLAG. Either:

1. The command's only purpose IS `cd && one-thing` and the
   one-thing won't be repeated — fine.
2. You expect later commands to inherit the `cd` — RED FLAG, they
   won't. Convert to absolute paths.

A reliable canary: drop a `pwd` as the FIRST step. If you see
`/Users/sethford/Projects/h-uman` when you wanted
`/Users/sethford/Projects/h-uman/.claude/worktrees/foo`, the trap
fired. Re-issue with absolute paths.

## Audit checklist

When working in a worktree:

- [ ] All `Bash` tool calls reference paths via absolute or `-C`
- [ ] No `cd` prefix unless the whole command is one-shot
- [ ] `git status` / `git log` use `git -C <abs-path>` if not 100%
      certain about cwd
- [ ] After every 5–10 Bash calls, drop a `pwd` canary to verify

## Related

- `~/.claude/rules/agent-team-os.md` — worktree isolation pattern;
  this rule is the operational complement that prevents the pattern
  from silently breaking via cwd drift.
- `~/.claude/rules/worktree-merge-before-cleanup.md` — sister rule
  for the END of the worktree lifecycle (merge before delete); this
  rule is for the MIDDLE of the lifecycle (don't lose track of which
  worktree commands are hitting).
- `~/.claude/CLAUDE.md` "Tool Choice" table — `Bash` for "run code"
  is correct; the trap is what to do with the cwd state.
