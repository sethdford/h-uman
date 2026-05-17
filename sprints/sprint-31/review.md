# Sprint 31 — review

## Goal

Add the two context-aware leak detections deferred from Sprint 29
(length anomaly + director-string echo) via a new opt-in `_ex` API
that lets callers pass per-turn context the guard couldn't access
under the original signature.

## API shipped

`include/human/agent/response_guard.h`:

- `hu_guard_context_t` struct with `recent_avg_len`,
  `director_text`, `director_len` fields.
- `hu_response_guard_check_ex()` accepting an optional `ctx`
  pointer. NULL ctx is byte-identical to legacy behavior.
- `hu_response_guard_check()` becomes a 3-line wrapper that calls
  `_ex` with `ctx=NULL`.
- 2 new bools on `hu_guard_report_t`: `detected_length_anomaly`,
  `detected_director_echo`.

## Detections

| Phase | Class | Trigger | Threshold |
| --- | --- | --- | --- |
| 4a | G5 length anomaly | `response_len > avg * MULT` | `MULT = 8` |
| 4b | G6 director echo | 30+ char substring of director_text appears verbatim in response | `MIN_MATCH = 30` chars |

Both run on the cleaned (post-Phase-1-strip) text. Length anomaly
uses the original `response_len` because what matters for the avg
comparison is "what gets sent on the wire."

## Tests added (6)

| Test | Asserts |
| --- | --- |
| `guard_ex_rejects_length_anomaly` | 22x avg → REJECT, length-anomaly flag set, director-echo flag NOT set, out=NULL |
| `guard_ex_rejects_director_echo` | 50-char verbatim director quote → REJECT, director-echo flag set |
| `guard_ex_passes_long_response_when_no_avg` | `recent_avg_len=0` disables G5; long benign reply → OK |
| `guard_ex_passes_legit_5x_response` | 4.5x is below 8x threshold; reply → OK |
| `guard_ex_passes_short_director_text` | `director_len < 30` disables G6; reply → OK |
| `guard_ex_with_null_ctx_matches_legacy_behavior` | NULL ctx → byte-identical to `hu_response_guard_check` |

Helper added: `guard_test_make_benign_long(out, cap, target_len)`
builds a long response with varied content that doesn't trip
Phase 0/1/2/3 detectors. First draft used `aaaa aaaa aaaa` which
hit Phase 2 (degenerate token run) before reaching Phase 4 — the
benign-text helper isolates Phase 4 cleanly.

## Files touched

| File | Change |
| --- | --- |
| `include/human/agent/response_guard.h` | +`hu_guard_context_t` struct, +`hu_response_guard_check_ex` decl, +2 report bools, +1 doc paragraph |
| `src/agent/response_guard.c` | +2 thresholds, +`hu_guard_has_length_anomaly`, +`hu_guard_has_director_echo`, +Phase 4 block, refactored `hu_response_guard_check` to wrapper |
| `tests/test_response_guard.c` | +6 tests + benign-long helper |
| `sprints/sprint-31/{stories,review,retro}.md` | New |

## Verification

| Build | Result |
| --- | --- |
| `cmake --preset dev` | **10296 / 10296 passed**, 0 ASan errors |
| `cmake --preset minimal` | **8836 / 8836 passed** |

Response Guard suite: **39 / 39** (was 33 at v-sprint-30-close,
+6 from Sprint 31).

## Out of scope (deferred)

- **Wiring `_ex` into production callers** (`agent_stream.c`,
  `agent_turn.c`, `daemon.c`). User's WIP touches these; would
  conflict. Sprint 33 will pick this up after WIP resolves.
- **Quality gate `MARGINAL → REJECT` policy** (Sprint 32).
- **Audit script as a recurring tool** (next).
- **Post-mortem doc** (after Sprint 32).
- **Lower length-anomaly threshold per channel.** Different
  channels have different baseline lengths (iMessage tends short,
  email tolerates long). Future enhancement.

## Operational follow-up

1. Cherry-pick / `git am` Sprint 31 onto main `rl-sota-phase-5`.
2. Once user's WIP merges to main, a follow-on commit should:
   - In `src/agent/agent_stream.c` (or wherever the daemon calls
     the guard), compute `recent_avg_len` from the recipient's
     last N inbound messages and pass it via `_ex`.
   - Pass `director_text` from the same prompt-template fragment
     that drove the turn.
   - Both wires immediately enable G5 + G6 in production.
