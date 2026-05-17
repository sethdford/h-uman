# Sprint 31 — length-anomaly + director-string echo (G5/G6)

## Why now

Sprints 29 + 30 closed the leak shapes Sprint 29 actually caught
(CoT dump) and the audit-discovered shape (template-label dump).
Two more high-leverage detections were deferred because they
require an API change to thread additional context into the
guard:

- **G5 (length anomaly)**: response is N times longer than the
  recipient's recent rolling-average reply length. The 2026-05-12
  Brea leak was 979 chars vs a 44 char rolling average — 22x.
  The downstream quality gate flagged it MARGINAL but did not
  block.

- **G6 (director-string echo)**: the model echoed the upstream
  director's scene-direction string verbatim into its reply. The
  Brea leak quoted "Professional, slightly skeptical, ask for
  clarification on why they" verbatim — that's the director
  string from the prompt template.

Both are caught by reading the response in light of *context the
guard doesn't currently have access to*: the rolling avg length
and the director text. Sprint 31 adds that context via an opt-in
`_ex` API, so existing callers continue to work and production
callers can migrate at their own pace.

## API design

Two new symbols in `include/human/agent/response_guard.h`:

```c
typedef struct {
    /* Rolling avg reply length from the recipient (over last N
     * messages). The guard rejects if response_len exceeds
     * `recent_avg_len * HU_GUARD_LENGTH_ANOMALY_MULT` (default 8).
     * 0 disables the check (e.g. no history yet). */
    size_t recent_avg_len;

    /* The director's scene-direction text for this turn. The guard
     * rejects if a 30+ char substring of director_text appears
     * verbatim in the response. NULL/0 disables the check. */
    const char *director_text;
    size_t director_len;
} hu_guard_context_t;

hu_error_t hu_response_guard_check_ex(
    hu_allocator_t *alloc,
    const char *response, size_t response_len,
    const hu_guard_context_t *ctx,           /* may be NULL */
    char **out_response, size_t *out_len,
    hu_guard_outcome_t *out_outcome,
    hu_guard_report_t *report);
```

The existing `hu_response_guard_check()` becomes a one-line
wrapper that calls `_ex` with `ctx=NULL`. Zero behavior change
for existing callers.

## Stories

### T1. Add `hu_guard_context_t` struct + `_ex` API + threshold constants

- New struct in the header.
- New `_ex` function with same signature as existing plus the
  ctx pointer.
- Existing `hu_response_guard_check` becomes a wrapper.
- Two constants: `HU_GUARD_LENGTH_ANOMALY_MULT = 8` and
  `HU_GUARD_DIRECTOR_ECHO_MIN_MATCH = 30`.

### T2. Length-anomaly detection (Phase 4a)

- If `ctx && ctx->recent_avg_len > 0 &&
   response_len > ctx->recent_avg_len * HU_GUARD_LENGTH_ANOMALY_MULT`,
  REJECT with `report->detected_length_anomaly = true`.
- Calibration: 22x is the leak; 5x is plausible legit (a long
  story reply); 8x is the safe midpoint.

### T3. Director-string echo detection (Phase 4b)

- If `ctx && ctx->director_text && ctx->director_len >=
   HU_GUARD_DIRECTOR_ECHO_MIN_MATCH`, scan the response for a
  verbatim match of any 30-char window from director_text. If
  found, REJECT with `report->detected_director_echo = true`.
- Why a sliding window: the director string may be long; the
  model may quote a fragment. 30 chars is shorter than the leak's
  64-char quote, longer than incidental phrase matches.
- Implementation: walk director_text in 30-char windows; for each
  window, do `hu_guard_ci_contains(response, len, window, 30)`.
  First match wins. O(director_len * response_len), fine for
  realistic sizes.

### T4. Tests

5 new tests in `tests/test_response_guard.c`:

1. `guard_ex_rejects_length_anomaly` — `recent_avg_len=44`,
   response 979 chars (22x), no other leak signals → REJECT.
2. `guard_ex_rejects_director_echo` — `director_text="Professional,
   slightly skeptical, ask for clarification on why they are
   sending it again"`, response contains a 50-char substring of
   it → REJECT.
3. `guard_ex_passes_long_response_when_no_avg` — `recent_avg_len=0`,
   response 2000 chars → OK (no anomaly check).
4. `guard_ex_passes_legit_5x_response` — `recent_avg_len=44`,
   response 200 chars (4.5x), no leak signals → OK.
5. `guard_ex_passes_short_director_text` — `director_text="be
   nice"` (9 chars, below MIN_MATCH), response contains "be nice"
   → OK.

## Files touched

| File | Change |
| --- | --- |
| `include/human/agent/response_guard.h` | +`hu_guard_context_t` struct, +`hu_response_guard_check_ex` decl, +2 report bools (`detected_length_anomaly`, `detected_director_echo`), +1 doc paragraph on the `_ex` API. |
| `src/agent/response_guard.c` | +2 thresholds, +Phase 4a/4b in a refactored `_ex` impl, existing `hu_response_guard_check` becomes 3-line wrapper. |
| `tests/test_response_guard.c` | +5 tests. |
| `sprints/sprint-31/{stories,review,retro}.md` | New. |

## Definition of Done

- All 5 new tests pass.
- No regression on the existing 33 response-guard tests.
- Existing `hu_response_guard_check` callers are byte-identical
  in behavior (ctx=NULL path).
- Dev: 10295 / 10295 passed.
- Minimal: 8835 / 8835 passed.
- Cherry-picked / `git am`'d to main.
- `git tag v-sprint-31-close`.

## Out of scope (deferred)

- **Wiring `_ex` into production callers** (`agent_stream.c`,
  `agent_turn.c`, `daemon.c`). The user's WIP currently modifies
  these files; touching them risks merge conflict. Sprint 33
  should pick this up after WIP resolves.
- **Quality gate `MARGINAL → REJECT` policy** (Sprint 32).
- **Audit script as a recurring tool** (separate sprint).
- **Post-mortem doc** (after Sprint 32).
