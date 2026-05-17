# Sprint 33 — Review

**Branch:** `sprint-33-wire-ex-api`
**Stories:** S1 (helper), S2 (3 call sites), S3 (REJECT log fields), S4 (helper tests).

## Demo

Sprint 31 introduced `hu_response_guard_check_ex` with G5 (length
anomaly) and G6 (director echo). They were unit-tested but production
still called the legacy ctx-less path, so a 1.8 KB CoT dump in a
150-byte channel was REJECTed only in tests, not at runtime.

Sprint 33 wires it up:

```c
hu_guard_context_t guard_ctx;
memset(&guard_ctx, 0, sizeof(guard_ctx));
guard_ctx.recent_avg_len =
    hu_agent_internal_recent_assistant_avg_len(agent, 5);
hu_error_t guard_err = hu_response_guard_check_ex(
    agent->alloc, sresp.content, sresp.content_len, &guard_ctx,
    &guard_out, &guard_out_len, &guard_outcome, &guard_report);
```

REJECT logs now identify the detector:

```
[agent_stream] response_guard REJECT: stream final
   (len=174, recent_avg=0)
   [semantic=0 length=0 director=0 repetition_run=63] —
   retrying once with repair prompt
```

This output came out of an actual streaming test in the suite — the
wire is live.

## Acceptance check

- [x] **S1**: `hu_agent_internal_recent_assistant_avg_len(agent, max_n)`
      lives in `src/agent/agent.c`, declared in `agent_internal.h`.
- [x] **S2**: All 3 production call sites switched to
      `hu_response_guard_check_ex`:
        - `agent_stream.c:1400` (no-tool-calls final)
        - `agent_stream.c:2131` (post-process final)
        - `agent_turn.c:5566` (post-stream final)
- [x] **S3**: REJECT log lines emit `len`, `recent_avg`, `semantic`,
      `length`, `director`, `repetition_run` fields. REWROTE log
      format unchanged (still emits `bytes_stripped`, `harmony`, `think`).
- [x] **S4**: Three new helper tests pass:
        - `agent_recent_assistant_avg_len_empty_history_returns_zero`
        - `agent_recent_assistant_avg_len_mixed_roles_skips_non_assistant`
        - `agent_recent_assistant_avg_len_uses_most_recent_n`

## Test results

- Response Guard suite: 44/44 (was 41 in Sprint 32; +3 new helper tests).
- Full dev suite: 10301/10301 (was 10298 in Sprint 32; +3 = matches).
- 0 ASan errors. Clean build with `-Wall -Wextra -Wpedantic -Werror`.

## Behavioral guarantee

For an agent that has had at least one prior assistant turn:

| Scenario | recent_avg_len | response_len | G5 fires? | Outcome |
|---|---|---|---|---|
| Cold start, first reply | 0 | any | no | preserved (legacy behavior) |
| Normal convo (~150 byte avg) | 150 | 150 | no | OK |
| Normal convo + slight verbose | 150 | 700 | no (4.7×) | OK |
| Normal convo + CoT dump | 150 | 1800 | yes (12×) | REJECT, retry slim |
| Tiny acks (avg ~10 bytes) | 10 | 100 | yes (10×) | REJECT, retry slim |

Threshold is 8× from `HU_GUARD_LENGTH_ANOMALY_MULT` (Sprint 31).

## What's in (Sprint 33)

- `src/agent/agent.c` +28 LOC (helper).
- `src/agent/agent_internal.h` +12 LOC (declaration + comment).
- `src/agent/agent_stream.c` 2 sites: 16 LOC delta.
- `src/agent/agent_turn.c` 1 site: 13 LOC delta.
- `tests/test_response_guard.c` +120 LOC (3 tests + forward decl).
- `sprints/sprint-33/{stories,review,retro}.md`.

## Out of scope (deferred)

- **Director echo (G6) wiring.** Requires `agent->scene_direction_text`
  + `_len` set by daemon before each turn. Plumbing lives in `daemon.c`
  (`hu_director_result_t.direction[512]`). Sprint 34 candidate.
- **Quality gate `MARGINAL → REJECT` policy** in `reflection.c`. The
  guard now REJECTs at 8× length anomaly. Whether reflection should
  also REJECT at a lower (5×) threshold becomes a measurement question
  once we have runtime telemetry from the wired guard. Sprint 34/35.
- **Daemon-side warn log on every guard REJECT.** The guard already
  logs `hu_log_error("response_guard REJECT: ...")` at all 3 sites.
  A separate Slack/PagerDuty wire is out of scope until those are
  configured.
- **Persona-derived dynamic detector.** Future sprint.
- **CI/cron schedule for `scripts/audit-imessage-leaks.sh`.** Future.
