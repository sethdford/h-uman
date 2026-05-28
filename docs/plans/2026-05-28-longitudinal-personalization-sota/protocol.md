# Longitudinal On-Device Personalization — Reproducible Protocol

**Status:** active (Sprints A–C shipped). Covers AC-5 (reproducibility +
synthetic fixture) and AC-8 (public-benchmark anchor).

This document is the seeded, step-by-step recipe to reproduce the
personalization-trajectory measurement, and the honest statement of what the
internal metric does and does not claim relative to public benchmarks.

## What the trajectory measures

For each adapter **generation** (gen 0 = base model, gen 1…K = successive
adapters retrained from accumulated implicit feedback) the harness measures two
orthogonal axes on a held-out prompt set:

- **Persona fidelity** — the deterministic shape classifier
  (`eval_shape_classifier.classify`), mean over held-out prompts, with a
  bootstrap 95% CI. Held FIXED across generations so the curve is comparable.
- **Base capability** — a frozen, deterministically-graded probe set
  (`scripts/data/base-capability-probes.jsonl`, scored by
  `eval_base_capability.score_base_capability`, **no LLM judge**). This is the
  guard that makes a voice gain bought by instruction-following collapse a
  FAIL, not SOTA (the 2026-05-25 `scale=20` incident,
  `.claude/rules/lora-scale-default-or-die.md`).

The pure gate (`trajectory_gate.evaluate_trajectory_gate`) PASSES iff, across
the generations: the fidelity curve is non-decreasing **within 1 stderr** of its
running maximum (noise-tolerant, not strict monotonicity), base capability stays
≥ `gen0 − ε` at **every** generation, and the newest generation clears a
published fidelity floor (default 0.80).

## A. Reproduce the method with NO model (CI-grade, ~1s)

This proves the *methodology* end to end without any private data or GPU. It is
exactly what runs in the unit suites.

```bash
# 1. Generate the shareable, PII-free synthetic held-out fixture (25 prompts).
python3 scripts/build_heldout_corpus.py --synthetic 25 \
  --out docs/plans/2026-05-28-longitudinal-personalization-sota/data/heldout-synthetic.jsonl

# 2. Run the full trajectory pipeline with the no-model deterministic stub.
python3 scripts/eval_personalization_trajectory.py --demo \
  --held-out-fixture docs/plans/2026-05-28-longitudinal-personalization-sota/data/heldout-synthetic.jsonl \
  --output-json docs/plans/2026-05-28-longitudinal-personalization-sota/data/demo-trajectory.json

# 3. Render the trajectory section on the SOTA scorecard.
python3 scripts/eval_sota_scorecard.py --markdown \
  --trajectory-json docs/plans/2026-05-28-longitudinal-personalization-sota/data/demo-trajectory.json
```

The committed `data/demo-trajectory.json` is the artifact of step 2.
**The `--demo` numbers are illustrative, not measured** — the stub answers
probes from their check specs and emits canned replies. Its only job is to prove
the pipeline runs end to end and emits a valid verdict JSON. The real claim is
always reported on real held-out data (section B).

### Determinism
- Synthetic fixture: seeded shuffle of a vetted PII-free pool
  (`generate_synthetic_prompts(n, seed)`), capped at the pool size.
- Bootstrap CI: `n_resamples` fixed, seeded in `eval_fidelity_helpers.bootstrap_ci`.
- Generation: `temp=0.0` (deterministic) on the real path.
- Probe set is hashed (`probes_sha256`); the hash is recorded in
  `trajectory.json` so a changed probe set loudly invalidates cross-generation
  comparison rather than silently shifting the baseline.

## B. Run the REAL curve on-device (live macOS, needs mlx_lm + the model)

This is the actual SOTA claim; it stays on real, on-device held-out data that
never leaves the machine.

```bash
# Manifest: ordered generations (gen 0 = base, then each real adapter).
# {"generations":[
#   {"gen":0,"label":"base","adapter_path":null,"train_pairs":0,"ts":"..."},
#   {"gen":1,"label":"v4-repair","adapter_path":"~/.human/training-data/adapters/seth-lora-v4-repair","train_pairs":1963,"ts":"..."}
# ]}

python3 scripts/eval_personalization_trajectory.py \
  --manifest ~/.human/training-data/trajectory-manifest.json \
  --model-id mlx-community/gemma-4-31b-it-4bit \
  --held-out-fixture docs/plans/2026-05-26-sprint-56-gemma-as-seth/data/heldout-prompts.jsonl \
  --cache-json ~/.human/logs/trajectory-cache.json \
  --output-json ~/.human/logs/trajectory.json

python3 scripts/eval_sota_scorecard.py --trajectory-json ~/.human/logs/trajectory.json
```

The `--cache-json` makes each nightly run measure only the NEWEST generation and
append (design D6); the full history is never recomputed. Wiring this after the
nightly retrain is task T10; the first real two-point curve (base → v4-repair)
is task T11.

## C. Relationship to public benchmarks (AC-8)

We are **not** submitting to a public leaderboard — the task surface (continual
on-device fine-tuning of one user's private model) has no public leaderboard.
The internal fidelity metric is anchored to named, real benchmarks so the number
is comparable rather than private:

- **Primary anchor — LaMP** (Salemi et al. 2024,
  [ACL](https://aclanthology.org/2024.acl-long.399/)): 7 personalized tasks with
  **time-based splits** — the closest public analog to a longitudinal
  personalization claim. Our shape-fidelity metric plays the role of LaMP's
  per-task personalized-generation quality, measured over *generations of the
  adapter* rather than over a static user profile.
- **Secondary anchor — PersonaGym / PersonaScore** (Samuel et al.,
  [arXiv 2407.18416](https://arxiv.org/abs/2407.18416)): persona-adherence
  scoring; our shape classifier is a deterministic, channel-specific stand-in
  for PersonaScore's adherence axis.
- **Adjacent (memory, not generation) — PerLTQA** (Du et al. 2024,
  [ACL](https://aclanthology.org/2024.sighan-1.18/)): semantic+episodic personal
  memory QA. Relevant to h-uman's memory stack but a different axis than this
  trajectory.

**The novel, defensible contribution is not the metric — it is the
*trajectory + base-capability guard, measured on-device under continual learning
from implicit feedback.*** No public benchmark measures that; it is structurally
uncopyable by cloud assistants (no on-device weights) and absent from
persona-text systems (no learning loop).

## D. What would falsify the SOTA claim

The claim is deliberately falsifiable:
- A flat or declining fidelity curve across real generations → gate FAIL (curve).
- Any generation dropping base capability below `gen0 − ε` → gate FAIL
  (base_capability) — the `scale=20` catch, pinned by
  `test_trajectory_gate.py::test_scale20_replay_fails_on_base_capability`.
- Final-generation fidelity below the published floor → gate FAIL (final_floor).

If the real on-device curve does not rise while preserving base capability, the
gate says so, by name, and there is no SOTA claim to make. That is the point.

## E. Artifacts & sources

- Code: `scripts/trajectory_gate.py`, `scripts/eval_base_capability.py`,
  `scripts/eval_personalization_trajectory.py`,
  `scripts/eval_sota_scorecard.py` (trajectory section),
  `scripts/build_heldout_corpus.py` (`--synthetic`).
- Tests: `scripts/test_trajectory_gate.py`,
  `scripts/test_eval_base_capability.py`,
  `scripts/test_eval_personalization_trajectory.py`,
  `scripts/test_eval_sota_scorecard.py`,
  `scripts/test_build_heldout_corpus_synthetic.py`.
- Fixtures: `data/heldout-synthetic.jsonl` (shareable),
  `data/demo-trajectory.json` (illustrative end-to-end artifact).
- Citations verified 2026-05-28 (DPO [2305.18290], KTO [2402.01306],
  SimPO [2405.14734], Anthropic persona vectors [2507.21509]); a prior pass's
  "PERSONA 2602.15669" was flagged UNVERIFIED and is not relied upon.
