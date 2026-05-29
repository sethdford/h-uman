---
title: Outbound Pipeline Stats — operator runbook
updated: 2026-05-26
---

# Outbound Pipeline Stats — operator runbook

Per-stage × per-verdict counters for the outbound safety pipeline,
surfaced via the `outbound_stats` doctor check. Sprint 60 follow-up
that closes item #5 of `docs/plans/2026-05-26-sprint-59-outbound-safety/STATUS.md`.

**Cross-references:**
[observability.md](observability.md),
[monitoring.md](monitoring.md),
[../security/data-privacy.md](../security/data-privacy.md)

---

## What this measures

The outbound pipeline (`src/agent/outbound/pipeline.c`) runs every
LLM-generated outbound message through an ordered list of stages —
`strip`, `shape`, `echo`, `crosstalk`, `persona`, `moderation`. Each
stage returns one of four verdicts: SEND, REWRITE, REGENERATE, REJECT.

`outbound_stats` keeps a process-wide atomic counter for every
(stage × verdict) pair. After any pipeline run, the counter for the
(stage that fired, verdict it returned) bumps by 1. Counters are
cumulative across the daemon's lifetime — no per-tick rollover.

## How to read the output

```
$ human doctor --json | jq '.checks[] | select(.name=="outbound_stats")'
```

Shape (truncated for brevity):

```json
{
  "name": "outbound_stats",
  "verdict": "pass",
  "reason": "outbound pipeline stats snapshot",
  "detail": {
    "stages": [
      {"name": "strip",      "send": 1247, "rewrite": 3, "regenerate": 0, "reject": 0},
      {"name": "shape",      "send": 1244, "rewrite": 0, "regenerate": 1, "reject": 2},
      {"name": "echo",       "send": 1241, "rewrite": 0, "regenerate": 0, "reject": 3},
      {"name": "crosstalk",  "send": 1240, "rewrite": 0, "regenerate": 0, "reject": 1},
      {"name": "persona",    "send": 1234, "rewrite": 0, "regenerate": 6, "reject": 0},
      {"name": "moderation", "send": 1234, "rewrite": 0, "regenerate": 0, "reject": 0},
      {"name": "other",      "send": 0,    "rewrite": 0, "regenerate": 0, "reject": 0}
    ],
    "total_send": 1234,
    "total_rewrite": 3,
    "total_regenerate": 7,
    "total_reject": 6,
    "reject_rate": 0.00,
    "healthy": true,
    "warnings": []
  }
}
```

### Key fields

| Field | Type | Meaning |
|---|---|---|
| `verdict` | string | Always `"pass"` — this check is informational, not a gate |
| `reason` | string | Static human-readable label |
| `detail.stages[]` | array | Per-stage counters in stable order (strip → moderation → other) |
| `detail.total_*` | uint64 | Sums across all stages — useful for ratio math |
| `detail.reject_rate` | float | `total_reject / (sum of all verdicts)`; 0.00 when no traffic |
| `detail.healthy` | bool | `true` unless a warning fires (see below) |
| `detail.warnings[]` | string array | Stable identifier strings for detected issues |

### Stage order

Stages fire in the order shown above for the `proactive` path. Other
paths skip some stages by design (e.g., `reactive` runs only
`strip + crosstalk`). For per-path configuration see
`src/agent/outbound/pipeline_configs.c`.

## Health interpretation

### `detail.healthy: true` + empty `warnings[]`

Steady state. The pipeline is running; rejection rate is below the
25% threshold OR the sample size is below 100 (too early to say).
**No action.**

### `detail.warnings` contains `"reject_rate_high"`

Triggered when `reject_rate > 0.25` **AND** sample size is at least
100 records. Means more than a quarter of outbound messages over the
daemon's lifetime have been rejected by some stage.

**Diagnose:**
1. Find the highest-count `reject` cell across stages. That's the
   stage doing the rejecting.
2. Cross-check the daemon's structured log for `[outbound]
   stage=<X> verdict=3 reason=<R>` lines. The `reason` field
   identifies the specific failure mode.
3. Common patterns:
   - `crosstalk` rejecting → likely a contact-corpus issue (stale
     messages, bad SQLite lookup state). Check
     `hu_outbound_crosstalk_register_sqlite` was called at daemon
     startup; see [data-privacy.md](../security/data-privacy.md).
   - `persona` rejecting → shape-classifier mismatch; the LLM is
     consistently producing out-of-voice content. Check persona
     overlay configs (`~/.human/personas/`) for stale rules.
   - `shape` rejecting → length/format issues; LLM output too long
     or carrying markdown.

**Action:** If the high reject rate is intentional (adversarial test
traffic, aggressive new rules just deployed), the warning is
informational only. If unexpected, raise an incident per
[incident-response.md](incident-response.md).

### `detail.warnings` contains `"unknown_stage_counts"`

Triggered when `detail.stages[].name == "other"` has any nonzero
count. Means the pipeline emitted a stage name that the stats
subsystem's name table doesn't recognize.

**Almost always:** a new stage was added to `pipeline_configs.c`
without a corresponding entry in `src/agent/outbound/stats.c::
s_stage_table`. The new stage's verdicts route to the OTHER bucket
silently, so per-stage dashboards miss its activity.

**Action:**
1. Find the new stage by grepping `src/agent/outbound/*.c` for
   `hu_outbound_pipeline_stage_t hu_outbound_stage_X = {.name = "X", ...}`.
2. Add a matching entry to `s_stage_table` AND extend the
   `hu_outbound_stats_stage_t` enum (in `include/human/agent/outbound_stats.h`).
3. Add a test case to `tests/test_outbound_stats.c` to pin the new
   mapping.

## Common operational patterns

### "All zeros across the board"

The pipeline ran but recorded nothing — likely the daemon hasn't
processed any outbound traffic yet (fresh restart) OR the wiring at
`src/agent/outbound/pipeline.c::hu_outbound_stats_record` was dropped.

Diagnose with `tests/test_outbound_stats_e2e.c`: the
`test_clean_run_increments_every_stage_send_counter` and
`test_bleed_run_increments_crosstalk_reject_counter` tests
explicitly pin the wiring contract. If those tests pass but
production shows zeros, traffic genuinely hasn't flowed.

### "Crosstalk counters move but stage:'other' has counts"

Indicates the crosstalk stage's lookup is returning hits (good) but
some OTHER unknown stage is firing (drift). See
`unknown_stage_counts` warning above.

### "Persona regenerate count climbs steadily"

The persona stage is asking the LLM to retry. Some retry is healthy
(catches drift); >5% regenerate rate persistently suggests the
underlying provider model is producing consistently-off-voice output
and the persona shape classifier is correctly catching it.

**Action:** Consider switching providers or re-running persona
fine-tuning (see the M3 fidelity eval harness,
`scripts/eval_fidelity_nightly.py`).

## Counter lifecycle

- Counters are atomic `uint_least64_t`. Increment is sub-microsecond.
- Reset only on daemon restart. No tick rollover, no per-window reset.
- For RATE calculations, diff two snapshots over a time window.

## Limits / known constraints

- Stage count is bounded by `HU_OUTBOUND_STATS_STAGE_COUNT = 7` (six
  named stages + OTHER bucket). Adding a new stage requires editing
  the enum AND the name table; the test suite enforces both.
- Sample-size floor (100 records) suppresses the
  `reject_rate_high` warning during fresh-deploy phases. See
  [`docs/plans/adr/2026-05-26-outbound-stats-health-thresholds.md`](../../plans/adr/2026-05-26-outbound-stats-health-thresholds.md)
  for the rationale.
