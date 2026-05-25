# Self-Model Scaffold — Requirements

## Goal

Add a continuous, turn-by-turn behavioral observation layer that lets the agent represent and reason about its own response patterns over time — separately from the prescriptive persona ("who I should be"), the user-side personal model ("who the user is"), and reactive calibration ("how to adjust given user feedback").

Today, the codebase has prescriptive (`hu_self_model_t` cell, persona deltas) and reactive (calibration, reflection engine driven by user feedback) layers, but NO continuous self-observation: nothing records the agent's own per-turn response length, tool-use sequence, emotional register, or response latency in a form the agent can later reflect on without external user correction.

This spec is **observe-only**. It establishes the substrate. Acting on observations (auto-correcting drift, surfacing to user, biasing future turns) is explicitly deferred to follow-up work.

## User stories

- As an agent operating on the user's behalf, I want a turn-by-turn behavioral record of my own responses (length, tool sequence, emotional register, latency), so that I can detect drift from my own baseline without waiting for user correction.
- As a user, I want the system that represents me to also notice when *it* is changing — e.g., when my agent has gradually become more terse in our chats over the past two weeks — so that style shifts don't accumulate invisibly.
- As a developer, I want a clean seam to plug self-observation in without refactoring the persona, memory, or world-model subsystems, because each of those is load-bearing today.
- As an operator, I want self-observation to be feature-flag-gated and zero-cost when disabled, so that it can ship behind a flag while we tune what's actually useful.

## Acceptance criteria

- [ ] **AC-SM-1: Per-turn behavior log.** A new `hu_agent_behavior_log_t` (circular buffer; size configurable via `self_model.behavior_log_capacity`, default 256 turns; in-memory plus optional SQLite-backed persistence behind feature flag) records on every turn:
  - `response_length_chars`, `response_length_tokens_est`
  - `tool_sequence_hash` (FNV-1a of the ordered tool names invoked)
  - `tool_count`
  - `emotional_register` (enum from existing world model)
  - `response_latency_ms`
  - `contact_hash`, `channel_id`, `persona_delta_kind` (NONE if no delta applied)
  - `timestamp_utc_ms`
  Pinned by a test that runs three fixture turns and asserts the buffer head advanced by exactly three with the recorded fields matching the turn inputs.
- [ ] **AC-SM-2: Single write site.** The log is populated from exactly ONE call site: an extension of `hu_agent_m3_route_per_turn()` (so both streaming and non-streaming paths share it, contingent on Spec 1 / AC-M3-2 closing the streaming gap). Pinned by a grep-based test that fails the build if `hu_agent_behavior_log_record(` appears more than once outside `tests/`.
- [ ] **AC-SM-3: Periodic aggregation into self-observations.** A new daemon tick (default every 100 turns OR 1 hour, whichever first; both configurable) computes aggregate `hu_agent_self_observation_t` records over the recent log window:
  - mean response length and standard deviation
  - tool-selection entropy (Shannon over recent tool-sequence-hashes)
  - emotional-register distribution
  - response-latency p50 / p95
  Stored in a new SQLite table `agent_self_observations` (one row per aggregation tick).
- [ ] **AC-SM-4: World-model integration via existing seam.** `hu_self_model_t` (already exists at `include/human/agent/world_model.h:196–209`) gains a `recent_self_observations` field. The existing `hu_world_model_merge_self_*` family at `src/agent/world_model_bridge.c` is extended (NOT replaced) to populate this field from the most recent `agent_self_observations` row. Existing world-model tests continue to pass; the change is additive.
- [ ] **AC-SM-5: Drift signal emission.** When an aggregation tick produces an observation whose `response_length_mean` deviates from the calibrated baseline by more than `self_model.drift_threshold_sigma` (default 2.0), a structured `hu_self_concern_t` is appended to a new log structure (`agent_self_concerns`), with the affected dimension, the magnitude, and the window. **Not wired into prompt assembly in this spec.** Pinned by a test that injects synthetic drift and asserts the concern is recorded.
- [ ] **AC-SM-6: Feature-flag gated, zero-cost when off.** A new build flag `HU_ENABLE_SELF_MODEL` gates all of the above. When the flag is OFF: `hu_agent_behavior_log_record()` is a no-op inline that compiles away; no SQLite table is created; no daemon tick fires. Pinned by a build-variant CI check that the disabled-flag build's binary size grows by less than 1 KB versus the pre-spec baseline (per `~/.claude/rules/test-source-gate-symmetry.md`).
- [ ] **AC-SM-7: Privacy hygiene.** The behavior log records hashes and aggregates only — never user message content, never agent response text, never tool arguments. Pinned by a grep-based test in `tests/` that fails the build if the new code includes any logging of `message`, `body`, `content`, `args`, or other content-carrying string fields.

## Non-goals

- **Acting on self-observations.** No auto-correction, no prompt-injection of self-concerns, no agent-side behavioral modification driven by the log. This is observe-only substrate.
- **Surfacing observations to the user.** No UI, no chat output, no notification.
- **Real-time observation.** Periodic aggregation is fine; per-turn realtime is not required.
- **Cross-user observation.** Self-model is per-h-uman-process; multi-user shared self-models are out of scope.
- **Defining "good" baselines.** We measure delta from the existing calibrated baseline (`hu_calibration_*`); calibration is unchanged by this spec.
- **Persistent SQLite is optional.** In-memory ring is enough for AC-SM-1 to AC-SM-5; persistence is behind a config flag. (The default may be persistent or non-persistent; design phase decides.)

## Constraints

- C11 `-Wall -Wextra -Wpedantic -Werror`, ASan-clean.
- Reuses existing SQLite memory backend; no new persistence layer.
- Reuses existing world-model merge family; no new top-level subsystem.
- Per-turn hot path: zero heap allocations beyond the ring-buffer slot write. Aggregation is on the daemon tick, off the hot path.
- Tests deterministic; no real wall-clock; injected timestamps for window testing.
- Backwards-compatible. Existing tests pass with `HU_ENABLE_SELF_MODEL=OFF` AND with it ON.
- Privacy invariants enforced by automated grep, not by reviewer attention.
- Build flag follows the project's existing gate pattern (`HU_ENABLE_ML`, `HU_ENABLE_LEARNING`); test/source gate symmetry per `~/.claude/rules/test-source-gate-symmetry.md`.

## Glossary

- **Self-model**: agent's representation of its own behavior over time. Distinct from the persona (prescriptive) and the user's personal model.
- **Behavior log**: the per-turn ring buffer of metrics recorded at the route_per_turn hook.
- **Self-observation**: a periodic aggregate row computed over a window of the behavior log.
- **Self-concern**: a flagged dimension where an observation exceeds the drift threshold.
- **Calibrated baseline**: existing `hu_calibration_*` per-contact baseline; this spec does not modify it.
