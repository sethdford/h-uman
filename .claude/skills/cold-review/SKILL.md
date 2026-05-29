# Cold-Diff Review

Review the current branch's diff with a **fresh-context** agent that reads the
change cold — no implementer narrative, no access to the conversation that
produced it. The implementer rationalizes shortcuts ("I'll fix that later",
"this is fine for now"); a reader who only sees `git diff` cannot, and surfaces
every unstated assumption. This is the Session-A/B technique: Session A builds
with full context; Session B reads the diff blind and reports what's actually
on the page.

Distinct from `critic` / `pr-review-toolkit` (which run *with* session context
and look for code-quality issues). The point here is the **absence** of
context — the reviewer is told nothing about intent beyond what the diff and a
one-line goal reveal.

## When to use

- Before committing a non-trivial change, especially one you implemented with
  lots of in-session back-and-forth (the shortcuts hide in that narrative).
- Before opening a PR, as the last gate after `/verify`.
- When you suspect you've been rationalizing — "works on my machine" energy.

NOT for: trivial diffs, pure formatting, or changes already cold-reviewed.

## Steps

### 1. Capture the diff and a one-line intent

```bash
git diff --merge-base main        # or: git diff <base>..HEAD
git diff --stat --merge-base main # scope summary for the reviewer
```

Write ONE sentence of intent (what the change is *supposed* to do). That single
sentence is the ONLY context the reviewer gets — everything else must be
inferable from the diff itself.

### 2. Dispatch a fresh-context reviewer

Spawn a subagent (`Agent`, `subagent_type: general-purpose` or `critic`) whose
prompt contains ONLY:

- the one-line intent,
- the full diff (paste it, or have the agent run the `git diff` itself),
- the instructions below.

Do **not** include the implementation conversation, your reasoning, or "what I
was trying to do." The blind read is the whole value.

Reviewer instructions to embed:

> You are reading this change COLD. You did not write it and have no context
> beyond the one-line intent. Read the diff and report, with `file:line`:
> 1. **Shortcuts** — anywhere the code does the easy thing, not the right
>    thing (hardcoded values, TODOs, "good enough" paths, skipped error
>    handling).
> 2. **Unstated assumptions** — what must be true for this to work that the
>    diff does not establish or check (non-null, ordering, a migration ran,
>    a config key exists).
> 3. **Half-fixes** — changes that address a symptom, not the cause.
> 4. **Missing edge cases** — NULL/empty/overflow/concurrency the diff
>    doesn't cover.
> 5. **Test gaps** — new behavior with no test, or a test that asserts the
>    old/wrong behavior (see tests-that-pin-bugs).
> For each finding give a confidence (high/medium/low). Report ONLY what the
> diff shows — do not invent context. If the diff is clean, say so plainly.

### 3. Triage findings into tasks

- High-confidence shortcuts / half-fixes / test gaps → fix before commit.
- Unstated assumptions → either add the guard/test, or document why it holds.
- Low-confidence items → judgment call; note them in the commit body if you
  ship anyway.

### 4. Re-run if you changed code

Any fix changes the diff — cold-review again only if the new code is itself
non-trivial. Don't loop forever; one or two passes is the norm.

## Notes

- This composes with `/verify` (which *runs* the code) — cold-review reads,
  verify executes. Do both for release-blocking changes.
- For a diff that spans many files, dispatch one reviewer per subsystem so each
  reads a coherent slice cold, then merge findings.

## Related

- `.claude/rules/ground-truth-over-proxy-signals.md` — why a blind read beats
  trusting the implementer's (or an agent's) own account.
- `.claude/rules/tests-that-pin-bugs.md` — the test-gap finding (#5) leans on
  this: a test that locks the wrong behavior is worse than none.
- `~/.claude/rules/audit-verify-before-allege.md` — the reviewer must report
  what the diff shows, not infer "missing" without evidence.
