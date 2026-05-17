# Sprint 37 — Review

**Branch:** `sprint-37-multi-turn-director`
**Stories:** S1 (struct fields), S2 (helpers), S3 (context + detector
extension), S4 (3 call sites), S5 (daemon plumbing), S6 (12 tests).

## Demo

Cross-turn G6 fires through `hu_agent_turn`:

```
[agent_turn] response_guard REJECT: turn final (len=62, recent_avg=0)
   [semantic=0 length=0 director=1 persona=0 identity=0
    repetition_run=2] — retrying once with repair prompt
[agent_turn] response_guard RECOVERED: retry passed (len=8, stripped=0)
PASS  agent_g6_history_cross_turn_rejects_and_retries
```

The agent had **NO** current `scene_direction_text` (NULL) — the only
director text in scope was the heap-owned ring buffer holding
yesterday's director. The mock provider quoted 30+ chars of that
historical director; G6 fired via the new history path. Slim retry
returned a clean 8-byte reply.

Daemon clear-on-exit also verified end-to-end:

```
PASS  agent_clear_on_exit_no_stale_memory
```

This test simulates the daemon's exact lifecycle: turn 1 sets a
stack-local director on the agent, then pushes it to history and NULLs
the pointer; the stack frame goes out of scope; turn 2 runs cleanly
with a benign mock. Under ASan, any read of stale stack memory through
`scene_direction_text` would crash here. It doesn't — proving the
Sprint 34 clear-on-exit invariant holds.

## Acceptance check

- [x] **S1**: `hu_agent_t` extended with `director_history[N]`,
      `director_history_lens[N]`, `director_history_count`. N defined
      via `HU_DIRECTOR_HISTORY_MAX` (4). Per-entry cap defined via
      `HU_DIRECTOR_TEXT_CAP` (256).
- [x] **S2**: `hu_agent_internal_push_director_history` and
      `hu_agent_internal_free_director_history` implemented.
      `hu_agent_deinit` calls the free helper first to avoid leaks.
      Push truncates oversized strings to `HU_DIRECTOR_TEXT_CAP`,
      evicts the oldest slot when full, shifts entries to keep
      slot-0-most-recent ordering.
- [x] **S3**: `hu_guard_context_t` extended with `director_history`,
      `director_history_lens`, `director_history_count`. G6 detector
      refactored: shared sliding-window helper
      (`hu_guard_director_window_matches`) used for both current
      `director_text` and history slots. No new report flag —
      `detected_director_echo` covers cross-turn matches just as well.
- [x] **S4**: All 3 production call sites populate the new context
      fields from `agent->director_history*`.
- [x] **S5**: `src/daemon.c` calls
      `hu_agent_internal_push_director_history` immediately before
      clearing `scene_direction_text` at end-of-turn. Idempotent on
      NULL/short directors.
- [x] **S6**: 12 new tests:
       - 5 G6 history unit tests (catches previous director, skips
         below-threshold entries, orthogonal-to-current,
         zero-count-disables, walks all slots).
       - 5 helper unit tests (push basic, overflow evicts oldest,
         truncates long, NULL/short is noop, free zeroes count).
       - 2 integration tests (cross-turn rejection + retry,
         clear-on-exit no stale memory).

## Test results

- Response Guard combined suite: 77/77 (was 65 in Sprint 36; +12).
- Full dev suite: 10334/10334 (was 10322; +12 = matches).
- 0 ASan errors. Clean build with `-Wall -Wextra -Wpedantic -Werror`.
- 0 cross-layer topology violations.

## Bonus: pre-existing test fragility fixed

`tests/test_model_router.c::route_populates_global_log` failed once
the Sprint 37 integration tests pushed the global route log past its
100-entry cap (`HU_ROUTE_LOG_SIZE`). Hardened by resetting the global
log at test start via `hu_route_log_init(log)`. The fix is independent
of the new feature and applies cleanly to any future test that adds
agent_turn-driven scenarios.

## Behavioral guarantee

| Scenario | scene_direction | history_count | response | G6 fires? | Source |
|---|---|---|---|---|---|
| Current-turn echo | "casual short..." (60B) | 0 | quotes 30B of current | yes | current |
| Cross-turn echo | NULL | 1 | quotes 30B of history[0] | yes | history |
| Mixed: only history | "today's director" | 1 | quotes 30B of history[0] | yes | history |
| Below threshold history | NULL | 1 (12B entry) | quotes "casual brief" (12B) | no | — |
| Empty history | NULL | 0 | any | no | — |
| Cold-start (no agent persona) | — | 0 | — | no (preserved) | — |
| Walks all 4 slots | NULL | 4 | quotes 30B of history[3] (oldest) | yes | history slot 3 |

Cold-start safety: agents that never set `scene_direction_text`
(test agents, CLI without director, etc.) see byte-identical legacy
guard behavior — `director_history_count = 0` skips the new path.

## Daemon lifecycle (post-Sprint 37)

```
turn N starts:
   ├─ agent->scene_direction_text  =  director_result.direction (stack)
   └─ agent->director_history[0..count]  =  past directors (heap-owned copies)

guard runs (G6 enforces both):
   ├─ check current director_text
   └─ for each history slot, check it

turn N ends:
   ├─ hu_agent_internal_push_director_history(agent, scene_direction_text, ...)
   │     ↳ allocates copy → slot 0 (shifts older slots down, evicts oldest)
   ├─ agent->scene_direction_text  =  NULL  (stack ptr goes out of scope safely)
   └─ agent->scene_direction_text_len  =  0

agent deinit:
   └─ hu_agent_internal_free_director_history(agent)  (frees all slots)
```

## What's in (Sprint 37)

- `include/human/agent.h` +28 LOC (3 ring buffer fields + macros).
- `include/human/agent/response_guard.h` +9 LOC (3 context fields).
- `src/agent/response_guard.c` +24 LOC (sliding-window helper +
  G6 history iteration).
- `src/agent/agent.c` +63 LOC (push + free helpers + deinit hook).
- `src/agent/agent_internal.h` +20 LOC (declarations + comments).
- `src/agent/agent_stream.c` +9 LOC (×2 sites populate history ctx).
- `src/agent/agent_turn.c` +5 LOC (×1 site).
- `src/daemon.c` +11 LOC (1 push call + 1 include).
- `tests/test_response_guard.c` +275 LOC (10 unit tests).
- `tests/test_response_guard_retry.c` +135 LOC (2 integration tests).
- `tests/test_model_router.c` +6 LOC (test robustness fix).
- `sprints/sprint-37/{stories,review,retro}.md`.

## Out of scope (deferred)

- **Quality gate `MARGINAL → REJECT`** (Sprint 28 carry-over).
- **Per-channel length thresholds.**
- **Widen G7 lookahead 30 → 60 bytes.**
- **Extend G8 to `persona->biography`.**
- **CI/cron schedule for `audit-imessage-leaks.sh`.**
- **Telemetry counter for G8 hits** (Sprint 36 retro).
