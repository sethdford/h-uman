# US-9: Resolve Sprint 3 Pattern C DoD literalism — annotate fallbacks

## Approach
PO ruling: option **(b)** — keep the two `hu_conversation_strip_channel_tags` calls in `daemon.c` (lines 2127, 2137) as defense-in-depth fallbacks; annotate them so future readers understand they are intentional, not survivors of an incomplete migration.

Lines 2127 and 2137 sit in the `chain_ok == false` and `else (no allocator)` arms respectively — both are reachable only when the primary chain path fails to build or execute. They are correct as defensive code. The audit flag was a DoD-text mismatch, not a behavioral defect.

Add a 6-8 line block comment above the first occurrence (line 2127) and a one-line cross-reference above the second (line 2137). Comment text follows the conventional pattern from `assistant_stream.c`:

```
/* Defensive fallback (Sprint 3 US-2 / Sprint 4 US-9):
 * The primary outbound path uses hu_output_validator_chain_execute above.
 * This strip survives only when the chain failed to build/execute (e.g.,
 * allocation failure mid-stream) or when no allocator is available. Do not
 * remove without restoring an equivalent safety net — see audit notes in
 * sprints/sprint-4/notes-from-sprint-3.md. */
```

Update Sprint 3 audit notes: write `sprints/sprint-4/notes-from-sprint-3.md` containing the disposition, with a backlink. Update Sprint 3 `stories.md` DoD line for US-2 in-place to match the actual invariant: "primary AGENT_STREAM_TEXT path uses chain; legacy strip survives only in chain-build-failure and no-allocator fallback arms (documented in source)."

## Files to modify / create
| File | Change | LOC |
|---|---|---|
| `src/daemon.c:2127` (and `:2137`) | block comment + one-liner | +10 |
| `sprints/sprint-3/stories.md` (US-2 DoD line) | update DoD text | ±2 |
| `sprints/sprint-4/notes-from-sprint-3.md` (new) | audit disposition note | +25 |

## Implementation steps
1. Read current `daemon.c:2120-2140` to confirm line numbers (may have drifted).
2. Insert the block comment + one-liner. Re-build.
3. Write `notes-from-sprint-3.md` referencing the audit and naming the disposition.
4. Edit Sprint 3 `stories.md` US-2 DoD line. Commit message: `docs(sprint-3): clarify US-2 DoD per Sprint 4 US-9 audit resolution`.
5. Run `./build/human_tests --suite=pattern_c_paths` (must remain 5/5).

## Risks
- **Line drift (LOW/SMALL)**: lines may have shifted post-merge of Sprint 3 work. Re-grep before editing: `grep -n "hu_conversation_strip_channel_tags" src/daemon.c`. Mitigation: pin by surrounding context, not raw line number.
- **Backward compat (NONE)**: comment-only change in source; markdown-only in sprint docs.
- **Audit closure (LOW)**: ensure the audit-note file is referenced from the Sprint-3 close artifact if one exists; otherwise the disposition is orphaned.

## Test strategy
- AC-9.3: `./build/human_tests --suite=pattern_c_paths` returns 5/5.
- No new test required (no behavior change).
- Manual: grep `daemon.c` for the new comment block, confirm `strip_channel_tags` calls now have adjacent doc.

## AC mapping
- AC-9.1 → option (b): comment block added (lines 2127, 2137)
- AC-9.2 → Sprint 3 `stories.md` US-2 DoD updated
- AC-9.3 → existing suite still passes

## Effort
**XS** — 5-10 lines of code + 2 markdown edits + new note file. ~40 LOC total. No risk to behavior.
