---
plan: docs/plans/2026-05-15-memory-scoping-followups.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Follow-up plan from the 2026-05-14/15 "Mindy texts are off" session. Documents what
shipped (channel-layer dedup, identity_links loader, session-scoped memory writes,
SQLite NULL-session filter, recency LRU module) and lists 7 follow-ups (FU-1..FU-7).

## Key Claims (from the plan)
- Claim 1: Channel-layer outbound dedup ring shipped + 7 tests
- Claim 2: `session.identity_links` JSON loader shipped + 9 tests
- Claim 3: Memory writes scope to `current_session_id` + 3 tests
- Claim 4: SQLite recall filter excludes NULL session_id (shipped, NO TEST yet — FU-6)
- Claim 5: `contact_send_recency` module shipped but NOT YET WIRED (FU-1)
- Claim 6: FU-1 (daemon-layer reactive-priority gate) — not done
- Claim 7: FU-3 (integration test) and FU-6 (recall-filter regression test) — not done

## Evidence

### Implemented? (code exists)
- `src/channels/imessage.c` — outbound dedup ring present ✓
- `tests/test_imessage_outbound_dedup.c` ✓
- `tests/test_config_identity_links.c` ✓
- `tests/test_memory_session_scoping.c` ✓
- `include/human/contact_send_recency.h`, `src/context/contact_send_recency.c` ✓
- `tests/test_contact_send_recency.c` (12 tests) ✓
- Commit log shows the related P3-x and P6-4 commits LANDED:
  - `5caf7ece feat(humanness): route silence-acknowledgments through persona context (2026-05-16 P6-4)`
  - `8b135cdc fix(agent): scope goals to contact_id (2026-05-16 P3-1)`
  - `b8057b78 fix(context): scope life_threads to contact_id (2026-05-16 P3-2)`
  - `eeae210c fix(agent): bind contact_id in scheduler dispatch SELECT (2026-05-16 P3-5)`
  - `a4d0371a fix(agent): strict contact_id only in world_model — drop name fallback (2026-05-16 P3-8)`
  - `58726224 fix(daemon,agent): scope replay-insights memory to contact_id (2026-05-16 RI-1/RI-2)`

### Proven? (tests exist)
- Tests for shipped items exist as listed above
- FU-3 (integration test using real SQLite + mock provider) — NOT FOUND
- FU-6 (recall-filter regression test asserting NULL-session exclusion) — NOT FOUND
  (existing test `experience_record_with_null_session_writes_empty` covers the write
  side, not the read filter)
- The P3-* and P6-4 commits would carry their own tests; not all sampled here

### Wired? (called in runtime path / dispatch)
- `contact_send_recency` module is built and tested, but `grep -rn "contact_send_recency"
  src/daemon.c src/daemon/daemon_proactive.c` returns 0 hits. FU-1 wiring is NOT done.
- Memory scoping commits (P3-1, P3-2, P3-5, P3-8, RI-1, RI-2) ARE wired in their respective
  call sites per the commit messages.

## Gaps
- FU-1 daemon-layer wiring of `contact_send_recency` — module exists, not wired
- FU-3 integration test using real SQLite — not present
- FU-4 service-level identity (RCS/SMS/iMessage) — no implementation, design decision still open
- FU-5 concatenated-handle anomaly — not addressed (chat.db data issue; safer fix in identity_links)
- FU-6 recall-filter regression test — missing
- FU-7 orphan-row cleanup script — optional, not run

## Notes
"Already shipped" inventory in the plan is accurate and matches the codebase. The
follow-ups are explicitly labeled "not done tonight" so their absence is the planned
state. The CLAUDE.md commit-log references to P3-1 (goals → contact_id) and P6-4
(silence-acks through persona context) are confirmed present in `git log`.
