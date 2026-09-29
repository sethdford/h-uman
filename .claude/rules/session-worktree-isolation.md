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
5. **A worktree on `main` IS a lock on `main`.** Give a rescue/scratch worktree
   its own branch (`git worktree add <dir> -b <name>`), never the shared branch,
   and remove it as soon as its work is pushed. Git permits a branch in exactly
   one worktree; while yours holds `main`, `git checkout main` in the shared
   checkout fails with *"fatal: 'main' is already checked out at …"* and every
   commit made there lands on whatever branch it was stranded on — including
   other sessions' commits.

### Evidence (2026-07-27) — the lock, and what it cost

Recovering a stranded commit, I created a worktree with `git worktree add -f
/private/tmp/hu-hermetic-fix main`. That was the correct instinct (stop
committing in the shared checkout) executed the wrong way: it took `main`
itself. For the next hour the shared checkout was pinned to another session's
branch, and **two commits from two different sessions** landed there —

| Commit | Author session | Fate |
|---|---|---|
| `498722d40` (test hermeticity) | mine | cherry-picked to `main` as `0bd7cd9aa` |
| `f4e785d52` (eval harness prompt) | a third session's | preserved only because the branch owner spotted it and pushed `rescue/eval-harness-measured-style` |

Neither author noticed at commit time; both were found by the *branch owner*
reading their own log before a force-push. The second had no copy anywhere
else. Cost: two cross-session rescues, a rescue branch, and a force-push that
had to be cleared with two sessions first.

The tell that a push landed somewhere unintended: `git push` reports
**"Everything up-to-date"** right after a successful-looking commit. That means
your commit is real but sits on a branch nobody is pushing to `main`.

**Attributing a stranded commit: read the `Co-Authored-By` trailer, nothing
else.** Every session commits as `Seth Ford <sethford@Mac.lan>` — the machine
identity — so author, committer and `git blame` cannot separate sessions. The
trailer names the model (`Claude Opus 5`, `Claude Fable 5`) and is the only
reliable discriminator. Do **not** infer ownership from topic, file, or "this
continues that other commit": every session reads the same corpus and the same
open problems, so topic continuity is the norm, not evidence. Concretely
(2026-07-27): a stranded eval-harness commit was attributed to the session that
had produced the corpus it measured and had authored the adjacent commit in the
same file — both wrong. The two commits carried *different* model trailers,
which settled it in one line:

```bash
git log -1 --format='%(trailers:key=Co-Authored-By)' <sha>
```

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

## Beyond git: ports and ~/.human state need a single writer too

Worktrees only isolate the REPO. Three incidents on 2026-07-25/26 came from
concurrent sessions mutating shared NON-git state:

1. **The sheet wipe.** One session bulk-filled `~/.human/blind_ab_human/`
   (valid human ratings, Seth as rater via a scribe session); the session that
   owned the sheet saw file-level anomalies, concluded contamination, and reset
   it — nearly destroying the cycle-2 human gate data. Provenance lived only in
   the scribe session's transcript.
2. **The near-double-flip.** Two sessions each held a user instruction about
   promoting the base on :8741; only an explicit "are you executing? reply
   before acting" exchange prevented a race on the live server.
3. **The mid-run kill.** A session killed the :8743 server on a misread idle
   signal while another session's driver was actively generating against it —
   55 generations lost.

The discipline, matching the worktree rule's shape:

- **One owner per shared resource, named in the session bus.** Serving ports
  (:8741 live — NEVER touched without its owner; :8743/:8745/:8747 spares),
  `~/.human/config.json`, `~/.human/blind_ab_human/`, the launchd plists, and
  the adapters registry each have exactly one session that writes them at a
  time. Everyone else reads.
- **Claim before you touch.** Before mutating any of the above, send the
  owning/likely-owning session a message and wait for an ack — or, if idle and
  unowned, announce the claim so the next session finds it. A queued message
  costs a minute; the sheet wipe cost an afternoon.
- **Kills require two idle signals, correctly parsed.** `ps etime` is
  `[[dd-]hh:]mm:ss` (09:14 = nine MINUTES); output-file mtime gaps must exceed
  2x the job's known logging cadence; when in doubt `sample <pid>`. And message
  the owner first — a wedged process can wait five more minutes.
- **Act-on-behalf leaves provenance where the owner will look.** If you write
  another session's files at the user's request (scribe pattern), drop a
  note in the resource dir or the session bus at write time, not after the
  owner notices.

## Related

- `~/.claude/rules/worktree-merge-before-cleanup.md` — never delete a worktree
  before confirming its work landed; squash-merge makes topology lie
- `~/.claude/rules/verify-worktree-isolation-before-fanout.md` — the subagent-level
  version of this same leak
- `.claude/rules/worktree-cwd-resets-in-bash.md` — how to operate once inside one
