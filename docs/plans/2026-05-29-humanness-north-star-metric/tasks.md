---
title: Humanness North-Star Metric — Tasks
description: Ordered, testable tasks to ship the 4-axis composite humanness gate. Each maps to acceptance criteria in requirements.md.
status: draft
created: 2026-05-29
---

# Humanness North-Star Metric — Tasks

Ordered by dependency. Each task is independently verifiable. ACs reference
requirements.md.

## Phase 1 — A4 relationship axis (the only genuinely new scorer)

- [ ] **T1. Extract `hu_register_formality_estimate(reply, len) → double[0,1]`**
  as a pure predicate (security-predicate-extraction pattern) from whatever
  feeds `hu_persona_effective_formality`. Header in `include/human/eval/register.h`.
  *Test:* `tests/test_register_estimate.c` — formal text high, casual text low.
  (AC-6)

- [ ] **T2. Add `hu_register_warmth_estimate(reply, len) → double[0,1]`** mapping
  greeting/sign-off/emoji/endearment features onto a normalized warmth scale
  (reuse `hu_followup_warmth_t` buckets). *Test:* warm vs distant replies. (AC-6)

- [ ] **T3. Add `hu_relationship_axis_score(reply, len, target_formality,
  target_warmth) → double`** = `1 − (0.5·Δformality + 0.5·Δwarmth)`, clamped.
  *Test:* too-formal-to-warm → low; calibrated → high. (AC-6)

## Phase 2 — `human eval score` C mode (reuse A1/A2, add A4)

- [ ] **T4. Add `score` subcommand to `src/eval/cli_eval.c`**: reads a JSONL of
  `{prompt, reply, channel, contact_id, target_register}`, runs
  `hu_persona_fidelity_score_l1` (A1), `hu_shape_classify` (A2),
  `hu_relationship_axis_score` (A4) per row, emits `axes.json` with per-prompt
  arrays + means + stderr (via `hu_bootstrap_ci_for_test`). Register in `main.c`
  help. *Test:* `tests/test_eval_score.c` invokes the mode on a fixture and
  asserts JSON shape + that it calls the C scorers (AC-2). (AC-1, AC-2)

- [ ] **T5. Pin scorer-drift note**: comment in `eval_shape_classifier.py` marking
  it CI-mirror-only; A2 in the gate path comes from C `shape.c`. (Risk mitigation)

## Phase 3 — composite + gate + baseline (Python, thin)

- [ ] **T6. `humanness_compose.py`** (or inline in nightly): read `axes.json` +
  optional `judge.json`, compute `composite` with config weights, redistribute
  A3 weight when unavailable (AC-9), write the additive verdict fields (AC-7, AC-8).

- [ ] **T7. Trailing baseline**: append per-night means to
  `~/.human/logs/humanness-baseline.jsonl`; baseline = median of last N (AC-4).

- [ ] **T8. Hybrid gate**: FAIL on any-axis-regression OR composite-floor; SKIP on
  no-baseline. Reuse the `hu_eval_gate` algorithm (call `human eval gate` or port
  the per-axis CI logic). *Test:* `tests/test_humanness_gate.py` (or C if ported)
  — synthetic arrays proving one-axis collapse FAILs even at high composite;
  composite-floor breach FAILs; all-green PASSes. (AC-3)

## Phase 4 — fixtures + wiring + schedule

- [ ] **T9. Author `data/relationship-prompts.jsonl`** ≥12 prompts, ≥3 profiles,
  `source` tagged; seed observed registers from `hu_communication_style_t` where
  available. (AC-5)

- [ ] **T10. Wire into `eval_fidelity_nightly.py`**: after generation, call
  `human eval score` + `eval_humanness.py`, then T6–T8. Keep existing pre/post
  delta path intact (AC-7). Verify AC-9 graceful-degrade by running with Gemini
  blocked.

- [ ] **T11. Verify the live nightly run end-to-end** via `/verify`: run the
  full harness once by hand against the live mlx server (port 8741), confirm the
  verdict JSON has all four axes + composite + baseline, and that exit codes map
  correctly. Capture the verdict JSON into `results/`.

## Follow-ups (explicit non-goals here, tracked so they aren't lost)

- [ ] **F1. Multi-turn drift fixtures** — conversation-level `line_consistency`
  using `hu_persona_fidelity_ab_score` over N-turn fixtures. (requirements Non-goal)
- [ ] **F2. Per-contact observed-register harvesting** — auto-generate A4 targets
  from real per-contact `hu_communication_style_t` instead of hand-authoring.
- [ ] **F3. Dashboard tile** — surface the nightly composite trend in `ui/`.

## Definition of done

- All ACs (AC-1…AC-9) have a passing test or a captured verdict JSON.
- Full suite green (`./build/human_tests`), 0 ASan errors.
- One real nightly verdict captured in `results/` showing 4 axes + composite.
- `lessons`/memory updated if any non-obvious scorer behavior surfaces.
