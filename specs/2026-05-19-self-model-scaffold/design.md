# Self-Model Scaffold — Design

## Components

- **Per-turn behavior log** — new in-memory ring buffer `hu_agent_behavior_log_t` with optional SQLite persistence. Declared in: `include/human/agent/self_model.h` (new file). Implementation in: `src/agent/self_model.c` (new). Default capacity 256 turns; configurable via `self_model.behavior_log_capacity`.
- **Per-turn record hook** — new function `hu_agent_behavior_log_record(...)` invoked from inside `hu_agent_m3_on_provider_success()` (per-spec single canonical call inside that function; on_provider_success itself is called from 11 sites that cover both streaming and non-streaming paths). **This differs from the original draft** which placed the hook at `hu_agent_m3_route_per_turn()`; recon Q-SM-A confirmed `emotional_register` is NOT populated at route_per_turn time (the world-model merge runs later during prompt assembly), so logging it there would record NULL/stale values. on_provider_success runs after provider return, by which time world-model state is populated.
- **Aggregation tick** — new daemon tick `hu_daemon_tick_self_observation_aggregate()` that runs every N turns OR T minutes (defaults 100 turns / 60 min). Computes `hu_agent_self_observation_t` over the recent ring window.
- **`agent_self_observations` SQLite table** — new schema:
  ```sql
  CREATE TABLE agent_self_observations (
    id INTEGER PRIMARY KEY,
    window_start_ts_ms INTEGER NOT NULL,
    window_end_ts_ms INTEGER NOT NULL,
    n_turns INTEGER NOT NULL,
    response_length_mean REAL,
    response_length_stddev REAL,
    tool_selection_entropy REAL,
    emotional_register_dist BLOB,    -- JSON or packed enum-count map
    latency_p50_ms INTEGER,
    latency_p95_ms INTEGER,
    contact_hash_set BLOB             -- optional, set of contact hashes seen in window
  );
  ```
  Lives in: extension of the existing SQLite migration pipeline (whichever module owns schema versioning).
- **`agent_self_concerns` SQLite table** — new schema:
  ```sql
  CREATE TABLE agent_self_concerns (
    id INTEGER PRIMARY KEY,
    observation_id INTEGER NOT NULL,
    dimension TEXT NOT NULL,       -- 'response_length', 'tool_entropy', 'emotion_consistency'
    magnitude_sigma REAL NOT NULL,
    window_n_turns INTEGER NOT NULL,
    created_ts_ms INTEGER NOT NULL,
    FOREIGN KEY (observation_id) REFERENCES agent_self_observations(id)
  );
  ```
- **World-model integration** — extension of `hu_self_model_t` (existing at `include/human/agent/world_model.h:196`) with a `recent_self_observations[N]` field (small fixed array, default 4 most-recent rows). New merge function `hu_world_model_merge_self_observations()` in `src/agent/world_model_bridge.c`, called from the existing merge orchestrator.
- **Feature flag** — new `HU_ENABLE_SELF_MODEL` in CMake, defaults to OFF. Wraps all new code per `~/.claude/rules/test-source-gate-symmetry.md`. When OFF, `hu_agent_behavior_log_record` is `static inline void ... { (void)args; }`.
- **Privacy-hygiene grep test** — new `tests/test_self_model_no_content_capture.c` that fails the build if `src/agent/self_model.c` or its header reference any of: `body`, `content`, `message`, `text`, `arg`, `prompt`, `response` as logged field names (allowlist for technical uses like `response_length`).

## Data flow

```
[Chat turn served, streaming or non-streaming]
   │
   ▼
[hu_agent_m3_route_per_turn()]
   │  (Spec 1 closes streaming-path call; for now non-streaming only)
   ▼
[hu_agent_behavior_log_record(
    response_length_chars, response_length_tokens_est,
    tool_sequence_hash, tool_count,
    emotional_register, response_latency_ms,
    contact_hash, channel_id, persona_delta_kind,
    timestamp_utc_ms)]
   │
   ▼
[Ring buffer head++ (mod capacity), zero alloc]

[Every 100 turns OR 60 min]
   │
   ▼
[hu_daemon_tick_self_observation_aggregate()]
   │
   ▼
[Compute over recent ring window:
   mean/stddev response_length,
   Shannon entropy over tool_sequence_hashes,
   emotional_register distribution,
   latency p50/p95]
   │
   ▼
[INSERT INTO agent_self_observations]
   │
   ▼
[For each dimension D, compute σ_D vs. calibrated baseline]
   │
   ▼
[If |σ_D| ≥ drift_threshold_sigma (default 2.0):
   INSERT INTO agent_self_concerns]

[At world-model assembly time]
   │
   ▼
[hu_world_model_merge_self_observations]
   │
   ▼
[Reads top-N recent observations + concerns]
   │
   ▼
[Populates hu_self_model_t.recent_self_observations]
   │
   ▼
[Planner sees observation deltas; this spec does NOT use them yet]
```

## Decisions

- **D-SM-1 (AC-SM-1, AC-SM-2): Single write call inside `hu_agent_m3_on_provider_success()`.** Chose on_provider_success over route_per_turn because recon Q-SM-A revealed `emotional_register` (one of the required log fields per AC-SM-1) is not populated until world-model merge runs DURING prompt assembly, which is after route_per_turn. on_provider_success runs after the provider returns and the world model is fully built. Single-write invariant: the hook function `hu_agent_behavior_log_record(` appears exactly once in `src/` (inside on_provider_success); the 11 sites that call on_provider_success are unchanged.
- **D-SM-2 (AC-SM-1): Ring buffer in memory; SQLite persistence optional.** Chose in-memory primary because: per-turn write must be O(1) and zero-alloc; SQLite-per-turn writes risk contention with other subsystems and add latency. Periodic flush to `agent_behavior_log` table can happen on the aggregation tick if persistence is enabled. Configurable via `self_model.persist_behavior_log` (default false).
- **D-SM-3 (AC-SM-3): Aggregation tick uses time + turn count.** Chose dual-trigger (whichever fires first) because: pure-time aggregation produces empty rows in idle periods; pure-turn aggregation produces stale aggregates if the user goes quiet. Dual gives both timely AND statistically meaningful windows.
- **D-SM-4 (AC-SM-4): World-model integration via existing merge family.** Chose extension over new merge orchestrator because the existing `hu_world_model_merge_self_*()` family already handles persona-side cells; adding `merge_self_observations` mirrors `merge_self_emotion` and `merge_self_recent_tools`. One existing pattern, not a new one.
- **D-SM-5 (AC-SM-5): Drift detection uses calibrated baseline + σ threshold.** Chose σ-threshold over absolute thresholds because response-length-distribution differs wildly across users; per-user calibrated baseline is the only meaningful comparator. The existing `hu_calibration_*` per-contact baseline supplies the baseline; per-window observation supplies the test. Default 2.0 σ is conservative; tunable.
- **D-SM-6 (AC-SM-6): Feature-flag gating via `HU_ENABLE_SELF_MODEL`.** Chose new flag (matching `HU_ENABLE_ML`, `HU_ENABLE_LEARNING` pattern) over reusing an existing flag because: this is genuinely a new subsystem; gating with `HU_ENABLE_ML` would either expose self-model code to non-ML builds or hide ML-adjacent code; both are wrong. Pinned by build-variant CI (existing).
- **D-SM-7 (AC-SM-7): Privacy hygiene enforced by grep test.** Chose grep enforcement (heavy-handed but unambiguous) over reviewer attention because the 2026-05-18 silent-no-op incident proved attention fails. Tradeoff: false positives on field names like `response_latency_ms` (allowlist with explicit suffix); the test maintains a small allowlist file.
- **D-SM-8: No agent-side action on observations.** This spec is observe-only. The aggregation output is *read* by the world model and *seen* by the planner, but no code in this spec modifies prompt assembly, persona application, or tool selection based on observations. A follow-up spec ("self-model feedback into planner") can wire that.

## Risks

- **Risk-SM-1 (D-SM-1): Single-write-site invariant breaks if `route_per_turn` semantics change.** **Mitigation:** the grep-based test in AC-SM-2's pin fails the build if `hu_agent_behavior_log_record(` appears elsewhere in `src/`. Future refactors will see the test fail loudly.
- **Risk-SM-2 (D-SM-5): Calibrated baseline may be poorly defined for new contacts (insufficient history).** **Mitigation:** the drift detector requires N ≥ 50 baseline turns before emitting concerns; below that, observations are recorded but no concerns are flagged. Configurable via `self_model.drift_minimum_baseline_n`.
- **Risk-SM-3 (D-SM-2): Ring buffer wrap loses data if aggregation tick falls behind.** With 256 capacity and 100-turn aggregation, headroom is 2.56×. **Mitigation:** raise capacity if profiling shows wrap occurs; or aggregate more often. Existing rotation/snapshot patterns in the codebase (e.g., outcome ring per Spec 1) handle this; reuse the same pattern.
- **Risk-SM-4 (D-SM-7): Privacy grep test produces false positives.** **Mitigation:** maintain a small allowlist; reviewer effort is small relative to the value of "content never logged" as a verified invariant.
- **Risk-SM-5 (Cross-spec): Streaming-path coverage depends on Spec 1 AC-M3-2.** Until that lands, the behavior log captures only non-streaming turns. **Mitigation:** call out in `tasks.md` that Spec 3's coverage is partial until Spec 1's streaming work lands; tasks ordered accordingly.

## Open design questions

- **Q-SM-A: emotional_register timing — RESOLVED.** Recon confirmed `hu_world_model_merge_self_emotion()` runs at `src/agent/world_model_bridge.c:221`, invoked during prompt-build at `agent_turn.c:3800-3950` — BEFORE `route_per_turn` at line 4224. By the time `hu_agent_m3_on_provider_success()` fires (after provider returns), the register IS populated. The hook site has been moved from route_per_turn to on_provider_success accordingly. See D-SM-1 update.
- Q-SM-B: `tool_sequence_hash` granularity — FNV-1a of `tool_name|tool_name|tool_name` (ordered, separator-delimited) gives distinct hashes for re-orderings, which is what we want. Confirm that tool names are stable identifiers (some tools may have versioned names).
- Q-SM-C: Should the aggregation tick handle a stalled aggregation gracefully (e.g., DB lock contention)? Match the existing tick error-handling convention. Defer to implementation.
