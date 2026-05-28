# Critic findings — retire reflection patterns on negative reaction (commit 4fb2b621)

Status: COMPLETE — adversarial review complete.

## Findings

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
