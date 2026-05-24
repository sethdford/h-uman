# Theory-of-Mind Activation — Design

## Components

- **Pre-turn expectation detector hook** — new call to `hu_tom_detect_user_expectation()` (already defined at `src/agent/theory_of_mind.c:152-351`) wired into the pre-turn context-assembly path around `daemon.c:8880`. One new call site; no new logic.
- **Expectation recorder** — new function `hu_tom_record_user_expectation(contact_id, topic, expected_knowledge_type, conversation_id, turn_number)` that persists detected expectations. Lives in: extension of `src/agent/theory_of_mind.c`.
- **`tom_user_expectations` SQLite table** — new schema:
  ```sql
  CREATE TABLE tom_user_expectations (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    topic TEXT NOT NULL,
    expected_knowledge_type INTEGER NOT NULL,  -- HU_TOM_EXPECT_* enum
    conversation_id TEXT,
    turn_number INTEGER,
    created_ts_ms INTEGER NOT NULL,
    resolved_ts_ms INTEGER,                     -- NULL if not yet matched against a belief
    UNIQUE (contact_id, topic, conversation_id) ON CONFLICT IGNORE
  );
  CREATE INDEX idx_tom_user_expectations_contact ON tom_user_expectations(contact_id, resolved_ts_ms);
  ```
- **Conversation-local belief temporality** — extension of `hu_tom_belief_t` with two new fields: `session_key` (TEXT, nullable; reuses the existing per-batch session_key from `daemon.c:4550`'s `msgs[m].session_key`) and `turn_number` (INTEGER, nullable). Existing belief rows have NULL session_key meaning "global / not conversation-scoped." Resolution of Q-TOM-A: project has no top-level `conversation_id` concept; `session_key` (batch-key, channel × contact × time window) is the closest existing stable identifier. We piggyback on it rather than synthesizing a new id. Lives in: schema migration + extension of the existing `belief_record_extract_topic` path.
- **Self-change event recorder** — new function `hu_tom_record_self_change_event(contact_id, event_kind, ...)` invoked from three sites:
  - persona delta application (existing `hu_persona_delta_apply()` or equivalent)
  - adapter swap completion (existing `hu_mlx_admin_swap_adapter()` success branch — depends on Spec 1's observability)
  - emotional-register transition (existing register-change detection point)
- **`tom_self_change_events` SQLite table** — new schema:
  ```sql
  CREATE TABLE tom_self_change_events (
    id INTEGER PRIMARY KEY,
    contact_id TEXT NOT NULL,
    event_kind INTEGER NOT NULL,         -- HU_TOM_SELF_CHANGE_* enum
    conversation_id TEXT,
    turn_number INTEGER,
    timestamp_utc_ms INTEGER NOT NULL,
    magnitude REAL                       -- e.g., σ for register shift, 1.0 for binary events
  );
  CREATE INDEX idx_tom_self_change_contact_ts ON tom_self_change_events(contact_id, timestamp_utc_ms);
  ```
- **Extended gap detection** — modification of existing `hu_tom_detect_gaps()` at `src/agent/theory_of_mind.c` to also flag beliefs that pre-date a relevant self-change event for the same contact (staleness gaps).
- **Prompt directive surfacing** — extension of the existing TOM context-assembly path (around `daemon.c:8910-8989`) to include an "Unmet-User-Expectations" section listing currently-active expectations without matching beliefs.

## Data flow

```
[Inbound user message arrives]
   │
   ▼
[Pre-turn context-assembly (around daemon.c:8880)]
   │
   ▼
[NEW: hu_tom_detect_user_expectation(message)]
   │
   ▼
[For each detected expectation:
    hu_tom_record_user_expectation(
      contact_id, topic, type,
      conversation_id, turn_number)]
   │
   ▼
[Belief gap query (existing, extended):
    hu_tom_detect_gaps(contact_id)
    returns:
      - expectations with no matching belief
      - beliefs pre-dating relevant self-change events]
   │
   ▼
[Existing TOM context block (daemon.c:8910)
    extended with "Unmet-User-Expectations" section]
   │
   ▼
[Prompt assembled with directive,
    LLM generation proceeds]
   │
   ▼
[Post-turn belief recording (existing):
    hu_tom_record_belief from extracted topics]
   │
   ▼
[Belief rows now include conversation_id + turn_number]
   │
   ▼
[Resolve loop: any tom_user_expectations.topic that
    now has a matching belief gets resolved_ts_ms set]

[Concurrently, when self-changes happen:]
   │
   ▼
[persona delta applied   → hu_tom_record_self_change_event(PERSONA_DELTA, ...)]
[adapter swap success    → hu_tom_record_self_change_event(ADAPTER_SWAP, ...)]
[emotional register shift→ hu_tom_record_self_change_event(REGISTER_SHIFT, ...)]
```

## Decisions

- **D-TOM-1 (AC-TOM-1): Detection runs every inbound message, not just first message of conversation.** Chose every-message because expectations can arise mid-conversation ("oh, as you know from earlier, I prefer X"). Tradeoff: ~14-pattern regex pass per inbound message — cheap, well within hot-path budget.
- **D-TOM-2 (AC-TOM-2): Separate `tom_user_expectations` table rather than extending `hu_tom_belief_t`.** Chose separate table because: (a) expectations have a `resolved_ts_ms` lifecycle that beliefs don't, (b) discriminator-field-on-belief makes existing belief queries pay for expectation filtering, (c) the existing `hu_tom_belief_state_t` is fixed-size 64 — expectations need unbounded growth.
- **D-TOM-3 (AC-TOM-3): Prompt directive section is additive, never replacing existing TOM block.** Chose additive over replacement so existing TOM tests continue to pass. The new section appears below the existing "Contact Mental Model" block with header "### Unmet User Expectations". When the list is empty, the section is omitted (no empty header in prompt).
- **D-TOM-4 (AC-TOM-4): Conversation-local temporality via nullable `session_key` column on belief rows.** Chose nullable extension over new table because: (a) the existing belief record is small and adding two columns is cheap, (b) querying "beliefs for this contact in this session" is one extra WHERE clause, (c) a separate "conversation_beliefs" table would force JOINs and double-write logic. **Resolution of Q-TOM-A:** the codebase has no `conversation_id` primitive. The closest stable identifier is `session_key` (a batch key derived from channel × contact × time-window, defined at `daemon.c:4550`). This spec piggybacks on `session_key` as the conversation handle. Default NULL means "global / pre-temporality" for backward compatibility.
- **D-TOM-5 (AC-TOM-5): Self-change events stored in dedicated table.** Chose dedicated over inlining-into-beliefs because self-change events have different schema (event_kind enum, magnitude) and different query patterns (range scans by timestamp for staleness checks).
- **D-TOM-6 (AC-TOM-6): Staleness gap detection uses timestamp comparison.** A belief is "stale" relative to a self-change event of relevant kind for the same contact if `belief.last_updated_ts < event.timestamp_utc_ms AND event.timestamp_utc_ms > now - staleness_window` (default window 7 days). The "relevant kind" mapping is:
  - PERSONA_DELTA → invalidates beliefs of expected_knowledge_type REMEMBERS
  - ADAPTER_SWAP → invalidates beliefs of expected_knowledge_type UNDERSTANDS
  - REGISTER_SHIFT → invalidates beliefs of expected_knowledge_type TRACKS
- **D-TOM-7: Activation does not redesign `hu_tom_belief_t`.** This spec adds two nullable columns and three new tables. The existing in-memory 64-slot belief state remains the hot-path cache; persistence is layered, not replacing.
- **D-TOM-8 (Constraint: TOM gating).** Per recon, TOM appears unconditional in the codebase (no `HU_ENABLE_TOM` flag visible). New code remains unconditional. If a flag is later added, this spec's code respects it.

## Risks

- **Risk-TOM-1 (D-TOM-1): Pattern table at `theory_of_mind.c:294-307` may have false positives for short messages.** Phrases like "as you know" can be conversational filler, not actual expectation signals. **Mitigation:** the existing pattern table is a sunk-cost decision; tuning patterns is an explicit non-goal of this spec. Track false-positive rate via expectation_resolved_ts_ms — if many expectations are recorded but never matched, that's a signal for pattern tuning in a follow-up.
- **Risk-TOM-2 (D-TOM-5, depends on Spec 1): Adapter-swap self-change recording requires Spec 1's swap-success observability.** Without Spec 1's structured swap-failure observability (AC-M3-3), we can't reliably detect "swap completed successfully." **Mitigation:** sequence — Spec 1's AC-M3-3 lands before Spec 4's AC-TOM-5. Until then, the adapter-swap event recorder is wired but never fires.
- **Risk-TOM-3 (D-TOM-6): Default staleness window of 7 days is arbitrary.** Real persona drift / adapter rotation may invalidate beliefs faster or slower. **Mitigation:** configurable via `tom.staleness_window_sec`; default 7 days is a starting heuristic. Eval-driven tuning is a follow-up.
- **Risk-TOM-4 (D-TOM-2): `tom_user_expectations` table can grow unbounded.** **Mitigation:** add a periodic cleanup tick that deletes resolved expectations older than 30 days; existing daemon-tick infrastructure pattern applies. Spec this in `tasks.md` as a small task.
- **Risk-TOM-5 (D-TOM-3): "Unmet User Expectations" prompt section edges toward "acting on" semantics.** This is acknowledged in the requirements scope tension. **Mitigation:** the directive is descriptive ("user expects you to remember X but you don't have a recorded belief about X"), not prescriptive ("ask the user about X"). The downstream prompt-generation decides whether to clarify or proceed.
- **Risk-TOM-6 (AC-TOM-4): Adding conversation_id to belief records may require backfill of existing belief rows.** **Mitigation:** the new columns default to NULL; existing rows are unchanged. Migration is a single `ALTER TABLE ADD COLUMN`. No backfill needed; old rows simply have no conversation temporality, which matches their existing semantic.

## Open design questions

- **Q-TOM-A: conversation_id source — RESOLVED.** No `conversation_id` primitive in codebase; closest is `memory_session_id = contact_id` (too coarse — never changes within a contact's lifetime) and `session_key` (the per-batch key from `daemon.c:4550`, finer-grained: channel × contact × time-window). Spec uses `session_key`. See D-TOM-4 update.
- Q-TOM-B: Resolution semantics — when is an expectation "resolved"? Options: (a) when a belief on the same topic with matching contact_id appears, (b) when the agent's next turn includes content matching the expectation, (c) explicit user signal ("oh you do remember"). (a) is simplest; (b) requires response analysis; (c) requires a new signal. Start with (a).
- Q-TOM-C: Pattern table extension for new expectation phrasings — explicitly out of scope. But the extension seam should be obvious enough that follow-up work doesn't have to refactor.
