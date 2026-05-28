# Critic findings — retire reflection patterns on negative reaction (commit 4fb2b621)

Status: CLOSED — adversarial review complete; all findings verified against
source and DISPOSED. No code change warranted.

## Verification disposition (2026-05-28)

Each finding below was re-checked by reading the actual source per the
`audit-verify-before-allege` discipline. All five are false positives or
tested-and-documented design choices. Editing working, tested code to
satisfy these would add regression risk for zero benefit, so no change
was made. Evidence:

| # | Sev | Verdict | Evidence |
|---|-----|---------|----------|
| 1 | HIGH | DESIGN, not bug | Crash-safety is handled: the UPDATE carries `WHERE retired = 0` (consumer.c:389), so an orphaned surfacing after a crash cannot re-retire an already-retired pattern. The over-broad `DELETE WHERE channel=?` is the intended "consume the channel ledger on thumbs-down" semantics; the channel-scoped+recency-windowed blast radius is documented at test:264-267 and pinned by test:269. |
| 2 | HIGH | FALSE POSITIVE | `int retired = sqlite3_changes(db);` is at consumer.c:401, which runs BEFORE the DELETE is prepared at :405. `sqlite3_finalize()` destroys a prepared statement; it does NOT reset the connection change counter. `sqlite3_changes()` reports the most recently *completed* DML — the UPDATE — so `retired` is the correct UPDATE row count. |
| 3 | MEDIUM | FALSE POSITIVE | The null guard already exists: `if (e->polarity == HU_REACTION_NEGATIVE && s_reflection_db)` at reaction_handler.c:297. A cleared/unset db is a no-op by construction. |
| 4 | MEDIUM | FALSE POSITIVE | The DELETE step IS error-checked and logged: `if (sqlite3_step(st) != SQLITE_DONE) hu_log_error(...)` at consumer.c:411-412. Non-propagation to the caller is intentional and documented (consumer.c:408-410): a failed consume leaves harmless orphans that the retired=0 guard neutralizes. |
| 5 | LOW | FALSE POSITIVE | The concurrent-turn blast-radius test already exists: `test_same_channel_thumbs_down_retires_all_in_window` (test:269) inserts pA (turn A) + pB (turn B), surfaces both in-window, thumbs-downs once → asserts both retired, lineage consumed, and a second thumbs-down returns 0 (no re-retire). |

RESULT_critic_disposition=ALL_FALSE_POSITIVE_OR_BY_DESIGN

Reflexion note: a review that produces 5/5 non-actionable findings against
tested code is a `tune-agent` candidate — the critic alleged without
reading the surrounding guards, comments, and existing tests. The
`audit-verify-before-allege` rule names exactly this failure mode.

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
