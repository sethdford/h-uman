---
title: "ADR — Outbound stats health thresholds (reject rate + sample size)"
created: 2026-05-26
status: accepted
deciders: engineering
parent: ../2026-05-26-sprint-59-outbound-safety/STATUS.md
related:
  - ../../standards/operations/outbound-pipeline-stats.md
  - ../../standards/operations/observability.md
---

# ADR: Outbound stats health thresholds

## Context

Sprint 60 added the `outbound_stats` doctor check
(`src/doctor/check_outbound_stats.c`) which exposes per-stage ×
per-verdict counters from the outbound safety pipeline. The initial
ship was informational-only: every snapshot returned PASS with no
derived health signal. Operators had to compute their own "is this
pipeline healthy?" judgment from raw counters.

The Sprint 60 follow-up (commit `20ee7084`) added three derived
fields to `detail_json`:

- `reject_rate` — total_reject / sum-of-all-verdicts (float)
- `healthy` — bool, true unless a warning fires
- `warnings` — array of stable identifier strings

Two thresholds shape when the `"reject_rate_high"` warning fires:

| Constant | Default | Defined in |
|---|---|---|
| `HU_DOCTOR_OUTBOUND_STATS_REJECT_RATE_THRESHOLD` | `0.25` | `src/doctor/check_outbound_stats.c` |
| `HU_DOCTOR_OUTBOUND_STATS_MIN_SAMPLE` | `100u` | same |

This ADR documents WHY these specific numbers, so the next person
tuning them has the original argument.

## Decision

**Reject rate: 25%.**

**Sample minimum: 100 cumulative records (across all verdicts and stages).**

## Rationale

### Why 25% reject rate (and not 10% or 50%)

We need a threshold that:

1. **Is high enough** that legitimate defensive rejection — a
   small number of LLM hallucinations correctly caught — doesn't
   trip the warning.
2. **Is low enough** that a real degradation (e.g., a provider
   model regression that starts producing 40% off-voice content)
   surfaces immediately.

Sprint 59's incident corpus (`docs/plans/2026-05-26-sprint-59-outbound-safety/incident-corpus.md`)
contains 24 production-incident rows. 16 of those are REJECT cases
(`#1-16`). The other 8 are BORDERLINE (REGENERATE, #17-18) or PASS
(SEND, #19-24). If the corpus were the entire traffic mix, reject
rate would be 16/24 = **66.7%** — wildly above 25%.

In production, the corpus is a sampled WORST-CASE distribution, not
the typical mix. Field expectation:

| Scenario | Expected reject rate |
|---|---|
| Healthy production traffic | 0.5% – 3% |
| New rule rollout (stricter) | 5% – 15% transiently |
| Provider regression (off-voice spike) | 30% – 60% |
| Adversarial corpus replay (testing) | 60% – 80% |

`0.25` sits cleanly between "new rule rollout transient" (worst
healthy case) and "provider regression" (lowest unhealthy case),
with comfortable margin on both sides:

- A new rule that pushes reject rate to 14% does not trip the
  warning during rollout.
- A provider regression that takes reject rate to 31% trips the
  warning immediately.

### Why MIN_SAMPLE = 100 (and not 10 or 1000)

The sample minimum is the LOAD-BEARING design choice. Without it,
the math is broken at small N:

| Sample N | One reject | reject_rate | Without floor |
|---|---|---|---|
| 1 | 1 | 1.00 (100%) | warns immediately on first send |
| 5 | 1 | 0.20 | doesn't warn (below threshold) |
| 5 | 5 | 1.00 | warns on adversarial test |
| 100 | 26 | 0.26 | warns (sustained pattern) |

A fresh deploy that processes one adversarial test send before any
real traffic would trip a reject_rate=1.00 warning permanently
without the floor. Operators would either:
- Habituate to the warning (dangerous — real alerts get ignored)
- Disable the check (worse — observability gap)
- File a "false positive" bug each time

100 records gives statistical meaning to the percentage — at that
sample size, a sustained 25%+ rate IS a real signal, not a quirk
of fresh-deploy ordering. Below 100, we silently treat reject_rate
as not-yet-actionable.

100 specifically (vs 1000) reflects:
- Single-user daemon hits ~100 outbounds in a day or so
- 1000 would delay the warning by ~1 week of real usage on a
  single-user deploy
- 100 still requires real traffic; 10 would be too easy to trip
  from automated tests if HU_IS_TEST sneaks in

### Why these in `#define` not config

These are diagnostic thresholds, not policy. Operators don't
typically tune them; engineering changes them deliberately when
the field data justifies. Putting them in `config.json` would:

- Spread tuning across machines (operators forget to sync)
- Create a "did you check the config?" debug step
- Risk per-machine drift in incident response

If a deploy NEEDS site-specific thresholds (e.g., a high-traffic
shared instance vs a single-user personal device), this ADR is the
trigger to revisit — at that point, the right answer is probably
two presets (`small` / `large`) hardcoded in the check, not arbitrary
operator-tunable floats.

## Consequences

### Positive

- Operator runbook (`docs/standards/operations/outbound-pipeline-stats.md`)
  can give a single concrete table mapping warnings → actions.
- Threshold changes leave a paper trail (this ADR + its history).
- Tests pin the behavior: `test_detail_json_small_sample_stays_healthy`
  is the regression guard for the floor.

### Trade-offs

- A genuinely unhealthy pipeline at <100 sample size goes unflagged
  for ~1 day of real traffic. Mitigation: structured log lines
  surface every reject in real-time; the doctor check is a
  rolled-up secondary signal, not the primary alarm.
- The 25%/100 numbers will need re-tuning once we have a longer
  window of production data on the new pipeline (Q-5 deferred this
  to 2 weeks post-deploy). When tuned, append a "Revisions" section
  to this ADR rather than overwriting.

## Notes

The thresholds live as `#define` constants at the top of
`src/doctor/check_outbound_stats.c`. To change them:

1. Edit the constants.
2. Update the affected test cases in
   `tests/test_doctor_outbound_stats.c` (specifically
   `test_detail_json_healthy_false_above_threshold` and
   `test_detail_json_small_sample_stays_healthy`).
3. Append a "Revisions" section to this ADR with the new values
   and reasoning.
4. Update the runbook
   (`docs/standards/operations/outbound-pipeline-stats.md`).
