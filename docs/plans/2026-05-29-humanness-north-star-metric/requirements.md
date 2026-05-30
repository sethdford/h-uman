---
title: Humanness North-Star Metric — Requirements
description: A single nightly-gated composite score that defines "better than human" for h-uman, composing Seth-fidelity, anti-AI-humanness, LLM-judge humanness, and per-contact relationship calibration.
status: draft
created: 2026-05-29
owner: seth
---

# Humanness North-Star Metric — Requirements

## Problem

h-uman's central product claim is "better than human." Today that claim is
**undefined and ungated**. The nightly fidelity gate
(`scripts/eval_fidelity_nightly.py`) measures exactly one signal — the shape
classifier's anti-AI-assistant score — and gates only on a pre-vs-post delta.
Meanwhile the codebase already computes, but never aggregates:

- persona-fidelity L1 (style_match + trait_coverage + line_consistency) — `src/eval/persona_fidelity.c`
- a 4-metric consistency scorer — `src/eval/consistency.c`
- an 8-axis stylometry breakdown — `scripts/fidelity_axes.py`
- a 12-dimension Gemini humanness judge — `scripts/eval_humanness.py`

These are "computed-but-discarded" signals. Without one tracked, release-gating
number, "better than human" stays a vibe, and regressions in voice, register,
or relationship calibration ship silently.

## Goal

Produce **one composite humanness score per night**, in `[0,1]`, decomposed into
four named sub-axes, that:

1. Runs unattended (extends the existing launchd nightly job).
2. Gates releases via per-axis no-regression **and** a composite floor.
3. Tracks a trailing historical baseline so absolute drift is visible, not just
   pre-vs-post delta.
4. Reuses existing C scorers as ground truth (no third re-implementation).

## The four axes (owner decision 2026-05-29)

| Axis | Question it answers | Source scorer |
|---|---|---|
| **A1 fidelity** | Does it sound like *Seth*? (lowercase/abbrev/length/vocab) | `hu_persona_fidelity_score_l1` (`src/eval/persona_fidelity.c`) |
| **A2 anti-AI** | Does it avoid sounding like an assistant? (no markdown, no "Certainly!", right length) | `hu_shape_classify` (`src/eval/shape.c`) |
| **A3 judge** | Does an independent LLM judge it as human? | `scripts/eval_humanness.py` (Gemini `gemini-3.1-pro-preview`), 12 dims → 1 score |
| **A4 relationship** | Does its warmth/formality match what *this contact* warrants? | new: register-distance against a per-contact target |

## Acceptance criteria (testable)

- **AC-1** A single command emits a verdict JSON containing, for a fixture run:
  `composite` (float `[0,1]`), and `axes.{fidelity,anti_ai,judge,relationship}`
  each with `mean`, `stderr`, and `n`.
- **AC-2** The four C/Python scorers feed the verdict **through the existing C
  scorers** — no new Python re-implementation of fidelity or shape logic. A test
  proves the harness invokes the C eval binary (or `human` subcommand) and parses
  its JSON.
- **AC-3** Gate logic: verdict is `FAIL` if **any** axis drops more than its
  configured per-axis tolerance below the trailing baseline, **or** the composite
  is below the composite floor. `PASS` only if all axes are within tolerance and
  composite ≥ floor. Pinned by tests for: one-axis collapse (must FAIL even if
  composite is high), composite-floor breach, and all-green PASS.
- **AC-4** A trailing baseline file accumulates each night's per-axis means;
  the gate compares against the median of the last N runs (N configurable,
  default 7). First run with no baseline → `SKIP` (bootstrap), not `FAIL`.
- **AC-5** A new fixture type exists for A4: prompts tagged
  `{prompt, channel, contact_id, target_register:{formality:[0,1], warmth:[0,1]}}`.
  At least 12 such prompts spanning ≥3 relationship profiles (e.g. partner/warm,
  colleague/neutral, stranger/distant).
- **AC-6** The relationship axis A4 scores `1 - normalized_distance` between the
  reply's measured register and the fixture's `target_register`, using the
  existing `hu_persona_effective_formality` + warmth classifiers as the measurer.
  Pinned by a test: a deliberately-too-formal reply to a warm contact scores low;
  a correctly-calibrated reply scores high.
- **AC-7** Verdict JSON is backward-compatible: the existing `pre`/`post`/`delta`/
  `gate` fields remain; the new `composite`/`axes`/`baseline` fields are additive.
- **AC-8** The composite weighting is config-driven (not hardcoded magic numbers),
  defaulting to the weights in design.md, with the weights echoed into the verdict
  JSON for provenance.
- **AC-9** Running the harness with `HU_IS_TEST`/offline (no Gemini reachable)
  degrades gracefully: A3 judge axis is marked `unavailable` and excluded from the
  composite with a logged note, rather than failing the run. Gate still evaluates
  A1/A2/A4.

## Non-goals

- Real-time inline gating of individual replies (that's the existing
  `response_guard` / `persona.c` path; this is the *nightly aggregate*).
- Multi-turn conversation drift scoring (deferred; fixtures are single-turn —
  tracked as a follow-up, see tasks.md).
- Replacing the pre-vs-post delta gate (kept alongside, per AC-7).

## Risks

- **Scorer drift** between `shape.c` and `eval_shape_classifier.py` (already two
  impls). Mitigation: route A2 through the C binary; mark the Python copy as the
  CI-only mirror in design.md.
- **Judge cost/flakiness** (Gemini). Mitigation: AC-9 graceful-degrade + cache
  via existing `hu_eval_judge_cache_t`.
- **A4 fixture authoring bias** — hand-authored target registers may encode the
  author's guess, not Seth's real calibration. Mitigation: seed target registers
  from observed per-contact `hu_communication_style_t` where available; flag
  hand-authored ones in the fixture.
