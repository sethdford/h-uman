# Sprint 34 — Review

**Branch:** `sprint-34-wire-g6-and-integration`
**Stories:** S1 (field + helpers), S2 (3 call sites), S3 (daemon plumbing), S4 (integration tests).

## Demo

Run the response_guard_retry suite — both new integration tests fire
the wired detectors live, with surfaced reply being the clean retry
text, not the leak:

```
[agent_turn] response_guard REJECT: turn final
   (len=1415, recent_avg=21)
   [semantic=0 length=1 director=0 repetition_run=2] —
   retrying once with repair prompt
[agent_turn] response_guard RECOVERED: retry passed (len=20, stripped=0)
PASS  agent_g5_length_anomaly_rejects_and_retries

[agent_turn] response_guard REJECT: turn final
   (len=93, recent_avg=0)
   [semantic=0 length=0 director=1 repetition_run=1] —
   retrying once with repair prompt
[agent_turn] response_guard RECOVERED: retry passed (len=10, stripped=0)
PASS  agent_g6_director_echo_rejects_and_retries
```

The G5 test seeded history with two 21/22-byte replies. The 1415-byte
third reply tripped G5 at ratio ≈ 67× (well past 8× threshold). The
G6 test set `scene_direction_text` and watched the mock return a
verbatim quote — `director=1` flag fired.

## Acceptance check

- [x] **S1**: `hu_agent_t.scene_direction_text` + `_len` added next to
      `conversation_context`. Setter/clear helpers in
      `agent_internal.h` + `agent.c`.
- [x] **S2**: All 3 guard call sites populate
      `guard_ctx.director_text` from agent fields:
        - `agent_stream.c:1404` (no-tool-calls final)
        - `agent_stream.c:2146` (post-process final)
        - `agent_turn.c:5593` (post-stream turn final)
- [x] **S3**: `daemon.c` sets agent fields after building
      conversation_context (line ~9357), clears them after
      `hu_agent_turn` returns (line ~9396) — before
      `director_result` (stack-resident) goes out of scope.
- [x] **S4**: Two new integration tests pass:
        - `agent_g5_length_anomaly_rejects_and_retries`
        - `agent_g6_director_echo_rejects_and_retries`

## Test results

- Response Guard suite: 44/44 (Sprint 33 baseline, unchanged).
- Response Guard Retry suite: 5/5 (was 3 in Sprint 33; +2 new).
- Full dev suite: 10303/10303 (was 10301; +2 = matches).
- 0 ASan errors. Clean build with `-Wall -Wextra -Wpedantic -Werror`.
- Layer topology check: 0 cross-layer violations.

## Behavioral guarantee

| Scenario | scene_direction_text | response | G6 fires? | Outcome |
|---|---|---|---|---|
| Cold start, no daemon set | NULL | any | no | preserved (legacy) |
| Daemon set, response no quote | "casual short" | "haha ok" | no | OK |
| Daemon set, response < 30b quote | "casual short" | "casual" (10b) | no | OK (under threshold) |
| Daemon set, response >= 30b verbatim | "Slightly skeptical..." | "got it - Slightly skeptical..." | yes | REJECT, retry slim |

Threshold is 30 bytes from `HU_GUARD_DIRECTOR_ECHO_MIN_MATCH` (Sprint 31).

## What's in (Sprint 34)

- `include/human/agent.h` +9 LOC (2 fields + comment).
- `src/agent/agent_internal.h` +12 LOC (helper decls + comment).
- `src/agent/agent.c` +20 LOC (set/clear helpers).
- `src/agent/agent_stream.c` 2 sites: 4 LOC delta.
- `src/agent/agent_turn.c` 1 site: 2 LOC delta.
- `src/daemon.c` ~12 LOC delta (set + clear blocks).
- `tests/test_response_guard_retry.c` +185 LOC (2 tests + mock provider).
- `sprints/sprint-34/{stories,review,retro}.md`.

## Out of scope (deferred)

- Quality-gate `MARGINAL → REJECT` policy (Sprint 28 carry-over) —
  measure first, decide later. Sprint 35 candidate.
- Persona-derived dynamic detector.
- Per-channel length thresholds.
- Multi-turn director memory (verbatim quote of *previous* turn's
  director text).
- CI/cron schedule for `scripts/audit-imessage-leaks.sh`.
