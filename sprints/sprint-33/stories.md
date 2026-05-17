# Sprint 33 — Wire `_ex` into production (G5 length anomaly)

**Branch:** `sprint-33-wire-ex-api`
**Sprint goal:** turn the Sprint 31 detectors on in production. The
G5 length-anomaly detector exists and is unit-tested but is not yet
called from `agent_stream.c` or `agent_turn.c`. This sprint closes
that gap so an iMessage CoT leak with the same shape as 2026-05-12
would be caught at runtime, not just in tests.

## Background

End of Sprint 31 honest review:

> "All these guard improvements are exercised by tests but the
>  production daemon still calls `hu_response_guard_check` (legacy
>  ctx-less path)."

Three call sites need updating:

| File | Site | Path |
|---|---|---|
| `src/agent/agent_turn.c` | 5566 | post-stream final check |
| `src/agent/agent_stream.c` | 1400 | streaming no-tool-calls final |
| `src/agent/agent_stream.c` | 2131 | streaming post-process final |

All three pass `agent` and have access to `agent->history`. The
`recent_avg_len` we want is "average content_len of the last N
assistant messages in this conversation". Director-echo (G6)
requires daemon-side plumbing of the active scene-direction buffer
into the agent and is deferred — wiring G5 alone closes the
length-anomaly leak class, which was the dominant signature on the
high-severity 2026-05-12 leaks (rowids 56354/56355 ran 1.8 KB vs.
~150 byte normal replies).

## Stories

### S1 — `hu_agent_recent_assistant_avg_len(agent, max_n)` helper

**As a** call site
**I want** an average length over the last N assistant content lengths
**so that** I can populate `hu_guard_context_t.recent_avg_len`.

**Acceptance:**

- New static helper `agent_recent_assistant_avg_len(...)` in
  `src/agent/agent_internal_helpers.c` (or inline in the call sites,
  whichever stays simpler).
- Walks `agent->history` from newest to oldest, counts up to
  `max_n` (=5) entries with `role == HU_ROLE_ASSISTANT && content_len > 0`,
  returns floor(sum / count). Returns 0 when count is 0 — guard
  treats 0 as "no signal, do not enforce G5", matching
  `hu_response_guard_check_ex(..., ctx=NULL)` behavior.
- Excludes tool messages, system messages, the in-progress
  assistant turn (we average the prior turns, not the candidate).
- Unit test: empty history → 0; mixed roles → counts only assistant
  messages; >N entries → uses the most recent N only.

### S2 — Switch all 3 guard call sites to `_ex`

**As an** operator
**I want** the production guard to reject length-anomaly responses
**so that** a 1.8 KB CoT dump in a 150-byte channel is REJECTed,
not REWROTEd, regardless of whether harmony tokens are present.

**Acceptance:**

- `agent_turn.c:5566`, `agent_stream.c:1400`, `agent_stream.c:2131`
  switch to `hu_response_guard_check_ex(...)` with a populated
  `hu_guard_context_t { recent_avg_len = avg, director_text = NULL,
  director_len = 0 }`.
- Behavior preserved when avg is 0 (cold start, first reply).
- All three sites must continue to call `hu_response_guard_retry_slim`
  on REJECT.

### S3 — REJECT log emits new detection bits

**As an** on-call
**I want** the daemon log to identify *which* detector REJECTed
**so that** future incidents are diagnosable in seconds, not hours.

**Acceptance:**

- All three sites' `hu_log_error("response_guard REJECT: ...")` lines
  include `semantic_leak`, `length_anomaly`, `director_echo`,
  `repetition_run`, `len`, and `recent_avg_len` fields.
- Existing `bytes_stripped` field on `hu_log_warn("REWROTE")` lines
  preserved unchanged.

### S4 — Integration tests

**As a** maintainer
**I want** tests that prove the wired call sites actually use the
new context
**so that** wiring doesn't silently regress to the legacy path.

**Acceptance:**

- New tests in `tests/test_response_guard.c`:
  - `agent_recent_assistant_avg_len_empty_history_returns_zero`
  - `agent_recent_assistant_avg_len_mixed_roles_skips_non_assistant`
  - `agent_recent_assistant_avg_len_uses_most_recent_n`
- All 10298 dev tests still pass; suite count grows by 3 → 10301.

## Definition of Done

- All 4 stories shipped.
- Dev build clean (`-Wall -Wextra -Wpedantic -Werror`).
- Full dev test suite passes 10301/10301, 0 ASan errors.
- Branch tagged `v-sprint-33-close` and cherry-picked to `h-uman` main.
- Sprint review/retro filed.

## Out of scope (future sprints)

- Director-echo (G6) wiring: requires `agent->scene_direction_text`
  field set by daemon. Plumbing churn is wider than this sprint;
  defer until daemon-side scene_direction state is consolidated.
- Quality-gate `MARGINAL → REJECT` policy in `reflection.c`. The
  Sprint 28 carry-over is real but lives at a different layer
  (reflection vs. guard). Address in Sprint 34 once we measure
  whether G5-at-runtime alone closes the leak.
- Daemon-side warn log on every REJECT: existing
  `hu_log_error("response_guard REJECT: ...")` already screams; add
  a Slack/PagerDuty wire when we have one.
- Persona-derived dynamic detector.
- CI/cron schedule for `scripts/audit-imessage-leaks.sh`.
