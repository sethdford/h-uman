# Sprint 37 — Retrospective

## What went well

- **Two long-standing carry-overs closed in a single sprint.** The
  cross-turn director gap was identified in Sprint 34's retro;
  daemon clear-on-exit testing was identified in Sprint 35's retro.
  Both share the same domain (director-text lifetime) and naturally
  pair. Shipping them together kept the agent's lifetime invariants
  consistent.
- **Refactored G6 into a shared primitive.** Sprint 31's
  `hu_guard_has_director_echo` had its sliding-window logic inline.
  Extracted into `hu_guard_director_window_matches(src, src_len, s,
  len)` so both current `director_text` and each history slot use the
  same code path. Easier to extend (Sprint 37+) and easier to reason
  about (single source of truth for the 30-byte threshold logic).
- **Lifetime safety proven, not just claimed.** The clear-on-exit
  test exercises the exact pattern that would have crashed under
  ASan if Sprint 34's `scene_direction_text = NULL` clear were ever
  removed: a stack-local director string set on the agent, then the
  stack frame goes out of scope, then the next turn runs. The test
  is deliberately scoped (an inner `{}` block) so the compiler
  invalidates the stack memory after the block. Under ASan, a
  use-after-free would crash here. It doesn't.
- **Eviction policy keeps memory bounded.** 4 slots × 256 bytes =
  1 KB max per agent, regardless of how many turns the daemon
  processes. Old directors are freed in FIFO order; no leak.
  Verified by the `agent_director_history_push_overflow_evicts_oldest`
  test (push 6 entries, only most-recent 4 retained).
- **Walks-all-slots test caught a subtle bug.** First version of the
  detector iterated `history[0]` only (typo: `< 1` instead of
  `< count`). The `guard_g6_history_walks_all_slots` test (with the
  match in the *oldest* slot) failed loudly — fixed before commit.
- **Fixed a pre-existing test fragility for free.** The global route
  log saturated at 100 entries; `route_populates_global_log` failed
  on full suite runs once enough agent_turn tests preceded it. Reset
  the log at test start. This was always a latent bug — Sprint 37
  just made it visible.

## What didn't go well

- **Macros leak from `agent.h` into every translation unit.**
  `HU_DIRECTOR_HISTORY_MAX` and `HU_DIRECTOR_TEXT_CAP` are now defined
  in the public header. This is fine for now (~1KB/agent overhead is
  acceptable), but if the cap or count ever needs per-channel tuning,
  we'd need to move them to a runtime config. Documented in stories.md
  as out of scope.
- **`agent_internal.h` cross-include from `daemon.c`.** Sprint 34
  intentionally avoided this by doing direct field assignment for the
  scene direction. Sprint 37's ring-buffer push needs allocation, so
  it has to call the helper — which lives in `agent_internal.h`. We
  followed the existing precedent (`gateway/openai_compat.c` already
  cross-includes), but it's a soft layering violation. A future
  refactor could move the public-facing helper to `agent.h` proper.
- **No log distinction for current-vs-history match.** Both sources
  set `detected_director_echo`. An operator looking at logs can't
  tell whether the model quoted today's or yesterday's director.
  Adding a `match_source` field to the report would help. Out of
  scope for now — the actionable info is "G6 fired", not "which
  slot fired". Could add in a future sprint if forensics need it.
- **The clear-on-exit test exercises the daemon flow but not the
  daemon itself.** The test simulates the daemon's pattern (set
  director → push → clear → next turn), but the daemon's actual
  end-of-turn code path (in `daemon.c`) is not exercised by the
  test binary because the daemon is a separate executable. We rely
  on visual code review of the `src/daemon.c` patch + the simulated
  test to give us confidence. A daemon-process integration test
  would close this gap but is large; out of scope.
- **`route_populates_global_log` fix is reactive, not preventive.**
  Other tests in the suite likely have similar dependencies on
  global-state freshness. We patched the one that fired; we didn't
  audit all of them. Sprint 38 candidate: sweep for global-state
  test fragility.

## Action items for Sprint 38

1. **Sweep tests for global-state dependencies.** The route-log fix
   was a one-off; there are likely other latent fragilities. Grep
   for `*_global_*` calls inside test bodies; flag any tests that
   depend on cumulative state.
2. **Persona biography as G8 input** (Sprint 36 carry-over). Adds
   `persona->biography` to the identity-echo detector for richer
   match coverage.
3. **G8 hit-rate telemetry** (Sprint 36 carry-over). Add a counter
   so we can tune the 25-byte threshold from real data after a week
   of runtime.
4. **Quality gate `MARGINAL → REJECT`** (Sprint 28 carry-over). Now
   that we have visibility into REJECT rates from G5-G8, decide
   whether to escalate the reflection layer's MARGINAL handling.
5. **Per-channel length thresholds.** Telegram/Discord allow longer
   replies than iMessage; G5's 8x multiplier applies uniformly.
   Could vary `recent_avg_len` lookback or multiplier by channel.

## Sprint metrics

| Metric | Value |
|---|---|
| Stories shipped | 6/6 |
| Carry-overs closed | 2 (Sprint 34's multi-turn director, Sprint 34's clear-on-exit test) |
| New ring-buffer subsystem | 1 (helpers + struct fields) |
| Production call sites updated | 3 + daemon |
| Public API additions | 3 fields on context, 0 on report (reused director_echo flag) |
| New unit tests | 10 |
| New integration tests | 2 |
| Test robustness fix | 1 (route_populates_global_log) |
| Total Response Guard combined suite | 77 (was 65; +12 = matches) |
| Total dev suite | 10334 (was 10322; +12 = matches) |
| Lines added (production) | ~169 |
| Lines added (tests) | ~410 |
| Sprint duration | < 2 hours |
| Behavioral regression | 0 (cold-start preserves legacy path; ASan clean) |
