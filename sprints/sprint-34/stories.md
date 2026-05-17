# Sprint 34 — G6 wiring + end-to-end integration tests

**Branch:** `sprint-34-wire-g6-and-integration`
**Sprint goal:** wire G6 (director-string echo) into production and
prove G5 + G6 fire end-to-end through real `hu_agent_turn` calls.

## Background

End of Sprint 33 review:

> "Director echo (G6) wiring requires `agent->scene_direction_text`
>  field set by daemon before each turn. Plumbing churn is wider
>  than this sprint; defer until daemon-side scene_direction state
>  is consolidated."

> "Integration test: deterministic mock provider that emits a 10×
>  length-anomaly response after seeding short prior replies."

Both shipped here.

## Stories

### S1 — `hu_agent_t.scene_direction_text` field + setter/clear helpers

**As a** daemon
**I want** a non-owning pointer slot on the agent that the response_guard reads
**so that** I can pass scene-direction text per-turn without changing
the call signature of `hu_agent_turn` / `hu_agent_turn_stream_v2`.

**Acceptance:**

- `hu_agent_t` adds `const char *scene_direction_text;` and
  `size_t scene_direction_text_len;` next to `conversation_context`.
- `agent_internal.h` declares
  `hu_agent_internal_set_scene_direction(agent, text, len)` and
  `hu_agent_internal_clear_scene_direction(agent)` as thin wrappers
  around field assignment (kept for potential future test ergonomics
  but the daemon assigns the fields directly).
- All implementations are in `src/agent/agent.c`.

### S2 — Wire `director_text` into all 3 guard call sites

**As an** operator
**I want** the production guard to reject director-echo responses
**so that** a verbatim quote of "casual short, dry" by the model is
REJECTed at runtime, not just in unit tests.

**Acceptance:**

- `agent_turn.c` and both `agent_stream.c` sites populate
  `guard_ctx.director_text` and `guard_ctx.director_len` from
  `agent->scene_direction_text` / `_len`.
- NULL pointer / 0 length preserves cold-start behavior (no G6
  enforcement on the first turn before the daemon has set anything).

### S3 — Daemon plumbing

**As a** daemon
**I want** to set scene_direction on the agent before every turn
**so that** the wired guard knows what the director said.

**Acceptance:**

- `daemon.c` sets `agent->scene_direction_text` from
  `director_result.direction` *just after* injecting the same string
  into `conversation_context` (existing block at `daemon.c:9337`).
- `daemon.c` clears the pointer to NULL/0 *just after* `hu_agent_turn`
  returns and *before* `director_result` goes out of scope. Without
  this, a subsequent turn could read stack-stale memory through
  `hu_guard_context_t.director_text`.

### S4 — End-to-end integration tests for G5 and G6

**As a** maintainer
**I want** tests that drive `hu_agent_turn` with a mock provider
**so that** I have proof the wiring fires on a real agent path,
not just in `_ex` unit tests.

**Acceptance:**

- `tests/test_response_guard_retry.c` adds:
  - `agent_g5_length_anomaly_rejects_and_retries` — mock provider
    returns 21-byte / 22-byte short replies for turns 1-2 (seeding
    history → recent_avg ≈ 21), then a 1500-byte varied reply on
    turn 3. G5 must fire (1500/21 ≈ 71× ≫ 8×), retry must succeed,
    surfaced reply must be < 200 bytes.
  - `agent_g6_director_echo_rejects_and_retries` — agent's
    `scene_direction_text` set to a 74-byte directive; mock returns
    a 93-byte reply that contains the directive verbatim. G6 must
    fire, retry must succeed, surfaced reply must not contain the
    directive substring.
- Both tests inspect surfaced response, not internal state — so they
  prove user-visible behavior, not just guard internals.

## Definition of Done

- All 4 stories shipped.
- Dev build clean with `-Wall -Wextra -Wpedantic -Werror`.
- Full dev test suite passes 10303/10303 (10301 + 2 new), 0 ASan errors.
- Suite logs visibly emit
  `[agent_turn] response_guard REJECT: ... [semantic=0 length=1 director=0 ...]`
  and
  `[agent_turn] response_guard REJECT: ... [semantic=0 length=0 director=1 ...]`
  during the new tests — proof the new code paths fire.
- Branch tagged `v-sprint-34-close` and cherry-picked to `h-uman` main.

## Out of scope (future)

- **Quality-gate `MARGINAL → REJECT` policy** (Sprint 28 carry-over).
  With G5 + G6 wired at 8× / 30-byte thresholds, decide whether the
  reflection layer needs an additional 5× MARGINAL → REJECT band.
  Measure runtime telemetry first (Sprint 35).
- **Persona-derived dynamic detector.** Read loaded persona fields
  (name, age, profession) and add them as an implicit "director_text"
  alongside the actual scene direction. Future sprint.
- **Per-channel length thresholds.** iMessage 8× ≠ Slack 8× ≠ email
  8×. Future enhancement once we have channel-level telemetry.
- **Multi-turn director memory.** Currently G6 only checks against
  the *current* turn's director text. A scene direction from turn N-1
  could still be echoed in turn N. Future enhancement.
