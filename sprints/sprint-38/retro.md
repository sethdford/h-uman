# Sprint 38 — Retrospective

## What went well

- **Three concerns, one sprint, no scope creep.** Global-state hygiene,
  G8 biography, and telemetry are independent but small — shipped together
  without touching quality-gate or per-channel thresholds (deferred).
- **Biography closes the "long backstory leak" class.** Identity/core_anchor
  are short; biography is often 200–500 chars of uniquely identifying
  narrative. A model quoting `"grew up in Utah, studied computer science
  at BYU"` with no name and no identity overlap now trips G8.
- **Telemetry is minimal and testable.** Five atomic counters, snapshot +
  reset API. No observer wiring yet — that's intentional (measure first,
  wire to metrics observer in a follow-up if hit rates justify it).
- **Global-state contract is now explicit in tests.** Two tests reset the
  route log before asserting; one documents the reset→zero contract.

## What didn't go well

- **Telemetry only records Phase 3 + Phase 4 REJECTs.** Degenerate-repetition
  and empty-strip REJECTs don't increment counters (no flags in report).
  Acceptable for G5–G8 measurement goals; document if we expand later.
- **Biography and identity can double-count in stats.** One REJECT with both
  sources matching still increments `persona_identity_echo` once (one
  report flag). Correct for ops ("G8 fired") but not per-source breakdown.
- **No daemon/metrics observer wire yet.** Counters are in-process only;
  operators must call snapshot from debug tooling or a future admin RPC.

## Action items for Sprint 39+

1. Wire `hu_guard_reject_stats_snapshot` into gateway admin or periodic
   daemon log (e.g. every 1000 turns).
2. Quality gate MARGINAL→REJECT — now that telemetry exists, compare
   guard REJECT rate vs reflection MARGINAL rate.
3. Per-channel G5 thresholds.
4. Widen G7 lookahead 30→60 bytes after measuring G7 false positives.
