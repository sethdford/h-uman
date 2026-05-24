# Theory-of-Mind Activation — Tasks

| # | Task | ACs | Owner | Status |
|---|---|---|---|---|
| 1 | Wire `hu_tom_detect_user_expectation()` into pre-turn assembly path around `daemon.c:8880` (before the existing t1b TOM block). Single call site; fires on every inbound user message. | AC-TOM-1 | TBD | pending |
| 2 | Implement `hu_tom_record_user_expectation(contact_id, topic, expected_knowledge_type, session_key, turn_number)` in `src/agent/theory_of_mind.c`. Writes to new `tom_user_expectations` table. Idempotent on `(contact_id, topic, session_key)` per the UNIQUE constraint. | AC-TOM-2 | TBD | pending |
| 3 | Schema migration: add `tom_user_expectations` table (see design.md for SQL). Bump schema version. Add an idx on `(contact_id, resolved_ts_ms)` for the "unresolved" lookup. | AC-TOM-2 | TBD | pending |
| 4 | Extend `hu_tom_belief_t` schema with `session_key` (TEXT, nullable) and `turn_number` (INTEGER, nullable). Schema migration: `ALTER TABLE` adding both columns with NULL default. Update belief-recording code at the existing post-turn capture point (around `daemon.c:10627-10637`) to populate them from current context. | AC-TOM-4 | TBD | pending |
| 5 | Extend `hu_tom_build_context()` in `src/agent/theory_of_mind.c` to add an "Unmet User Expectations" section after the existing "Contact Mental Model" block. Lists expectations where no matching belief exists for the contact. Empty list → section omitted. | AC-TOM-3 | TBD | pending |
| 6 | Schema migration: add `tom_self_change_events` table (see design.md for SQL). Index on `(contact_id, timestamp_utc_ms)`. | AC-TOM-5 | TBD | pending |
| 7 | Implement `hu_tom_record_self_change_event(contact_id, event_kind, session_key, turn_number, magnitude)` and hook into the persona-delta application path (`hu_persona_delta_apply()` or equivalent). | AC-TOM-5 | TBD | pending |
| 8 | Hook self-change recording into adapter-swap success: invoked from `hu_mlx_admin_swap_adapter()` on success branch. **Depends on Spec 1 AC-M3-3** (swap observability). Until that lands, this hook is wired but no-ops since failures aren't distinguishable from successes. | AC-TOM-5 | TBD | pending |
| 9 | Hook self-change recording into emotional-register transitions: invoked from the existing register-change detection point. Event_kind `REGISTER_SHIFT`, magnitude = σ of the shift. | AC-TOM-5 | TBD | pending |
| 10 | Extend `hu_tom_detect_gaps()` to flag staleness gaps: a belief is stale if `belief.last_updated_ts < event.timestamp AND event.timestamp > now - staleness_window` (default 7d, configurable). Relevant-kind mapping per design D-TOM-6. | AC-TOM-6 | TBD | pending |
| 11 | Periodic cleanup tick (`hu_daemon_tick_tom_expectation_gc`): every 24h, DELETE FROM tom_user_expectations WHERE resolved_ts_ms IS NOT NULL AND resolved_ts_ms < now - 30d. Bounds table size. | Risk-TOM-4 | TBD | pending |
| 12 | Unit tests: (a) expectation extraction fires on representative inbound, (b) expectation row is recorded with correct fields, (c) prompt includes Unmet section when unresolved expectations exist, (d) session_key separates beliefs across batches, (e) staleness gap fires when persona delta pre-dates belief, (f) self-change events recorded for all three event kinds. | AC-TOM-1 through AC-TOM-6 | TBD | pending |

## Dependencies

- Tasks 1, 2, 3, 5 depend on each other in sequence (extract → record → table → surface).
- Task 4 (belief temporality) is independent of the expectation path; can be done in parallel.
- Tasks 6, 7, 9 are the self-change event chain — independent of expectation work but they share Task 6 (the table).
- Task 8 (adapter-swap hook) depends on **Spec 1 AC-M3-3**.
- Task 10 (gap extension) depends on Task 6 (events to query against).
- Task 11 (cleanup) is independent; can land anytime after Task 3.

## Sequencing recommendation

**Phase A (expectation activation):** 1, 2, 3, 5, 11 — the core dead-code-activation work.
**Phase B (temporality):** 4 — schema + write-site change.
**Phase C (self-change events):** 6, 7, 9 — non-blocking event recording.
**Phase D (gap extension):** 10 — depends on Phase C.
**Phase E (cross-spec):** 8 — gated on Spec 1 AC-M3-3.
**Phase F (verification):** 12.

Phases A, B, C can run in parallel. D, E, F sequential.

## Cross-spec dependencies

- **Spec 1 AC-M3-3** (swap-failure observability) must land before Task 8 fires meaningfully. Task 8's code can land before Spec 1 — it just no-ops until Spec 1's structured success/failure events are emitted.
- No other cross-spec dependencies.

## Verification

After all tasks complete:
```
Agent({
  description: "Verify TOM activation spec satisfaction",
  subagent_type: "spec-verifier",
  prompt: "Spec at specs/2026-05-19-tom-activation/. Verify AC-TOM-1 through AC-TOM-6. Pay special attention to AC-TOM-1 (dead-code activation): grep src/ for callers of hu_tom_detect_user_expectation — recon found ZERO before this spec, should find ≥1 after. Output RESULT_spec-verifier=PASS|FAIL."
})
```
