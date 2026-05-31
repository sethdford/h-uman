---
title: Memory Scoping & Continuity — Follow-ups
status: closed
created: 2026-05-15
last_audit: 2026-05-25
---

# Memory Scoping & Continuity — Follow-ups

> Spawned from the 2026-05-14/15 "Mindy texts are off" debugging session.
> Read [the adversarial review](#adversarial-review-recap) below for context.

## Status of work already shipped (DO NOT redo)

| Change | File(s) | Test | Status |
|---|---|---|---|
| Channel-layer outbound dedup ring (120s window) | `src/channels/imessage.c` | `tests/test_imessage_outbound_dedup.c` (7 tests) | ✅ shipped |
| A1: long-message false-positive fix (`sent_ring_full_len[]`) | `src/channels/imessage.c` | `tests/test_imessage_outbound_dedup.c::long_messages_sharing_prefix_*` + `::long_message_retry_*` | ✅ shipped |
| `session.identity_links` JSON loader | `src/config/config_parse.c` | `tests/test_config_identity_links.c` (9 tests) | ✅ shipped |
| Memory writes scope to `memory->current_session_id` | `src/intelligence/experience.c`, `src/context/context_engine_rag.c` | `tests/test_memory_session_scoping.c` (3 tests) | ✅ shipped |
| SQLite recall filter excludes NULL session_id on scoped queries | `src/memory/engines/sqlite_lucid.c` (FTS + LIKE paths) | _no test yet_ (see follow-up FU-3) | ✅ shipped |
| Intentional-global writers documented | `src/agent/preferences.c`, `src/pwa/learner.c`, `src/memory/lifecycle/summarizer.c` | _comments only_ | ✅ shipped |
| `contact_send_recency` LRU module (scaffolding) | `include/human/contact_send_recency.h`, `src/context/contact_send_recency.c` | `tests/test_contact_send_recency.c` (12 tests) | ✅ shipped (not yet wired into daemon — see FU-1) |
| Backup of pre-cleanup `memory.db` | `~/.human/memory.db.pre-orphan-cleanup-20260515-084804` | n/a | ✅ kept |

## Adversarial review recap

The 2026-05-15 adversarial pass identified 7 attack vectors (A–G). Outcomes:

- **A1 (long-message false positive)** — REAL, fixed.
- **A2 (ring overflow under burst)** — REAL but LOW severity. See FU-2.
- **A3 (clock skew)** — no finding.
- **B (missed unscoped writers)** — I claimed 5+ broken; on re-audit, all 5 are either already-correct (sites at agent_stream.c/agent_turn.c read `current_session_id` upstream) or intentionally-global (preferences, pwa, summarizer). Comments added.
- **C (service-level sharding RCS/SMS/iMessage)** — REAL. See FU-4.
- **D (integration-test gap)** — REAL. See FU-3.
- **E (live daemon claim)** — no finding, evidence held up.
- **F (orphan rows leaking into recall)** — REAL, fixed at the recall-filter level. Orphans left in db; harmless going forward.
- **G (echo prevention vs new ring fields)** — no finding.

## Follow-ups (not done tonight)

### FU-1 · Daemon-layer reactive-priority gate · MEDIUM

**What:** Wire the `contact_send_recency` module (already on disk) into `src/daemon.c` so proactive paths (F25 emotional check-in at line 1017, scheduler at 1090, proactive check-in at 1726, photo at 1922) defer when the reactive turn has fired for the same contact within `HU_DAEMON_REACTIVE_GATE_WINDOW_S = 60`.

**Why:** The channel-layer ring already catches retries and identical-text dupes. The remaining duplication class is "different but related text from two daemon paths firing for the same contact within seconds" (e.g., reactive reply + proactive morning greeting). Today nothing stops that.

**Scope:** 4 instrumented sites in daemon.c (1 record, 3 check). Estimated ~50 LOC + tests.

**Trap:** I originally underestimated this as "4 sites of 23" — the reactive turn is buried in the batch-reply loop (line 9368) which has 8+ send sites. Classify carefully before instrumenting.

### FU-2 · Dedup ring sizing under burst · LOW

**What:** Ring size is 32 with FIFO modulo eviction. Under a 33+ message burst within 120s, the oldest slot is overwritten; a re-send of the first message in the burst would then miss dedup.

**Repair options:**
1. Bump to 128 (cheap, mostly papers over).
2. Switch to time-based LRU eviction (overwrite oldest by `ts`, not by `idx`).
3. Per-target rings keyed by `target_hash` (more accurate, more memory).

**Trigger:** only matters if you observe a real burst-then-retry case in production logs. Defer until evidence.

### FU-3 · Integration test for memory scoping · HIGH

**What:** All current memory-scoping tests use mocks. They prove writers READ `current_session_id` but don't prove the daemon SETS it correctly before every code path.

**Test design:**
1. Stand up a real SQLite memory backend in a temp dir.
2. Drive the daemon through a mock-provider turn against a fake contact `+15551234567`.
3. Assert that the resulting `memories` rows have `session_id = '+15551234567'` (or its routed form).
4. Repeat for proactive paths (F25, scheduler, etc.) — these set `memory_session_id` independently and could regress.

**Why:** This is the test that would have caught the original bug. The mock-backed regression tests are useful but narrow.

### FU-4 · Service-level identity (RCS / SMS / iMessage) · MEDIUM

**What:** Mindy's chat.db has handles in three services: RCS, SMS, iMessage. The session_key format `agent:<agent>:<channel>:direct:<peer>` includes the channel name. If h-uman maps these to different channel strings, the same person's memory shards by service.

**Design options:**
1. **Service-collapse mode**: route RCS/SMS fallbacks of an iMessage thread to the iMessage session_key. Pros: simple. Cons: loses ability to distinguish "she texted via SMS so service was down" from "she sent via iMessage."
2. **Cross-service identity links**: extend `identity_links` JSON schema with optional `services[]` array so one canonical can span services. Pros: per-contact opt-in. Cons: more config burden.
3. **Channel-agnostic memory namespace**: keep service-specific sessions for routing but recall across them. Pros: best of both. Cons: recall path needs work.

**Action:** Pick approach with Seth before implementing. Likely option 2 since it composes with the already-shipped identity_links loader.

### FU-5 · Concatenated handle anomaly in chat.db · LOW

**What:** `~/Library/Messages/chat.db` contains a handle row `+18018285260+18018285260|iMessage` — a literal concatenation that's a separate `handle.id`. Any message routed through it gets a different session_id from the canonical.

**Repair:** Either:
- Delete the bogus row directly (one-shot SQL on chat.db — destructive, needs backup).
- Add it as a `peers[]` entry under the canonical `+18018285260` once Seth adds an `identity_links` entry for Mindy.

**Risk:** Editing chat.db could destabilize macOS Messages. The safer fix is the second option.

### FU-6 · Recall-filter regression test · MEDIUM

**What:** Add a test that proves the SQLite recall filter excludes NULL-session entries on scoped queries.

**Test design:**
1. Open a real SQLite memory backend.
2. Store two entries:
   - `(key=A, session_id=NULL, content="experience: unrelated thing")`
   - `(key=B, session_id="contact_X", content="experience: relevant thing")`
3. Call `recall(query="experience", session_id="contact_X")`.
4. Assert: result contains B, does NOT contain A.

**Why:** The shipped fix is invisible to existing tests (the in-memory engine already had correct behavior; only the SQLite prod path was broken).

### FU-7 · Orphan row cleanup script · LOW

**What:** 52 `experience:*` rows in `~/.human/memory.db` with NULL session_id. Now harmless to scoped recall thanks to the FU-3 filter fix, but still occupy disk and would appear in unscoped queries.

**Repair:** Optional one-shot:
```sql
-- After backup verified
DELETE FROM memories WHERE session_id IS NULL AND key LIKE 'experience:%';
```

**Trigger:** Only if Seth wants to reclaim disk or wants the unscoped recall path to be cleaner. Backup at `~/.human/memory.db.pre-orphan-cleanup-20260515-084804`.

## How to verify the shipped fixes are working in practice

After ~24h of Mindy texting:

```bash
# Mindy's session-id row count should grow past 1
sqlite3 ~/.human/memory.db \
  "SELECT COUNT(*) FROM memories WHERE session_id LIKE '%18018285260%';"

# Total scoped vs unscoped — scoped count should grow; NULL count should stay frozen
sqlite3 ~/.human/memory.db \
  "SELECT COALESCE(session_id,'(null)') AS sid, COUNT(*) FROM memories GROUP BY sid ORDER BY 2 DESC LIMIT 10;"

# Dedup-drop telemetry — every drop logs at INFO
grep "outbound dropped: duplicate text" ~/.human/logs/*.log | wc -l
```

If Mindy's row count stays at 1 after a day of real conversations, FU-3 integration test is urgent — there's a write path we haven't covered.

## Definition of "Mindy actually feels continuous"

A future session should declare this work done when:

1. Mindy's `memories` row count grows monotonically per real conversation (FU-3 integration test passes).
2. No iMessage texts to her get duplicated within 120s (current channel-layer fix; FU-1 closes the cross-path case).
3. Per-contact recall for her returns ONLY her scoped memories (current sqlite_lucid fix; FU-6 test confirms).
4. RCS/SMS fallbacks during connectivity issues don't shard her memory (FU-4).
5. The Mac/iPad/phone handles all resolve to one session_key (depends on FU-4 design choice).

Today: items 1–3 are infrastructure-ready; items 4–5 are open work.
