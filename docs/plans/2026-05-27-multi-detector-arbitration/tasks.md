# Multi-Detector Arbitration with Uncertainty — Tasks

## Status: BLOCKED on production data

Per requirements.md, this spec is gated on:
1. ≥1000 DPO rejections across ≥3 channels in
   `~/.human/training-data/m3-dpo-rejections-*.jsonl`.
2. Per-detector retry-outcome telemetry showing non-uniform rescue
   rates (currently we have G9 telemetry only; **prerequisite task
   T0** below adds the rest).
3. A real operator issue about per-channel disable being too
   coarse-grained.

DO NOT start T1+ until all three gates are met. If they aren't met
in 90 days, sunset this spec — the cost of writing it is paid; the
cost of implementing speculatively is the failure mode.

## Prerequisite (can ship before gate clears)

### T0 — Per-detector retry-outcome counters

Currently `hu_guard_reject_stats_t` has `g9_retry_{rescued,thrashed,
starved}` counters (Sprint 41 #3). Generalize to per-detector:
```c
uint64_t retry_rescued[HU_GUARD_DETECTOR_COUNT];
uint64_t retry_thrashed[HU_GUARD_DETECTOR_COUNT];
uint64_t retry_starved[HU_GUARD_DETECTOR_COUNT];
```

The agent_turn / agent_stream retry sites already check WHICH
detector flag was set; they just record into the G9 slot today
because that's the only one with retry wired. Extend the recorder
+ the wire sites.

Files:
- `include/human/agent/response_guard.h` — extend struct.
- `src/agent/response_guard.c` — extend the recorder.
- `src/agent/agent_turn.c`, `src/agent/agent_stream.c` (2 sites) —
  record the SPECIFIC detector index, not the G9-only slot.
- `tests/test_response_guard.c` — extend the existing rescued/
  thrashed/starved tests to cover other detectors.

Estimate: ~0.5 day. Independent of the rest of this spec — ships
the per-detector telemetry, which is the data this spec eventually
needs.

## Implementation tasks (gated)

### T1 — Arbiter struct + NULL-safe call site

Add `hu_response_guard_arbiter_t` (header + impl). Wire into
`response_guard_check_ex` after the existing G1-G9 phases with a
NULL check — `NULL arbiter → existing behavior unchanged`.

Test: pin that uniform-1.0 weights produce byte-identical outputs
to today's pipeline (AC-7).

Estimate: ~1 day.

### T2 — Calibration script

`scripts/compute-detector-weights.py` — reads DPO logs + retry-outcome
snapshots, computes per-channel × per-detector precision, writes the
weights JSON.

Test: fixture corpus with known labels → expected weights output.

Estimate: ~1 day.

### T3 — Config override for weights

Add `response_guard.detector_weight_overrides` parser + apply at
startup, AFTER the auto-computed defaults.

Test: override-wins-over-default semantics.

Estimate: ~0.5 day.

### T4 — REVISE verdict + repair retry

Add `HU_GUARD_REVISE = 3` enum + repair-prompt retry + re-arbiter
loop.

Test: REVISE → retry → re-arbiter flow, with all three terminal
states (OK after retry, REJECT after retry, still REVISE).

Estimate: ~1 day.

### T5 — Heuristic router

`should_run_detector(detector, msg, msg_len)` — initial allowlist-
style heuristic per design.md.

Test: short drafts skip G1-G4; long drafts run all; always-on
detectors always run.

Estimate: ~0.5 day.

### T6 — Production calibration + 1-week A/B

Run T2's calibration script on real production data. Compare
unified-dispatch + arbiter outcomes to unified-dispatch + raw G1-G9
on a held-out tap (real proactive sends, half through each path,
A/B comparison via the existing retry-outcome telemetry).

Estimate: 1 week calendar.

### T7 — Default-on + delete arbiter NULL path

Once T6 shows the arbiter is at least as good as raw G1-G9
(rescue rate within 5% AND no new failure modes), default the
arbiter to ON. T7b: delete the NULL-arbiter fallback path.

Estimate: ~0.5 day (gated on T6).

## Totals

- T0 (prerequisite): ~0.5 day.
- T1-T5 (implementation): ~4 days.
- T6 (calendar): 1 week.
- T7+T7b: ~0.5 day.
- **Total engineering: ~5 days + 1 week calendar.**
- **Total tests: ~25 new tests.**

## Open commitment

This spec ships ONLY when the production-data gates clear. If they
don't clear in 90 days, sunset:

```
mv docs/plans/2026-05-27-multi-detector-arbitration \
   docs/plans/.archive/2026-05-27-multi-detector-arbitration-sunset
```

Sunset is preferable to letting the spec rot — future engineers
shouldn't have to wonder if this is in-flight. Either we shipped it
or we decided not to.
