# Reflection Loop — Design

**Status:** Draft (post-brainstorm 2026-05-26)
**Owner:** Seth
**Mission:** Closes M2 ("Personal Model" — unified model-of-the-person from memory) in CLAUDE.md.
**Sibling specs:** [`2026-05-25-initiative-layer/`](../2026-05-25-initiative-layer/) (init_proposer, in flight — consumes reflection output).

## Why

`hu_personal_model_t` accumulates typed facts with confidence and half-life decay (`hu_fact_extract` + `src/memory/personal_model.c`). Facts are raw observations. They tell the agent *what was said*; they don't tell it *what to make of it*.

Without a reflection layer, the agent treats every turn as if encountering Seth for the first time, modulo whatever the personal-model summary surfaces. A human friend doesn't work that way — they think about you when you're not in the room and arrive with updated mental models. Reflection is the equivalent: a periodic pass that turns observations into patterns the agent can act on.

This is the highest-leverage architectural addition between now and the 100-DAU mission (M4) because every other "better than human" capability (cross-channel synthesis, proactive surfacing, calibrated uncertainty) depends on patterns existing.

## Goals

1. Periodically distill accumulated conversations into typed, queryable patterns.
2. Make patterns queryable by downstream consumers (system prompt builder, init_proposer, future cross-channel synthesis).
3. Ship privacy-by-architecture: start with cloud reflection (Gemini 3.1 Pro) but architect for local (Gemma 4) via eval-driven ratchet — same shape as the validated M3 win.
4. Fail safely: no single bad reflection can corrupt the personal model or surface a wrong observation repeatedly.

## Non-goals (Phase 1)

- **Belief updates** that mutate `hu_personal_model_t` directly — deferred to Phase 2 with quorum gate.
- **Cross-channel synthesis layer** that decides whether to bring up channel A's content in channel B — that's the M2-cross-channel sub-project (separate spec).
- **Real-time / event-driven reflection** — reflection is a periodic batch task, not a per-turn signal.
- **Calibrated uncertainty output formatting** — separate sub-project (#5).

## Architecture overview

```
┌──────────────────────────────────────────────────────────────────┐
│  Daemon main loop (existing)                                     │
│                                                                  │
│   ... tick_channels ... tick_feeds ... tick_reaction_collection  │
│                                ↓                                 │
│                    hu_reflection_tick()                          │
│                                ↓                                 │
│                  idle + interval gates pass?                     │
│                          yes ↓                                   │
│                   hu_reflection_run(daemon)                      │
│                                ↓                                 │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  prompt.c  — assemble last-N turns since prev run       │    │
│  │      ↓                                                   │    │
│  │  provider->chat()  — Gemini 3.1 Pro (cloud default)      │    │
│  │      ↓                                                   │    │
│  │  schema.c — parse + validate JSON output                 │    │
│  │      ↓                                                   │    │
│  │  storage.c — INSERT runs row + UPSERT pattern rows       │    │
│  │              + JSON dump to ~/.human/reflections/        │    │
│  │      ↓                                                   │    │
│  │  consumer.c — invalidate cached system-prompt slices     │    │
│  └─────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────┘

Downstream consumers (separate, query-driven):
    system_prompt_builder → hu_reflection_query_for_system_prompt(channel)
    init_proposer         → hu_reflection_query_unsurfaced()
    cross_channel (future)→ hu_reflection_query_cross_channel()
```

Approach: in-daemon tick (Approach A from brainstorm). Reflection runs inline on the daemon thread; channel polls back up during the 30-60s reflection window and catch up after. Acceptable because reflection fires once per ≥12h and the daemon has no per-second SLA.

## Components

### `include/human/reflection.h`

```c
typedef enum {
    HU_REFLECTION_PATTERN_TOPIC_RECURRENCE = 0,
    HU_REFLECTION_PATTERN_BEHAVIORAL_SHIFT,
    HU_REFLECTION_PATTERN_PREFERENCE,
    HU_REFLECTION_PATTERN_EMOTIONAL_STATE,
    HU_REFLECTION_PATTERN_SCHEDULE_PATTERN,
    HU_REFLECTION_PATTERN_RELATIONSHIP,
    HU_REFLECTION_PATTERN_COUNT
} hu_reflection_pattern_type_t;

typedef struct hu_reflection_pattern {
    char        id[64];                  // stable hash of (type, subject, observation prefix)
    hu_reflection_pattern_type_t type;
    char        subject[128];
    char        observation[512];
    double      confidence;              // [0,1] self-rated by reflection model
    char        evidence_ids[8][64];     // up to 8 turn references
    int         evidence_count;
    char        channels[8][32];     // cap of 8 channels per pattern (sufficient for h-uman's 4 Tier-1 + occasional channel)
    int         channel_count;       // if model emits >8, storage layer truncates and logs warning
    uint64_t    created_at_ms;
    uint64_t    last_observed_at_ms;     // bumped on UPSERT when re-derived
    uint64_t    expires_at_ms;           // half-life: 30 days default
    bool        surfaced_to_user;
    bool        retired;                 // set when user contradicts
    uint64_t    retired_at_ms;
} hu_reflection_pattern_t;

// Tick — called from daemon main loop. Cheap; gates internally.
hu_error_t hu_reflection_tick(struct hu_daemon *d);

// Force a reflection run (for tests / manual trigger). Bypasses idle gate
// but still respects min_interval_hours unless force=true.
hu_error_t hu_reflection_run(struct hu_daemon *d, bool force);

// Query consumer helpers.
hu_error_t hu_reflection_query_for_system_prompt(
    sqlite3 *db, const char *channel, int max_patterns,
    hu_reflection_pattern_t **out_patterns, int *out_count);

hu_error_t hu_reflection_query_unsurfaced(
    sqlite3 *db, double min_confidence,
    hu_reflection_pattern_t **out_patterns, int *out_count);

void hu_reflection_mark_surfaced(sqlite3 *db, const char *pattern_id);
void hu_reflection_retire(sqlite3 *db, const char *pattern_id);

// Quorum predicate for Phase 2 (belief updates). Returns true iff pattern has
// been observed in ≥3 distinct runs with confidence > 0.7 each. Phase 1 callers
// can use this for logging/telemetry but MUST NOT mutate personal_model on it.
bool hu_reflection_pattern_has_quorum(sqlite3 *db, const char *pattern_id);
```

### `src/reflection/reflection.c`

- `hu_reflection_tick()`: cheap gate (< 1ms). Checks:
  1. `cfg->reflection.enabled` — if false, emit one-shot disabled-log (per `silent-config-gated-subsystems.md`) and return HU_OK
  2. Time since last `reflection_runs.completed_at_ms` ≥ `min_interval_hours` (default 12)
  3. Either: (a) idle ≥ `idle_threshold_hours` (default 2) — no inbound/outbound activity OR (b) time since last run ≥ `daily_floor_hours` (default 24, forced reflect)
  4. If gates pass: call `hu_reflection_run(daemon, false)`
- Idle detection: query daemon's existing message ledger for max(ts) across inbound + outbound; compare to `now - idle_threshold_hours`
- All three thresholds JSON-configurable in `config.reflection.{min_interval_hours, idle_threshold_hours, daily_floor_hours}`

### `src/reflection/prompt.c`

- `hu_reflection_build_input(daemon, since_ms, &out_buf, &out_turn_count)`
- Reads conversations from all channels since `since_ms` (= `MAX(last completed reflection, now - 7 days)`)
- Formats as compact transcript: `[channel] [ts] sender: text\n...`
- Caps at 25K input tokens (≈ 100K chars) — truncate oldest turns first if over
- Reflection system prompt (held in `reflection_system_prompt.txt`):
  - Defines the 6 pattern types with examples
  - Demands strict JSON output matching schema
  - Asks model to self-rate confidence per pattern
  - Asks for a 2-3 sentence prose summary alongside structured patterns

### `src/reflection/schema.c`

- `hu_reflection_parse(const char *json, hu_reflection_pattern_t **out_patterns, int *out_count, char **out_prose_summary, char **out_error)`
- Strict validation: required fields present, type enum valid, confidence in [0,1], evidence/channel arrays bounded
- Stable id computation: `SHA-256(type_str + subject + observation[:128])[:16]` hex-encoded
- Confidence floor (Layer 2 from failure handling): patterns with `confidence < 0.5` are returned with a flag but not inserted by storage layer

### `src/reflection/storage.c`

- SQLite migrations (gated on `HU_ENABLE_SQLITE`):

```sql
CREATE TABLE IF NOT EXISTS reflection_runs (
    run_id          TEXT PRIMARY KEY,
    provider        TEXT NOT NULL,
    started_at_ms   INTEGER NOT NULL,
    completed_at_ms INTEGER,
    input_turns     INTEGER NOT NULL,
    input_tokens    INTEGER,
    output_tokens   INTEGER,
    status          TEXT NOT NULL,         -- 'ok'|'schema_invalid'|'provider_error'|'in_progress'
    error_message   TEXT,
    json_dump_path  TEXT,
    prose_summary   TEXT
);

CREATE TABLE IF NOT EXISTS reflection_patterns (
    id              TEXT PRIMARY KEY,
    type            TEXT NOT NULL,
    subject         TEXT NOT NULL,
    observation     TEXT NOT NULL,
    confidence      REAL NOT NULL,
    evidence_json   TEXT NOT NULL,
    channels_json   TEXT NOT NULL,
    first_seen_run_id  TEXT NOT NULL REFERENCES reflection_runs(run_id),
    last_seen_run_id   TEXT NOT NULL REFERENCES reflection_runs(run_id),
    observation_count  INTEGER NOT NULL DEFAULT 1,
    created_at_ms   INTEGER NOT NULL,
    last_observed_at_ms INTEGER NOT NULL,
    expires_at_ms   INTEGER NOT NULL,
    surfaced_to_user INTEGER NOT NULL DEFAULT 0,
    retired         INTEGER NOT NULL DEFAULT 0,
    retired_at_ms   INTEGER
);

CREATE INDEX IF NOT EXISTS idx_patterns_recent ON reflection_patterns(last_observed_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_patterns_unsurfaced ON reflection_patterns(surfaced_to_user, retired, confidence DESC);
CREATE INDEX IF NOT EXISTS idx_patterns_channel ON reflection_patterns(channels_json);
```

- UPSERT semantics on `reflection_patterns.id`: same pattern observed → bump `observation_count`, update `last_observed_at_ms`, update `confidence` (take max of stored vs new), extend `expires_at_ms`
- JSON dump per run to `~/.human/reflections/<provider>/<run_id>.json` — captures raw model output for shadow comparison
- Variant build (no SQLite): module compiles but `hu_reflection_tick()` returns HU_OK after one-shot disabled-log

### `src/reflection/consumer.c`

- `hu_reflection_query_for_system_prompt(db, channel, max_patterns=5, ...)`:
  - WHERE retired = 0 AND surfaced_to_user = 0 AND last_observed_at_ms > now - 7d AND confidence > 0.7
  - AND (EXISTS(SELECT 1 FROM json_each(channels_json) WHERE value = ?) OR json_array_length(channels_json) > 1)
  - (uses SQLite's JSON1 extension — standard in modern SQLite builds; substring `LIKE` matching is rejected because it false-positives on e.g. "imessage" matching "imessage_group")
  - ORDER BY confidence * recency_weight DESC LIMIT max_patterns
- `hu_reflection_query_unsurfaced(db, min_confidence=0.6, ...)`:
  - WHERE retired = 0 AND surfaced_to_user = 0 AND last_observed_at_ms > now - 30d AND confidence > min_confidence
- Integration with `hu_personal_model_build_prompt`: appends a "Recent observations:" section with top-5 patterns formatted as bullet points + the latest run's prose summary
- Integration with init_proposer: adds `hu_reflection_query_unsurfaced` as a candidate source; init_proposer's existing throttle/judge logic decides whether to actually surface

## Reflection pipeline (data flow)

1. Daemon tick fires `hu_reflection_tick`. Gates pass.
2. `hu_reflection_run`:
   a. Insert `reflection_runs` row with `status='in_progress'`
   b. `prompt.c` assembles input transcript (last reflection's completed_at_ms → now, capped)
   c. Call `daemon->router->reflection_provider` (configurable; default Gemini 3.1 Pro)
   d. If `shadow_mode = true`: also call local Gemma sequentially within the same tick (total wall time ~3-4 min). Both outputs JSON-dumped under `~/.human/reflections/{cloud,local}/<run_id>.json`. Cloud output is what proceeds through the pipeline. Local output is dump-only; not parsed into reflection_patterns rows during shadow mode.
   e. `schema.c` parses + validates output. On invalid: update run row to `status='schema_invalid'`, return HU_OK (don't fail daemon)
   f. `storage.c` UPSERTs patterns. Confidence < 0.5 patterns dropped here.
   g. Update run row: `status='ok'`, `completed_at_ms`, `output_tokens`, `prose_summary`
   h. Future: emit event to consumer cache-invalidation listeners

## Failure handling (the four layers)

1. **Schema validation** — Layer 1. Malformed JSON → run marked `schema_invalid`, single retry with repair prompt next tick. Two consecutive schema failures → operator log warning.
2. **Confidence floor** — Layer 2. Patterns with confidence < 0.5 are logged (`reflection_runs.low_confidence_dropped_count`) but not inserted.
3. **Quorum gate for Phase 2** — Layer 3 (deferred). Belief updates to `hu_personal_model_t` require `hu_reflection_pattern_has_quorum() == true` (≥3 distinct runs, each with confidence > 0.7). Phase 1 callers MUST NOT mutate personal_model on reflection output; the predicate exists for telemetry only until Phase 2.
4. **Retire-on-contradiction** — Layer 4. When init_proposer surfaces a pattern and the user responds negatively (reaction = thumbs-down, response_guard detects contradiction phrase, or DPO pair labels surfacing as bad), `hu_reflection_retire(id)` sets `retired=1`. Retired patterns can be re-derived from raw conversations later but must re-clear quorum.

**Operator health:**
- One-shot info log on first disabled tick (`silent-config-gated-subsystems.md`)
- One-shot info log on first successful tick (positive confirmation)
- Daily failure rate > 50% logged as warning
- `reflection_runs.status` is queryable for ops dashboards

## Eval & shadow-mode strategy

Per the dual-path ratchet decided in brainstorm:

**Phase 1 (sprint 1):** Cloud-only (`reflection.provider = "gemini-3.1-pro-preview"`). Ships the loop.

**Phase 2 (sprint 2+):** Shadow mode (`reflection.local_shadow_mode = true`). Every reflection runs both cloud and local Gemma 4 with v4-repair adapter; cloud output ships, local output JSON-dumped for offline comparison.

**Eval harness:** `scripts/eval_reflection_shadow.py`:
- Reads all `reflection_runs` from last N days with both cloud and local JSON dumps
- Computes per-run metrics:
  - **Pattern set Jaccard:** intersection / union of pattern ids
  - **Critical-miss rate:** patterns cloud emitted with confidence > 0.8 that local missed entirely
  - **Confidence calibration error:** mean |cloud_conf - local_conf| for shared patterns
  - **Schema validity rate:** % of local runs that pass schema validation
- Aggregates to a verdict similar to `docs/plans/2026-05-26-sprint-56-gemma-as-seth/results/us15-empirical-verdict.json`

**Ratchet criterion:** when local Gemma achieves (Jaccard ≥ 0.85, critical-miss-rate < 5%, schema validity ≥ 95%) for 14 consecutive days, flip `reflection.provider = "gemma-4-31b-local"` and set `reflection.cloud_fallback = true`.

**Phase 3 (if needed):** If Phase 2 reveals local quality gap > 15% Jaccard, train a reflection-specific LoRA on cloud outputs. Recipe matches v4-repair (rank=8, scale=2.0, ~500 iters); training data is the cloud JSON dumps. Per `lora-scale-default-or-die.md`, scale stays at 2.0 — DO NOT override without validation.

## Testing strategy

**Unit (`tests/test_reflection_schema.c`):**
- Schema rejects malformed JSON, missing required fields, out-of-range confidence
- Stable id is deterministic across runs given same (type, subject, observation)
- Stable id differs when subject differs
- Confidence floor: patterns < 0.5 returned with drop flag

**Unit (`tests/test_reflection_storage.c`):**
- UPSERT semantics: re-observing same pattern bumps observation_count and last_observed_at_ms; doesn't create duplicate row
- Confidence on UPSERT takes max of stored vs new
- expires_at_ms extends on re-observation (30d from last_observed_at_ms)
- `surfaced_to_user` flag persists across UPSERTs

**Unit (`tests/test_reflection_consumer.c`):**
- `query_for_system_prompt` filters: retired excluded, surfaced excluded, > 7d old excluded, confidence < 0.7 excluded
- Channel filter: pattern with `channels = ["imessage"]` does NOT appear in `query_for_system_prompt(db, "telegram")`
- Cross-channel pattern (`channel_count > 1`) appears for ALL channels
- LIMIT respected (max 5)
- Ordering: confidence × recency_weight DESC

**Unit (`tests/test_reflection_quorum.c`):**
- `has_quorum` returns false for single observation
- `has_quorum` returns false for 3 observations if any has confidence ≤ 0.7
- `has_quorum` returns true for 3 distinct run_ids with confidence > 0.7
- Phase 1 contract: NO call site mutates `hu_personal_model_t` based on quorum (grep test in CI)

**Integration (`tests/test_reflection_e2e.c`):**
- Mock provider returns canned valid output → full pipeline inserts patterns, JSON dump written
- Mock provider returns malformed JSON → run marked schema_invalid, no patterns inserted, daemon unaffected
- Mock provider returns valid output but all patterns have confidence < 0.5 → run completes, zero patterns inserted, drop count recorded
- Tick gates: enabled=false → returns OK, no run; min_interval not elapsed → returns OK, no run; idle threshold met → runs

**Retire-on-contradiction (`tests/test_reflection_retire.c`):**
- `hu_reflection_retire(id)` sets retired=1, retired_at_ms
- Retired pattern excluded from all query helpers
- Next reflection re-deriving the same pattern: re-creates with new id semantics (retired row stays; new row has fresh stable id only if observation text changed — otherwise it's still suppressed)

**Gate symmetry (`scripts/check-test-source-gate-symmetry.sh`):**
- All `src/reflection/*.c` gated on `HU_ENABLE_SQLITE`
- All `tests/test_reflection*.c` either gated identically in CMake OR use internal-`#ifdef`-with-stub-runner pattern

## Sprint sequencing

**Sprint 1 (week 1-2) — ship Phase 1:**
- US-1: `include/human/reflection.h` + `src/reflection/schema.c` + storage migrations
- US-2: `src/reflection/storage.c` + tests
- US-3: `src/reflection/prompt.c` + reflection_system_prompt.txt
- US-4: `src/reflection/reflection.c` (tick + run) with cloud-only provider
- US-5: `src/reflection/consumer.c` + integration with `hu_personal_model_build_prompt`
- US-6: init_proposer consumer integration (calls `query_unsurfaced`)
- US-7: e2e tests with mock provider
- US-8: Config plumbing (`cfg.reflection.{enabled, min_interval_hours, idle_threshold_hours, daily_floor_hours, provider}`)
- US-9: One-shot disabled-log + operator health logging

**Sprint 2 (week 3-4) — shadow mode + eval:**
- US-10: `reflection.local_shadow_mode` config flag + dual-call execution in `reflection.c`
- US-11: `scripts/eval_reflection_shadow.py` Jaccard + critical-miss + calibration metrics
- US-12: Retire-on-contradiction wired to reaction collector
- US-13: First 14-day shadow eval; verdict JSON committed under `docs/plans/2026-05-26-reflection-loop/results/`

**Phase 2 (sprint 5+, separate spec):** Belief updates with quorum gate. Out of scope here.

## Acceptance criteria (Phase 1)

- AC-1: With `cfg.reflection.enabled = true`, daemon emits exactly one reflection run within 24h of startup on an active account
- AC-2: A reflection run produces ≥3 typed patterns covering ≥2 of the 6 pattern types on a corpus of >20 turns
- AC-3: Same conversation corpus re-run on two consecutive reflections: ≥80% pattern overlap by stable id (dedup works)
- AC-4: Malformed model output → `reflection_runs.status = 'schema_invalid'`, zero patterns inserted, daemon continues to serve channels normally
- AC-5: `hu_reflection_query_for_system_prompt(db, "imessage", 5)` returns at most 5 patterns, all `channels` containing "imessage" OR `channel_count > 1`
- AC-6: init_proposer can read `query_unsurfaced` results and surface one without crashing; on user contradiction, pattern is retired and never re-surfaces
- AC-7: With `HU_ENABLE_SQLITE` off, `src/reflection/` compiles and `hu_reflection_tick` returns HU_OK after emitting one-shot disabled-log

## Risks

- **R1 — Reflection model hallucinates patterns** that didn't happen. *Mitigation:* confidence floor (Layer 2) + retire-on-contradiction (Layer 4) + quorum gate before Phase 2 belief updates. Cloud Gemini 3.1 Pro hallucination rate on structured-output tasks is empirically low; risk increases when we move to local Gemma where the model is smaller.
- **R2 — Daemon stalls during 30-60s reflection run.** *Mitigation:* reflection runs ≤2 times per day; channel poll backups catch up trivially; if ever measured > 2min in practice, migrate to subprocess pattern (Approach B from brainstorm) — call site stays the same.
- **R3 — Pattern bloat in system prompt** if filter is too permissive. *Mitigation:* hard LIMIT 5, confidence threshold 0.7, 7-day recency window, surfaced flag prevents repeats.
- **R4 — Local Gemma reflection quality unproven.** *Mitigation:* shadow mode is the entire point; never flip default until eval ratchet criteria met. Cloud stays as fallback.
- **R5 — Storage growth.** ~5-20 patterns/run × ~2 runs/day × ~365 days = ~3K-15K rows/year, with retire/expire pruning. Negligible.
- **R6 — Pattern id collisions** across semantically different observations. *Mitigation:* id hashes (type, subject, observation[:128]) — observation prefix is part of the key, so "Seth mentioned insomnia 3x" and "Seth mentioned work stress 2x" hash differently. Collisions theoretically possible but vanishingly rare; if observed, extend hash input.

## Open questions

1. **Should reflection have visibility into HuLa tool-call history**, or just user-facing conversation turns? *Default:* conversation turns only for Phase 1; HuLa traces add noise without proportional signal. Revisit if reflections feel shallow.
2. **Pattern retirement: TTL vs explicit only?** *Default:* explicit retire-on-contradiction + 30-day half-life expiration. No automatic TTL beyond that.
3. **Multiple personal-model subjects** (Seth + family members)? *Default:* `subject` field supports any string. Reflection model decides who a pattern is about. Cross-subject patterns (e.g., "Seth tends to mention his wife more on weekends") use subject="Seth" with the wife mentioned in observation text.

## Related rules

- `~/.claude/rules/silent-config-gated-subsystems.md` — one-shot logs on enable/disable
- `~/.claude/rules/security-predicate-extraction.md` — `hu_reflection_pattern_has_quorum` is a pure predicate testable without forking
- `~/.claude/rules/audit-verify-before-allege.md` — eval harness must verify negative claims (e.g., "local missed pattern X") by checking presence/absence empirically
- `.claude/rules/tests-that-pin-bugs.md` — quorum test must assert NO mutation occurs under Phase 1, not assert the value of any field that might be set if mutation happens
- `.claude/rules/test-source-gate-symmetry.md` — `HU_ENABLE_SQLITE` gating must match between src/ and tests/
- `.claude/rules/lora-scale-default-or-die.md` — if Phase 3 LoRA happens, scale stays at 2.0 (mlx_lm default)
- `.claude/rules/classifier-score-plus-flag-gate.md` — when consumer.c filters patterns for system prompt, hybrid score + flag pattern applies if we add "always block" flags later
