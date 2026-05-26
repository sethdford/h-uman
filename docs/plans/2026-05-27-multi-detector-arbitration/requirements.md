# Multi-Detector Arbitration with Uncertainty — Requirements

## Why this spec exists

The `response_guard.c` pipeline currently runs G1–G9 detectors as
**ordered filters**: each detector returns OK / REWROTE / REJECT, and
any REJECT short-circuits the rest. The order is hardcoded; the
weights are implicitly equal (any single REJECT is fatal); there is
no notion of per-channel calibration or per-detector confidence.

This works while the detectors are individually high-precision and the
failure-mode coverage is roughly disjoint. **It stops working** as
soon as:

1. **A detector starts producing false positives** on a specific channel.
   Today's only escape is the per-channel disable list (Sprint 41 #4),
   which is operator-tuned and binary.
2. **Two detectors disagree on a borderline draft.** Today: whichever
   fires first wins, regardless of which has historically been right.
3. **A draft is borderline-bad — all detectors say "maybe."** Today:
   the model emits the response unchallenged.
4. **Channel-specific calibration matters.** Voice-channel registers
   differ from text — but G9 fires identically. The disable list is
   the only knob.

A multi-detector arbiter would let the system *weight* detector
outputs by **historical accuracy per channel** and use a learned
router to pick which detectors to run for which message. The Sprint
41 #3 retry-outcome telemetry (rescued/thrashed/starved per detector)
plus the DPO rejection logs are the calibration data this would
consume.

## Goals

- **G1.** Replace the hardcoded ordered-filter chain in
  `response_guard_check_ex` with a *weighted ensemble* where each
  detector's vote is multiplied by its per-channel historical
  accuracy weight (range [0, 1]).
- **G2.** Compute per-channel × per-detector accuracy from the
  Sprint 41 #3 retry-outcome counters + DPO rejection logs, updated
  daily by a nightly job.
- **G3.** Add a "borderline" verdict (REVISE / WARN) for cases where
  the weighted ensemble score lands in the 30–70% confidence band.
  Borderline drafts get a single repair-prompt retry (not skipped,
  not sent as-is).
- **G4.** Allow a learned router (small MLP or rule-based heuristic)
  to *select* which detectors to run for which message based on
  message features (length, channel, recipient relationship,
  conversation context). Avoids paying full G1–G9 cost on
  obviously-safe drafts.
- **G5.** Operator can override any per-channel × per-detector
  weight via config, with the auto-computed weight as the default.

## Non-goals

- **Replacing any individual detector's predicate.** G1–G9 stay as
  they are; this spec changes the *arbitration* layer, not the
  detection layer.
- **Cross-process arbiter state.** The accuracy weights are
  process-private (loaded at daemon startup from the nightly job's
  output JSON, like the DPO logger's path-rotation pattern).
- **Online learning during a daemon run.** Updates are batch (nightly)
  to avoid per-request training overhead.
- **Operator-facing dashboard.** That's a downstream tool that
  consumes this spec's calibration JSON; out of scope here.

## Acceptance criteria

**AC-1.** A new `hu_response_guard_arbiter_t` struct holds per-
channel × per-detector weights. Initialized from a JSON file
(`~/.human/training-data/detector-weights.json`) at daemon startup;
defaults to uniform 1.0 weights when the file is absent (backwards
compatible). Verified by: a test pinning that empty/missing weights
file → unchanged G1–G9 behavior.

**AC-2.** `response_guard_check_ex` consults the arbiter when
non-NULL, computing a weighted ensemble score in [0, 1] from each
detector's vote × its per-channel weight. Thresholds:
  - score ≥ 0.7 → REJECT
  - 0.3 ≤ score < 0.7 → REVISE (single repair-prompt retry)
  - score < 0.3 → OK
The current behavior corresponds to weights = uniform 1.0, threshold
collapsed to "any REJECT". Pinned by a test that uniform weights
produce byte-identical outputs to today's pipeline.

**AC-3.** A nightly script
(`scripts/compute-detector-weights.py`) reads the previous N days of
DPO rejection logs + retry-outcome counters, computes per-channel ×
per-detector precision/recall, writes the weights JSON. Pinned by:
a small fixture-driven unit test on a corpus with known
ground-truth labels.

**AC-4.** The "borderline" REVISE verdict triggers a single
repair-prompt retry (NOT the same as G9's retry, which fires on
REJECT). REVISE is appropriate for "this draft looks suspicious but
not clearly wrong" — give the model one chance to clean up. After
the retry, re-run the arbiter; if still borderline, emit with a log
warning (don't ship to canned-fallback). Pinned by: a test
exercising the REVISE → retry → re-arbiter flow.

**AC-5.** A learned router (initial implementation: simple
heuristic) decides which detectors to run for which message.
Heuristic v0: always run G6/G7/G8 (persona protection, low cost,
high stakes); skip G1/G2/G3/G4 (CoT-leak detectors, high cost) when
message length < 200 chars (Jordan-class is short, but CoT leaks are
long). Pinned by: a test that short drafts skip G1-G4.

**AC-6.** Per-channel × per-detector weight override via config:
```
"response_guard": {
  "detector_weight_overrides": {
    "voice": {"naked_discourse_opener": 0.0}
  }
}
```
Override wins over the nightly-computed default. Pinned by: parser
test + arbiter test.

**AC-7.** Zero regression in the existing response_guard test suite.
The default-uniform-weights path MUST produce byte-identical
outputs to today's pipeline.

## Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Per-channel weights drift away from operator intent because nightly job uses noisy data | High | Config override (AC-6) lets operators pin specific weights. The override is loaded AFTER the computed defaults, so operator-set weights survive nightly recompute. |
| REVISE retry doubles LLM cost on borderline drafts | Medium | Per-channel REVISE-disable knob; defaults to enabled only on high-stakes channels (imessage, sms). Channels like debug/cli where extra retry has no business value get REVISE disabled. |
| Learned router skips a detector that would have caught the next Jordan-class | High | Router is allowlist (must explicitly skip), not blocklist (must explicitly include). High-stakes detectors (persona protection, naked discourse opener) are always-run by default. |
| Calibration JSON schema drift breaks daemon startup | Medium | Schema versioned; daemon falls back to uniform weights on parse error (logged loudly). Same shape as the existing config error-handling pattern. |
| The "weighted ensemble" math is wrong for adversarial inputs | Medium | The current "any single REJECT short-circuits" is itself a degenerate weighted ensemble (max function with uniform weights). Generalizing it preserves the conservative property while adding the per-channel tuning. Tests pin specific historical incident shapes (Jordan, Brea, the Annie/Mindy/Betty F25 case). |

## Out of scope (explicitly)

- The **learned router architecture**. Heuristic v0 is enough for
  AC-5. A real learned router (e.g. tiny MLP trained on offline data)
  is a follow-up sprint with its own training pipeline.
- **Channel-detector co-evolution**. If `g9_disabled_channels` is in
  config AND the arbiter has `voice.naked_discourse_opener = 0.0`,
  which wins? Spec says: disable list overrides (it's an explicit
  operator action). Documented in design.md.
- **Cross-detector calibration** (e.g. learned correlations between
  G3 and G7). Future sprint.

## Pre-conditions

- M3 dispatch spec FULLY closed (T1-T8b on origin/main as of 2026-05-26).
- DPO rejection logs accumulating in
  `~/.human/training-data/m3-dpo-rejections-*.jsonl` for ≥ 7 days
  (need a corpus to compute initial calibration).
- Sprint 41 #3 retry-outcome telemetry shipping for ≥ 7 days (need
  per-detector rescue rates).
- A confirmed operator demand for per-channel tuning beyond the
  current binary disable. Currently a HYPOTHESIS, not a measured
  need — the spec should NOT be implemented until production data
  shows the per-channel disable list is too coarse-grained.

## Status

**REQUIREMENTS DRAFT** — 2026-05-27. Not yet approved for design /
task breakdown. The pre-condition gate (≥7 days production data) is
not yet met; revisit when:
1. The DPO log has ≥1000 rejections across ≥3 channels.
2. The retry-outcome telemetry shows non-uniform rescue rates across
   detectors (currently we have G9 telemetry only; other detectors
   need their own retry-outcome counters added first — that's a
   prerequisite task).
3. An operator has filed a real issue about the per-channel disable
   being too coarse.

If all three are met, advance to design.md; otherwise this spec
sits until the demand is real.
