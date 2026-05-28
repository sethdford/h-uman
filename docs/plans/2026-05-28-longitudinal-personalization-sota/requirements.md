# Longitudinal On-Device Personalization — SOTA Flag — Requirements

**Date:** 2026-05-28
**Owner:** Seth
**Status:** DRAFT — pending approval before design phase
**Mission:** M3 (Private Learning). This is the *measurable finish line* for M3,
not a new direction.

## The SOTA claim we are trying to earn

> **h-uman is the first personal AI to demonstrate a measured, monotone-within-CI
> on-device personalization *trajectory* under continual learning from implicit
> feedback — with a hard base-capability non-regression guarantee — reproducible
> via a published protocol.**

This is a *measurement* claim, deliberately. Measurement is the most defensible
kind of SOTA because it is falsifiable and structurally uncopyable by the
competition:

- Gemini / Claude / Cowork are cloud — they cannot do continual *on-device*
  learning (privacy + they don't run local weights on the user's machine).
- OpenClaw has persona text (SOUL.md) but **no learning loop** — it does not get
  measurably better at being *you* over time.
- h-uman already has the loop (DPO pair collection from iMessage tapbacks,
  `outcomes_to_dpo.py`, `dpo_mlx_train.py`, `lora_nightly.c`, `training_loop.py`,
  `hu_mlx_admin_swap_adapter` hot-swap) **and** a fidelity eval that produced a
  real number (0.586 → 0.856, +27pp, commit 9ab9b86e).

## The gap (why this isn't already done)

The existing evals measure **point-in-time** deltas, not a **trajectory**:

| Existing tool | What it measures | What it does NOT measure |
|---|---|---|
| `scripts/eval_fidelity_nightly.py` | base-vs-adapter fidelity at one moment, for one adapter, with bootstrap CI + statistical/practical gate | whether successive retrain cycles *improve* fidelity over time |
| `scripts/eval_sota_scorecard.py` | one-time pre-fix vs post-fix shape-score delta (run_id ≤4 vs ≥5) | a continuous trajectory across adapter generations |

Three things are missing, and all three are required for the claim above to be
true rather than marketing:

1. **A longitudinal curve.** Does fidelity *rise* across adapter generations
   (gen0 = base, gen1…genK = successive retrains from accumulated feedback)?
2. **A base-capability guard.** Continual learning must never degrade the
   model's general instruction-following. This axis exists *because* of the
   2026-05-25 `scale=20` incident, where an adapter lifted persona voice but
   destroyed instruction-following (`.claude/rules/lora-scale-default-or-die.md`).
   A fidelity gain bought by base-capability collapse is a regression, not SOTA.
3. **A reproducible protocol.** Today's curve would run on Seth's private
   held-out fixtures. A defensible SOTA claim needs a documented, seeded,
   replicable protocol — including a PII-free synthetic fixture so the
   *methodology* is shareable even though the personal data is not.

## User stories

- **As Seth**, I want proof that *each* retrain cycle makes the model more me —
  not just "v4 beat the base model once" — so I can trust the loop is actually
  learning, not drifting.
- **As Seth**, I want a hard guard that the personalization loop can never tank
  the model's general ability, so I never re-live the `scale=20` collapse where
  the model spoke in my voice but couldn't follow an instruction.
- **As a skeptic / future engineer**, I want a reproducible protocol and a
  scorecard I can re-run, so "personalized AI" is a falsifiable claim backed by
  a curve, not an unfalsifiable marketing line.
- **As the M3 owner**, I want the trajectory wired into the existing nightly
  loop so the curve updates itself as feedback accumulates, with no manual step.

## Acceptance criteria

Each AC is testable. A reference run must prove each empirically — not by reading.

### Trajectory measurement (the curve)
- **AC-1**: A new harness `scripts/eval_personalization_trajectory.py` runs the
  existing fidelity eval at each adapter generation `gen0=base, gen1…genK` and
  emits a time-series JSON: per-generation fidelity mean + bootstrap CI +
  generation metadata (adapter path, training-pair count, timestamp).
- **AC-2**: The **curve gate** PASSES iff fidelity is monotone-non-decreasing
  *within CI tolerance* across generations — i.e. no generation regresses
  fidelity more than 1 stderr below the running maximum. (Real adaptation is
  noisy; strict monotonicity is the wrong gate. Within-CI monotonicity is the
  right one.)

### Base-capability guard (the safety axis)
- **AC-3**: At each generation, the harness runs a **fixed, deterministically-
  gradeable base-capability probe set** (instruction-following / extraction /
  reasoning prompts that are scored WITHOUT an LLM judge — e.g. "return valid
  JSON", "translate to French", "sort these numbers"). The base-capability
  score at each gen MUST be ≥ `gen0_base_score − ε` (ε configurable, default
  0.05). A generation that raises fidelity but drops base-capability below the
  floor **FAILS** the gate. This AC specifically reproduces-and-blocks the
  `scale=20` failure mode: a fixture replaying that adapter's behavior must FAIL.
- **AC-4**: The combined **SOTA verdict** = (curve rising per AC-2) AND
  (base-capability preserved per AC-3) AND (final-generation fidelity ≥ a
  published threshold, default 0.80). All three must hold; any one failing
  yields a non-PASS verdict with the specific failing axis named.

### Reproducibility & reporting
- **AC-5**: A documented, seeded protocol (`protocol.md` in this spec dir) lets a
  third party re-run the trajectory on their own data. A **PII-free synthetic
  held-out fixture** (generated, no real contacts/handles/numbers) ships so the
  methodology is replicable without Seth's private corpus. The synthetic-fixture
  run must execute end-to-end and produce a verdict JSON.
- **AC-6**: `eval_sota_scorecard.py` gains a **"personalization trajectory"**
  section rendering the curve (per-gen fidelity + CI), the base-capability line,
  and the combined verdict, in both plain and `--markdown` output.
- **AC-7**: The trajectory harness is wired into the existing loop: a single
  orchestrator path (extend `scripts/live_fire_m3_full_loop.sh` or a thin new
  driver) runs `outcomes_to_dpo → dpo_mlx_train → new adapter genK → record genK
  in the trajectory`, so the curve advances automatically as feedback
  accumulates.

### External anchor (comparability)
- **AC-8**: The protocol documents how the internal fidelity metric relates to
  ≥1 **recognized public benchmark** for personalization (LaMP — time-based
  splits, personalized generation — is the primary anchor; PersonaGym /
  PersonaScore secondary). We are NOT required to top a public leaderboard
  (different task surface), but the claim must cite where our metric sits
  relative to a named, real benchmark so it is not a private, incomparable
  number. (Citations verified — see design.md "Prior art".)

## Non-goals (each gets its own plan if pursued)

- **Activation steering / persona vectors on the local model.** Verified
  feasible-with-work on MLX (`mlx_fun` demonstrates residual-stream hooks via
  Python wrappers; MLX has no *native* hook API). This is a strong retraining-
  free **fast-path complement** for the local tier — but it is a separate spike,
  NOT part of this measurement spec. Tracked as a follow-up.
- **Beating cloud frontier models on reasoning.** Out of scope by thesis — local
  31B does not match cloud at hard reasoning, and analytical/deep tiers stay
  cloud (Dermot C2 / `lora-scale-default-or-die.md`).
- **New training algorithms.** DPO/KTO/SimPO trainers already exist in `scripts/`.
  This spec measures the loop; it does not change how training works.
- **Changing the shape classifier** used as the fidelity metric. It is held
  FIXED on purpose (anti-Goodhart — see design Risks). Changing it would make
  cross-generation comparison invalid.
- **A general public benchmark submission.** We anchor to a public bench (AC-8)
  but do not commit to a leaderboard submission here.

## Constraints

- **Languages:** Python harness (matches existing `scripts/eval_*.py`); any C
  touchpoints (e.g. a curve-gate pure predicate, if wanted in `src/eval/`)
  follow C11 + `-Wall -Wextra -Wpedantic -Werror` + ASan-clean.
- **No LLM judge in the gate.** Both the fidelity metric (shape classifier) and
  the base-capability probes must be deterministic so the gate is cheap,
  reproducible, and free of judge drift across generations.
- **Compute:** each generation = 2 inference passes × ~20–30 prompts + ~10
  base-capability probes on a 31B model → minutes on Apple Silicon. The
  trajectory records one generation per nightly run; it does NOT re-run all
  generations each night.
- **Privacy:** real curve uses real held-out Seth data that never leaves the
  device; the *shareable* artifact is the synthetic fixture + protocol only.
- **Determinism:** all sampling seeded; `temp=0.0` generation (already the
  convention in `eval_fidelity_nightly.py`).
- **Existing tests pass:** full `human_tests` suite stays 0-fail / 0-ASan; new
  Python harness gets `scripts/test_*.py` coverage matching the existing pattern
  (`test_eval_fidelity_nightly.py`).

## Approval gate

This requirements doc MUST be approved (or amended) before design.md is
finalized. Per the project's three-file spec convention.
