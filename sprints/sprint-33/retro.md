# Sprint 33 — Retrospective

## What went well

- **Tight scope held.** Three call sites, one helper, three tests.
  Sprint shipped in well under a day. The temptation to also wire G6
  (director echo) and rewrite the quality gate was real, but the
  Sprint 31 review had already flagged "production still calls the
  legacy path" as the highest-leverage carry-over — and that's what
  we did.
- **Streaming test caught the wire live.** During `human_tests` run,
  a streaming test triggered the new code path and emitted the new
  REJECT log format end-to-end. Better proof than any synthetic test:
  ```
  response_guard REJECT: stream final (len=174, recent_avg=0)
    [semantic=0 length=0 director=0 repetition_run=63]
  ```
  This came out of `stream_guard_buffers_raw_output_until_retry_passes`
  — an existing test that suddenly exercised our wiring without
  modification, exactly as designed.
- **Forward-declaring the helper avoided header churn.** The helper
  is internal (`src/agent/agent_internal.h`); exposing it publicly
  would have meant editing `include/human/agent.h` and possibly the
  ABI. A 1-line forward decl in the test file gave us coverage
  without surface area.
- **Cold-start safety.** `recent_avg_len = 0` is treated by the guard
  as "no signal, do not enforce G5". That means a brand-new conversation
  (no prior assistant history) gets exactly the legacy behavior, not
  a spurious REJECT on the first reply. Conservative default; no
  behavior change for fresh agents.
- **Single-pass test fixture pattern.** The helper unit tests build a
  stack-allocated `hu_owned_message_t[]` and a zeroed `hu_agent_t` —
  no allocator, no provider, no setup. Each test stands alone and is
  trivially debuggable.

## What didn't go well

- **G6 wiring blocked on daemon plumbing.** I wanted to wire G6
  (director echo) too, but the active scene-direction text lives on
  `hu_director_result_t.direction[512]` inside `daemon.c` and is not
  threaded into the agent. Doing it right means adding a non-owning
  `agent->scene_direction_text` field set by the daemon before each
  turn — meaningful API surface change. Deferred to Sprint 34.
- **Quality-gate carry-over still pending.** The Sprint 28 carry-over
  was "MARGINAL → REJECT for length anomaly ≥ 5×". Our wiring REJECTs
  at 8× (the guard's `HU_GUARD_LENGTH_ANOMALY_MULT`). Whether the
  reflection layer should *also* reject at 5× becomes a tuning
  question that needs runtime telemetry from the now-wired guard.
  Honest call: leave it alone until we measure.
- **No integration test exercising G5 end-to-end.** The unit tests
  prove the helper works and the existing streaming tests proved the
  wire is connected, but there's no test that says "feed agent N
  short replies, then a 10× reply, watch it REJECT-and-retry via
  G5". Building that test means a deterministic mock provider that
  returns a long string on demand — feasible in tests/, but Sprint 34.

## Action items for Sprint 34

1. Wire G6 (director echo): add `agent->scene_direction_text/_len`
   non-owning fields, set them from `daemon.c` before each turn,
   pass via `hu_guard_context_t.director_text`. End-to-end test
   that proves a verbatim quote of the director string is REJECTed.
2. Integration test: deterministic mock provider that emits a 10×
   length-anomaly response after seeding the agent's history with
   short prior replies. Verify REJECT, slim retry, recovery.
3. Decide on the Sprint 28 quality-gate carry-over: with G5 wired,
   is the 5× MARGINAL→REJECT in `reflection.c` still needed? Measure
   first with `human-daemon` running for a week, then act.
4. Consider promoting `hu_agent_internal_recent_assistant_avg_len`
   to a public API (`hu_agent_recent_assistant_avg_len`) if any
   non-guard callers want it. Don't rush — only promote when there's
   a second caller.

## Sprint metrics

| Metric | Value |
|---|---|
| Stories shipped | 4/4 |
| Production call sites updated | 3 |
| New helper functions | 1 |
| New unit tests | 3 |
| Total response_guard tests | 44 (was 41) |
| Total dev suite | 10301 (was 10298) |
| Lines added (production) | ~70 |
| Lines added (tests) | ~120 |
| Sprint duration | < 1 day |
| Behavioral regression | 0 (cold-start preserves legacy path) |
