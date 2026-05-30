---
title: Humanness North-Star Metric — Design
description: Compose four axes into one nightly-gated composite by reusing the existing C scorers (persona_fidelity, shape), the existing multi-axis gate (hu_eval_gate_t), and the existing external judge; add only the A4 relationship axis, the composite, and a trailing baseline.
status: draft
created: 2026-05-29
---

# Humanness North-Star Metric — Design

## Guiding constraint: reuse, don't re-implement

The repo already has every hard part. This design is **wiring + one new scorer**,
not a green-field build. Confirmed entry points (2026-05-29):

| Need | Existing primitive | File:line |
|---|---|---|
| A1 fidelity scorer | `hu_persona_fidelity_score_l1` | `src/eval/persona_fidelity.c:56` (already called from `src/ml/cli.c:1829`) |
| A2 anti-AI scorer | `hu_shape_classify` / `_ex` | `include/human/eval/shape.h:85,92` |
| A4 register measurers | `hu_persona_effective_formality`, `hu_followup_warmth_from_string` | `persona.h:719`, `follow_up.h:80` |
| Multi-axis gate w/ per-axis CI | `hu_eval_gate_t` + `hu_eval_gate_verdict_t` | `include/human/eval/eval_gate.h:18,34` |
| Bootstrap CI | `hu_bootstrap_ci_for_test` | `src/eval/bootstrap_ci.c` |
| External LLM judge | `eval_judge_external.c`, `eval_humanness.py` | `src/eval/`, `scripts/` |
| Eval CLI surface | `human eval {competitive,leaderboard,gate}` | `src/eval/cli_eval.c` |
| Nightly runner + launchd | `scripts/eval_fidelity_nightly.py` + `com.human.eval-fidelity-nightly.plist` | `scripts/` |

## Architecture (cross-language via subprocess JSON, not FFI)

Per `.claude/rules/cross-language-via-http.md`, the language boundary is a
**process boundary with a JSON contract**, exactly as the nightly harness already
shells out to `mlx_lm`. The C side owns all scoring (the ground truth); Python
orchestrates and gates.

```
eval_fidelity_nightly.py  (orchestrator, unchanged role)
  │  for each fixture prompt: generate reply via mlx server (existing)
  │
  ├─► human eval score  --in <replies.jsonl> --json <axes.json>      [NEW C mode]
  │       reads {prompt, reply, channel, contact_id, target_register}
  │       emits per-axis per-prompt scores + means:
  │         { "axes": { "fidelity":{...}, "anti_ai":{...},
  │                     "relationship":{...} }, "n": N }
  │       (A1 via fidelity_score_l1, A2 via shape_classify, A4 via NEW register-distance)
  │
  ├─► eval_humanness.py  (existing Gemini judge) → judge.json   [A3, optional]
  │       12 dims → single humanness_judge ∈ [0,1]; AC-9 graceful-degrade if offline
  │
  └─► compose + gate  [NEW Python, thin]
        composite = Σ wᵢ·axisᵢ   (weights from config, echoed to verdict)
        baseline  = median of trailing N nightly verdicts (default 7)
        gate      = hu_eval_gate-style per-axis no-regression  ∪  composite floor
        write verdict JSON (additive to existing pre/post/delta/gate)
```

### Why a new `human eval score` mode (not extend `gate`)

`human eval gate` consumes *already-computed* per-conversation scores (a CSV of
numbers) and runs the promotion gate. It does not itself score text. We need a
mode that turns `(prompt, reply, channel, contact)` → per-axis numbers using the
C scorers. That's `human eval score`. The Python side then feeds those numbers to
the *existing* gate logic (reuse `hu_eval_gate_decide_from_arrays_for_test`'s
algorithm, or call a thin `human eval gate` with the composite axes).

## Axis computation

### A1 fidelity ∈ [0,1]
Call `hu_persona_fidelity_score_l1(target_style, replies, lens, n, traits, &out)`.
`target_style` = Seth's `hu_communication_style_t` (load from personal model, or a
pinned fixture style for determinism in CI). Use `out.composite` per prompt.

### A2 anti-AI ∈ [0,1]
Call `hu_shape_classify(reply, len, channel_enum, &shape)`. Use `shape.score`.
**Note:** route through C `shape.c`, not the Python `eval_shape_classifier.py`
mirror, to kill scorer drift. Mark the Python mirror as CI-only in a comment.

### A3 judge ∈ [0,1] (optional, AC-9)
`eval_humanness.py` returns 12 dimension pass/fails. Reduce to a single score =
fraction of dimensions passed (or mean of dimension confidences if available).
If Gemini unreachable → axis `unavailable`, excluded from composite, logged.

### A4 relationship ∈ [0,1] (NEW)
The differentiating axis. For a fixture prompt tagged with
`target_register:{formality, warmth}`:

1. Measure the reply's register:
   - `formality_measured` — reuse the formality signal behind
     `hu_persona_effective_formality` (extract the underlying 0..1 estimator into
     a pure predicate `hu_register_formality_estimate(reply,len)` per
     `.claude/rules/security-predicate-extraction.md` — testable in isolation).
   - `warmth_measured` — map reply features (greeting presence, sign-off warmth,
     emoji, endearments) onto the `hu_followup_warmth_t` scale, normalized to [0,1].
2. `distance = 0.5·|formality_measured − target.formality| + 0.5·|warmth_measured − target.warmth|`
3. `A4 = 1 − distance`  (clamp [0,1])

Pinned by AC-6 tests: too-formal-to-warm-contact → low; calibrated → high.

## The composite + gate

```
composite = w_fid·A1 + w_ai·A2 + w_judge·A3 + w_rel·A4
```
Default weights (config-driven, AC-8), echoed into verdict for provenance:
`w_fid=0.30, w_ai=0.25, w_judge=0.20, w_rel=0.25`. When A3 is `unavailable`, its
weight is redistributed proportionally across A1/A2/A4 and the verdict records the
reweighting.

Gate (AC-3), hybrid per the `classifier-score-plus-flag-gate` rule:
```
FAIL if   (any axisᵢ.mean < baselineᵢ − toleranceᵢ)     # per-axis no-regression
       or (composite < composite_floor)                  # absolute floor
PASS if   all axes within tolerance AND composite ≥ floor
SKIP if   no baseline yet (first N runs)                  # bootstrap, AC-4
```
Per-axis tolerance default `0.05`; `composite_floor` default `0.70`. All in config.

### Trailing baseline (AC-4)
Append each night's `{date, axes.means, composite}` to
`~/.human/logs/humanness-baseline.jsonl`. Baseline for a given axis = median of
the last N (default 7) entries. Median, not mean, to resist a single bad night.

## Verdict JSON (additive, AC-7)
```jsonc
{
  // ... existing pre/post/delta/gate fields unchanged ...
  "composite": 0.81,
  "weights": { "fidelity":0.30, "anti_ai":0.25, "judge":0.20, "relationship":0.25 },
  "axes": {
    "fidelity":     { "mean":0.84, "stderr":0.03, "n":29, "baseline":0.82, "pass":true },
    "anti_ai":      { "mean":0.88, "stderr":0.02, "n":29, "baseline":0.86, "pass":true },
    "judge":        { "mean":0.79, "stderr":0.05, "n":29, "baseline":0.77, "pass":true, "available":true },
    "relationship": { "mean":0.74, "stderr":0.06, "n":12, "baseline":0.75, "pass":false }  // ← would FAIL the run
  },
  "humanness_verdict": "FAIL",   // ← distinct from existing delta-gate verdict
  "baseline_window": 7
}
```

## Fixture format (AC-5) — `data/relationship-prompts.jsonl`
```jsonc
{"prompt":"hey did you eat yet", "channel":"imessage", "contact_id":"partner",
 "target_register":{"formality":0.1,"warmth":0.9}, "source":"observed"}
{"prompt":"can you send the q3 deck", "channel":"slack", "contact_id":"colleague",
 "target_register":{"formality":0.5,"warmth":0.5}, "source":"hand"}
{"prompt":"is this the right address for the return", "channel":"imessage",
 "contact_id":"stranger","target_register":{"formality":0.7,"warmth":0.3}, "source":"hand"}
```
`source` flags observed (seeded from `hu_communication_style_t`) vs hand-authored
(AC-6 risk mitigation). ≥12 prompts, ≥3 relationship profiles.

## Test/gate symmetry
New C in `src/eval/` → matching `tests/test_eval_score.c` and
`tests/test_register_estimate.c` registered per
`.claude/rules/test-source-gate-symmetry.md`. A4 predicate gets isolated tests
(AC-6). Python composition gets a unit test with synthetic axis arrays proving the
one-axis-collapse FAIL (AC-3).

## Open design choices deferred to implementation
- Whether to extend `hu_eval_gate_t` with named 4-axis fields or reuse its
  4 generic slots. Lean: add a sibling `hu_humanness_gate_t` to keep the LoRA
  promotion gate semantically separate.
- Multi-turn fixtures (conversation-level drift) — explicit non-goal here, tracked
  as follow-up in tasks.md.
