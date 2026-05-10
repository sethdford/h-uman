# Memory Conflict Resolution

When a new observation contradicts a stored fact, the cheap mistake is to overwrite. The right move is to keep both, with time, and let later evidence settle it.

## When to Use
- A new fact has the same subject + relation type as an existing one but a different object.
- A confident user statement contradicts a low-confidence inferred fact (or vice versa).
- An observation could be a *correction* ("Actually it was 2022, not 2023") or an *update* ("She left Acme last month").

## Workflow
1. **Classify the contradiction first.** Single-valued relations (`works_at`, `lives_in`) typically *supersede* — only one is currently true. Multi-valued ones (`knows`, `interested_in`) *branch* — both can coexist.
2. **For supersession:** close the old row with `event_end = cutover_ts`, link the new one via `supersedes_id`. Don't delete. Yesterday's truth is still part of the user's history.
3. **For low-confidence vs. high-confidence contradiction:** flag the new row instead of writing it live. Quarantine until AutoDream review.
4. **Don't average.** Two contradictory facts averaged into a third lukewarm one is worse than either alone — it's now wrong *and* uncalibrated.
5. **Surface to the user when ambiguous.** "I had her at Acme but you mentioned Globex earlier — has she moved?" beats silently picking one.

## Anti-patterns
- Hard-deleting the prior fact ("they updated, so the old one is wrong") — destroys auditability.
- Treating every disagreement as a correction instead of an update.
- Letting the most recent write always win regardless of source quality.

## Examples
**Example 1:** Two months ago user said Sara works at Acme. Today they mention Globex. → SUPERSEDE. Close `works_at(Sara, Acme)` at today's timestamp, insert new row with supersedes_id. Both visible in window queries: "where did Sara work in March?" still returns Acme.

**Example 2:** Web scraper says "Alice lives in Tokyo," user-typed history says "Alice lives in Seattle (high confidence)." → FLAG. Do not write to live graph. AutoDream will revisit with more context.
