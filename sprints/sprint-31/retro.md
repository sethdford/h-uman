# Sprint 31 — retro

## What went well

- **Opt-in `_ex` API was the right design.** Two callsites for
  `hu_response_guard_check` exist in the user's WIP territory
  (`agent_stream.c`, `daemon.c`). Breaking the existing function
  signature would have forced me to touch those files, which
  would conflict with the user's uncommitted modifications.
  Adding `_ex` alongside the legacy function ships the new
  detections + tests safely; production wiring is a follow-on
  the user controls.

- **Phase 4 placement is consistent with Phase 3.** Same shape:
  detection helper returns bool, the function frees `cleaned`
  if needed, sets outcome to REJECT, populates report flags.
  Future detectors (Phase 5, etc.) can follow the same template.

- **Threshold calibration is grounded in the production data.**
  - 8x length threshold catches the 22x leak with margin while
    leaving legit 3-5x replies through.
  - 30-char director-echo window is shorter than the leak's
    64-char quote (so we catch it) and longer than incidental
    common phrases.
  - Both thresholds are named constants so future tuning is
    obvious.

- **Negative tests pin behavior.** `recent_avg_len=0` disables
  G5; `director_len < MIN_MATCH` disables G6; `ctx=NULL` makes
  the function behave identically to the legacy entry point.
  Each is a separate test.

## What was hard

- **Test fixtures kept tripping Phase 2 by accident.** First
  draft used `memset(raw, 'a', N)` to build a long response.
  That's a 4-char repeated token (`aaaa aaaa aaaa`) which trips
  the degenerate-token-run detector before reaching Phase 4.
  Built `guard_test_make_benign_long()` that loops a varied
  English sentence, padded to exact target_len with periodic
  punctuation. This isolates Phase 4 cleanly. Lesson: when
  testing a specific phase, ensure the fixture doesn't trip
  upstream phases — otherwise the test passes/fails for the
  wrong reason.

- **`-Wunsequenced` warning on `out[i++] = (i % 10 == 0) ?
  '.' : ' '`.** The `i++` modification and `i % 10` access in
  the same expression is undefined per C11. Fixed by hoisting
  to a temporary, then incrementing afterwards. Caught by
  `-Wall -Wextra -Wpedantic -Werror`. The kind of bug I'd never
  notice at runtime because `i` was already updated by the
  time the `?` evaluated, but the standard makes no such
  promise.

- **Director-echo sliding window has O(director_len *
  response_len) cost.** For director ~100 chars and response
  ~1KB, that's ~100K char comparisons inside `ci_contains`.
  Each comparison is up to 30 chars. Total ~3M comparisons.
  Fine on modern hardware (<1ms) but worth noting if either
  size grows by an order of magnitude. A future optimization
  could use a rolling hash or substring index — but YAGNI for
  now.

## What surprised us

- **The legacy `hu_response_guard_check` behavior was already
  identical to `_ex(ctx=NULL)`** — no code path needed
  reorganization. The wrapper is literally a one-line forward.
  This validates the design: context-aware detections are a
  pure addition, not a refactor.

- **The director-echo detector caught the leak's 64-char quote
  in a single 30-char window match.** The window slides at
  1-char granularity, so the leak's quote `Professional,
  slightly skeptical, ask for clarification on why they`
  (which contains 35+ chars matching director text) trips
  on multiple windows simultaneously. First-match-wins exits
  early; net cost is bounded.

## New carry-overs

- **Wire `_ex` into production callers** (Sprint 33 candidate).
  `agent_stream.c` / `agent_turn.c` / `daemon.c` need to:
  1. Compute `recent_avg_len` from the recipient's last N
     inbound messages (`COALESCE(AVG(LENGTH(text)), 0)` from
     chat.db or equivalent).
  2. Pass the `director_text` from the prompt template
     fragment.
  3. Switch the call site from `hu_response_guard_check` to
     `hu_response_guard_check_ex`.
  The user's WIP currently modifies these files; do this after
  WIP resolves to avoid merge conflict.

- **Per-channel length thresholds.** Different channels have
  different baseline reply lengths (iMessage ~30 chars, email
  ~200 chars, Slack ~50 chars). A single 8x multiplier across
  all channels is OK as a first cut but probably 12x for email
  is more accurate. Future enhancement.

- **Director-echo over multi-turn.** Currently we only check
  the CURRENT turn's director. A model could echo a director
  string from a previous turn. Worth tracking the last K
  director strings and checking against all.

- **Length anomaly with smoothing.** A single 22x outlier
  reply gets caught. But a sequence of 3x replies (each just
  below 8x) wouldn't. Worth tracking a longer rolling window
  and flagging "consistently above avg" too.

## Process notes

- **Three-sprint cadence (S29 → S30 → S31) was right.** Each
  sprint shipped one independent leak class with its own
  regression tests. Trying to ship all three in a single
  sprint would have meant 18-test diff and a 600-line code
  change. Smaller sprints = faster review = faster cherry-pick
  to main.

- **Audit-driven sprint scoping is gold.** Sprint 30 only
  exists because Sprint 29's audit found leaks Sprint 29 missed.
  Sprint 31 only exists because Sprint 29's retro identified
  the API-change-required gap. Pattern: ship → audit →
  retro → next sprint.
