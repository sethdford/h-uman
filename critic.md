# Critic findings — retire reflection patterns on negative reaction (commit 4fb2b621)

Status: CLOSED — findings verified against the COMMITTED blob and disposed.
3 false-positive/by-design, 2 genuine low-severity (both fixed in df663986).

## CORRECTION (2026-05-28, supersedes the first disposition)

The first disposition of this file (commit d64d7660) wrongly tagged
findings #4 and #5 as FALSE POSITIVE. Root cause: that review read the
WORKING-TREE files, but a parallel agent had already made — and not yet
committed — the exact hardening edits for #4 and #5. Reviewing the dirty
working tree instead of `git show 4fb2b621:<path>` showed someone else's
uncommitted fixes and made the genuine findings look pre-handled.

Re-checked against the committed blob `4fb2b621`:
- #4 DELETE-unchecked: VALID. In 4fb2b621 the consume DELETE was a bare
  `sqlite3_step(st);` with no error check. Low-severity (orphans are
  harmless under the retired=0 guard), but real. Fixed in df663986.
- #5 missing blast-radius test: VALID. `test_same_channel_thumbs_down_
  retires_all_in_window` did NOT exist in 4fb2b621 (`grep -c` = 0). Added
  in df663986 as AC-6, pinning the channel-scoped blast radius as intended.

## Verification disposition (corrected)

| # | Sev | Verdict | Evidence |
|---|-----|---------|----------|
| 1 | HIGH | DESIGN, not bug | Crash-safety is handled: the UPDATE carries `WHERE retired = 0` (consumer.c:389), so an orphaned surfacing after a crash cannot re-retire an already-retired pattern. The over-broad `DELETE WHERE channel=?` is the intended "consume the channel ledger on thumbs-down" semantics; blast radius documented + pinned (df663986 AC-6). |
| 2 | HIGH | FALSE POSITIVE | `int retired = sqlite3_changes(db);` is at consumer.c:401, which runs BEFORE the DELETE is prepared at :405. `sqlite3_finalize()` destroys a prepared statement; it does NOT reset the connection change counter. `sqlite3_changes()` reports the most recently *completed* DML — the UPDATE — so `retired` is the correct UPDATE row count. |
| 3 | MEDIUM | FALSE POSITIVE | The null guard already exists in 4fb2b621: `if (e->polarity == HU_REACTION_NEGATIVE && s_reflection_db)` at reaction_handler.c:297. A cleared/unset db is a no-op by construction. |
| 4 | MEDIUM→LOW | VALID, fixed | 4fb2b621 had a bare `sqlite3_step(st)` for the consume DELETE. df663986 added the `!= SQLITE_DONE` check + `hu_log_error`. Severity is LOW not MEDIUM: orphans are neutralized by the retired=0 guard, so the only impact is operator visibility of a persistent I/O fault. |
| 5 | LOW | VALID, fixed | The blast-radius test did not exist in 4fb2b621. df663986 added `test_same_channel_thumbs_down_retires_all_in_window` (pA + pB surfaced in-window → one thumbs-down retires both, lineage consumed, second thumbs-down returns 0). |

RESULT_critic_disposition=2_HIGH_FALSE_POSITIVE_1_MEDIUM_FALSE_POSITIVE_2_LOW_VALID_FIXED

Reflexion notes:
1. The critic's two HIGH findings were both false positives (it alleged
   without reading the surrounding guard + the sqlite3_changes ordering) —
   a `tune-agent` candidate per `audit-verify-before-allege`. But it was
   RIGHT about the two low-severity gaps.
2. My own first disposition erred by reviewing the working tree while a
   parallel agent held uncommitted edits. Lesson: to review commit X,
   read `git show X:path`, never the live working-tree file, whenever
   concurrent agents may have dirty state.

## Original findings (preserved for audit trail)

| Severity | file:line | concern | suggested fix |
|----------|-----------|---------|---------------|
| HIGH | src/reflection/consumer.c:381-412 | DELETE-after-UPDATE not transactional; if crash between lines 400-410, surfacing rows persist and re-retire patterns on next thumbs-down in same channel. Also: DELETE WHERE channel=? removes ALL surfacings for that channel, not just the retired ones — patterns surfaced-but-not-yet-retired or from concurrent turns lose lineage. | Wrap both queries in explicit `BEGIN TRANSACTION...COMMIT` OR collapse to single UPDATE-with-subquery-delete. Test the crash path. Clarify intent: are concurrent-turn surfacings re-retiring intended or a bug? |
| HIGH | src/reflection/consumer.c:401 | sqlite3_changes(db) read AFTER finalize of the UPDATE statement (line 400). If DELETE statement's prepare/step occurs before changes() is queried, changes() may return DELETE count instead. | Capture `int retired = sqlite3_changes(db);` immediately after UPDATE's finalize, before preparing DELETE. |
| MEDIUM | src/agent/reaction_handler.c:297-300 | s_reflection_db is a static global with implicit daemon lifecycle assumption. Reaction handler fires on every inbound reaction; if lifecycle desync occurs (e.g. test harness clears db mid-turn), silent no-op with no explicit guard. | Add defensive `if (!s_reflection_db) return HU_OK;` at line 297 to make the contract explicit. |
| MEDIUM | src/reflection/consumer.c:405-410 | DELETE statement is never error-checked. If DELETE fails (SQLITE_IOERR, etc.), surfacing rows leak silently and caller doesn't know retire was partial (UPDATE succeeded, DELETE failed). | Wrap DELETE in error check and either return error code or emit log_error + return retired count to acknowledge partial failure. |
| LOW | tests/test_reflection_retire_on_contradiction.c | Tests do NOT pin concurrent-turn blast-radius or DELETE-after-UPDATE ordering. Missing test case: two patterns surfaced from different turns in same channel → one thumbs-down retires both. | Add test: insert pattern_A (T1), insert pattern_B (T2), surface both, thumbs_down → verify both retired AND no re-retire on next thumbs_down. Pins whether concurrent retirement is intended. |

## Cross-agent regression risk

(none observed)

RESULT_critic=HAS_FINDINGS_0_4
