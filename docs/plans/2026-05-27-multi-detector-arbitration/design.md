# Multi-Detector Arbitration with Uncertainty — Design

## TL;DR

Replace `response_guard_check_ex`'s hardcoded `if (detected_X) REJECT`
chain with a weighted ensemble that consults per-channel × per-detector
accuracy weights. Add a "borderline" REVISE verdict + single retry.
Drive weights from a nightly calibration job that reads the existing
Sprint 41 #3 DPO logs and retry-outcome counters.

```
BEFORE:                       AFTER:
detect G1 → if hit, REJECT    score = Σ (detector_vote × weight[chan][det])
detect G2 → if hit, REJECT    if score ≥ 0.7 → REJECT
detect G3 → if hit, REJECT    elif score ≥ 0.3 → REVISE (1 retry, re-arbiter)
...                           else → OK
detect G9 → if hit, REJECT
```

## Architecture

### `hu_response_guard_arbiter_t`

New type held by the daemon, populated at startup from the nightly
job's output JSON. Fields:

```c
typedef struct hu_response_guard_detector_weight {
    const char *channel;      // e.g. "imessage", "voice"
    const char *detector;     // e.g. "naked_discourse_opener"
    double weight;            // [0.0, 1.0]; default 1.0
    /* Calibration metadata for operator inspection: */
    double precision;         // % of REJECTs that were correct
    double recall;            // % of true-bad drafts caught
    uint64_t sample_count;    // n behind this weight
} hu_response_guard_detector_weight_t;

typedef struct hu_response_guard_arbiter {
    hu_response_guard_detector_weight_t *weights;
    size_t weights_len;
    double reject_threshold;  // default 0.7
    double revise_threshold;  // default 0.3
    /* Operator-set overrides, applied AFTER computed defaults: */
    hu_response_guard_detector_weight_t *overrides;
    size_t overrides_len;
} hu_response_guard_arbiter_t;
```

### Ensemble math

For each draft + channel pair, compute:

```
score = sum(detector_vote[d] × weight(channel, d)) / sum(weight(channel, d))
```

Where:
- `detector_vote[d]` ∈ {0, 1} — does detector d say "bad"?
- `weight(channel, d)` ∈ [0, 1] — historical precision of d on channel.
- Sum normalizes to [0, 1].

Threshold mapping:
- `score ≥ reject_threshold` (default 0.7) → REJECT
- `revise_threshold ≤ score < reject_threshold` → REVISE
- `score < revise_threshold` → OK

When the arbiter is NULL (default), the old `if (any_detector) REJECT`
behavior runs. AC-7 backwards-compat guaranteed by this NULL check.

### REVISE verdict

A new `HU_GUARD_REVISE = 3` enum value. Triggers a single
repair-prompt retry (NOT G9's retry — different repair instruction).
After the retry, re-run the arbiter:
- If still REVISE → emit with `log warn "borderline draft sent
  after revise"`. Don't ship to canned fallback (that would be too
  aggressive for borderline cases).
- If now OK → emit cleaned text.
- If now REJECT → behave like REJECT (DPO capture + skip).

### Learned router (heuristic v0)

Before running detectors, decide which to run. Initial heuristic:

```c
bool should_run_detector(const char *detector, const char *msg, size_t msg_len) {
    // Always run high-stakes, low-cost:
    if (str_eq(detector, "naked_discourse_opener")) return true;
    if (str_eq(detector, "persona_pii_echo")) return true;
    if (str_eq(detector, "persona_identity_echo")) return true;

    // CoT/leak detectors are expensive AND only fire on long drafts:
    if (msg_len < 200 && str_starts_with(detector, "g1_")) return false;
    if (msg_len < 200 && str_starts_with(detector, "g2_")) return false;

    return true;  // default-on; allowlist not blocklist
}
```

A real learned router (tiny MLP from offline labeled data) is a
follow-up sprint.

### Calibration job (Python)

`scripts/compute-detector-weights.py`:

1. Read DPO logs from `~/.human/training-data/m3-dpo-rejections-*.jsonl`.
2. Read retry-outcome counter snapshots (operator periodically dumps
   `hu_guard_reject_stats_snapshot()` to a JSON file — new tiny
   feature: doctor check could emit this side-effect daily).
3. For each (channel, detector) pair:
   - precision = (rejected AND verified-bad) / (rejected)
     where "verified-bad" comes from the retry-outcome telemetry:
     if retry produced clean text → original was bad → original
     rejection was correct.
   - recall is harder — requires labeled true-bad samples that
     weren't caught. Punt v0: use precision as the weight directly.
4. Write `~/.human/training-data/detector-weights.json`.
5. Doctor check reports weights age (warn if > 7 days stale).

## Open questions

These need either operator input or production data before
implementation:

1. **Should REVISE retry use the same propose-or-skip prompt or a
   separate repair prompt?** The G9 retry uses an
   identity-anchored repair prompt; REVISE may want something gentler
   ("you generated this — does it match the persona?"). Needs A/B.
2. **What's the right `reject_threshold` default?** 0.7 is a guess;
   production data will tell. The CONFIG OVERRIDE for the threshold
   is in AC-6; the DEFAULT is the question.
3. **Does the heuristic router's 200-char cutoff hold across
   channels?** Voice messages might be longer; iMessage drafts
   shorter. May need per-channel cutoffs.
4. **How does this interact with `g9_disabled_channels`?** Two answers
   possible:
   - "Disable list wins" (explicit operator action overrides
     calibration). Simple.
   - "Disable list is just `weight = 0`" (unified surface).
     Conceptually cleaner.
   Per requirements.md, this spec says "disable list wins" for now;
   future cleanup can unify.

## Backwards compatibility

The arbiter is OPT-IN. When `agent->guard_arbiter == NULL`,
`response_guard_check_ex` runs the existing G1-G9 chain unchanged.
The new code path activates ONLY when:
- Daemon loaded `detector-weights.json` successfully at startup, OR
- Operator set `response_guard.use_arbiter: true` in config.

This is the same "feature flag during rollout" pattern as
`use_unified_dispatch` (T3) — but learned-from: this one will be
DELETED in its own T8b commit, not left vestigial.

## Why this spec might never ship

The requirements.md pre-condition gate (≥7 days data, non-uniform
rescue rates across detectors, operator demand) might never be met:
- If rescue rates ARE uniform across detectors → the ensemble adds
  zero value over the existing chain. Don't ship.
- If operator demand never materializes → there's no real
  use case. Don't ship.
- If the per-channel disable list (Sprint 41 #4) covers all
  observed operator needs → don't ship.

This spec exists to *scope* the work in case those pre-conditions are
met, NOT as a commitment to implement.
