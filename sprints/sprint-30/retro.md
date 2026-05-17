# Sprint 30 — retro

## What went well

- **The audit found leaks the user didn't know about.** Going
  beyond "fix the one Brea leak" and running a full chat.db sweep
  on all outbound since 2026-05-10 surfaced THREE additional
  leaks (one to a second recipient on the same day, two to a
  third contact on a different day with a different leak shape).
  Without the audit, Sprint 30 wouldn't exist and those leak
  shapes wouldn't be caught.

- **Iteration on Sprint 29 was the right move.** Sprint 29
  closed when the Brea leak was caught. Then we audited and
  found we had a coverage gap. Sprint 30 closed THAT gap. The
  alternative — declaring "ship it, we caught the obvious leak"
  — would have left the May-11 template-label leak shape in
  production indefinitely.

- **PII restraint won.** First draft of Sprint 30 hardcoded
  "Seth Douglas Ford" and "Chief Architect" as guard substrings.
  That's safe for THIS deployment but bakes Seth's PII into
  open-source guard code. Removed in favor of structural
  template-label patterns. The 4 audit leaks each match 4+
  structural patterns, so coverage didn't drop. A future sprint
  can make this dynamic by reading the loaded persona's fields.

- **Verbatim regression tests for all 4 audit leaks.** Even
  Sprint 29's tests are reinforced — the second 17:07:38
  recipient variant is now pinned, ensuring the same content sent
  to a different recipient is also caught.

## What was hard

- **Reading attributedBody from chat.db.** Modern macOS stores
  iMessage content as typedstream NSAttributedString in
  `attributedBody`, not in `text`. The naive `SELECT text` query
  returns empty for most modern messages. Required `writefile()`
  to extract bytes, then `strings -n 4` to find printable runs.
  Worked, but the `text` vs `attributedBody` discrepancy is the
  kind of thing that hides leaks.

- **Shell-loop kept eating exit codes.** First two attempts at
  the audit loop silently exited mid-iteration. Switched to a
  proper bash script file (`/tmp/imsg-audit.sh`) and it ran
  cleanly. Lesson: complex pipelines belong in a script, not
  inline.

- **Date math on chat.db.** Apple's date format is nanoseconds
  since 2001-01-01, not seconds since 1970. `m.date/1000000000 +
  978307200` converts. Initial filter using `strftime('%s', 'now',
  '-24 hours')` returned zero rows because of integer truncation
  in the conversion. Switched to a numeric date threshold
  (`m.date > 800000000000000000`) which is guaranteed to match.

## What surprised us

- **The leak count was 4x what the user reported.** User
  mentioned the Brea leak. Audit found 3 more. Two of them were
  a completely different leak shape (template labels) that
  Sprint 29's detectors miss entirely.

- **The 17:07:38 second-recipient leak.** Same content sent to
  a DIFFERENT contact 3 minutes after the Brea leak — and that
  content explicitly references Brea by name. The model's
  prompt context didn't change between recipients; it just kept
  dumping the same self-narration to whoever was next in the
  queue. This points to a per-turn model-state hygiene issue
  upstream of the guard — probably worth its own sprint after
  the safety patches.

- **Candidate response drafts in the leaks.** msg 56055 ends
  with `"ha i'll take that as a compliment i guess"` /
  `"it's just math but i'll take it"` / `"still just code
  though. but thanks i guess"` — the model's draft alternatives
  shipped to the recipient instead of just one chosen reply.
  That's a different failure mode than CoT leak: it's a
  selection-step skip.

## New carry-overs

- **Persona-derived dynamic detector.** Read the loaded
  persona's `name` / `title` / `age` / `pronouns` fields and
  reject verbatim quotes. Generalizes to every user's
  deployment.

- **Per-turn model-state hygiene.** Why was the same context
  dumped 3 minutes apart to two different recipients? The
  agent loop should reset (or at least not echo) the per-turn
  prompt context across turns. Worth investigating
  `src/agent/agent_turn.c` and the streaming path.

- **Selection-step missing.** Why did msg 56055 ship 3
  candidate drafts instead of picking one? Either the
  candidate-generation step is leaking its workspace, or the
  selection step was bypassed. Worth a code-trace.

- **Audit script as a recurring tool.**
  `scripts/audit-imessage-leaks.sh` would be useful for
  weekly CI / cron — run the same scan against the last N
  messages and alert on any G1/G2/G3/G4 hits. Currently
  one-shot in `/tmp/`.

- **Daemon restart.** This sprint added new detectors but the
  running daemon still has the Sprint 29 build until restarted.

## Process notes

- **The user's "DO it all!" was the right scoping.** They
  wanted everything shipped, not just the bare-minimum patch.
  The audit + Sprint 30 + the queued Sprints 31/32 + post-
  mortem fall under that umbrella. Each sprint delivers
  independent safety value; together they form the layered
  defense the SOTA goal requires.

- **The audit became its own deliverable.** What started as a
  carry-over ("audit Brea thread for other sends") expanded
  to a full chat.db sweep. The sweep results justified
  Sprint 30's existence. Without the audit, the May-11 leak
  shape would have shipped silent.
