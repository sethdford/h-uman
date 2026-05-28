# Longitudinal On-Device Personalization — SOTA Flag — Design

**Status:** DRAFT — pending approval before tasks.md.
**Builds on:** requirements.md in this directory.

## Codebase reconnaissance (verify-before-allege)

Confirmed in the live tree before writing this design:

| Claim | Evidence |
|---|---|
| Point-in-time fidelity eval exists, with bootstrap CI + statistical/practical gate | `scripts/eval_fidelity_nightly.py` — two-pass (base, base+adapter), shape-classifier scored, one-sided t-test α=0.025, practical floor 5%. |
| Shared scoring helpers are reusable | `scripts/eval_fidelity_helpers.py` — `bootstrap_ci`, `compute_persona_fidelity_scores`, `load_held_out_prompts_from_jsonl`. |
| SOTA scorecard exists but is one-time pre/post, not a trajectory | `scripts/eval_sota_scorecard.py` — reads `eval_runs` from `~/.human/memory.db`, run_id ≤4 vs ≥5 delta. |
| The continual loop exists | `scripts/outcomes_to_dpo.py`, `dpo_mlx_train.py`, `lora_nightly.c` (`src/ml/`), `training_loop.py`, `scripts/live_fire_m3_full_loop.sh`; hot-swap via `hu_mlx_admin_swap_adapter` (`src/ml/mlx_admin.c`). |
| A held-out fixture format already exists | `docs/plans/2026-05-26-sprint-56-gemma-as-seth/data/heldout-prompts.jsonl` (JSONL, `{"prompt": ...}`); `scripts/build_heldout_corpus.py` already does PII redaction (PHONE/URL/NAME). |
| The base-capability failure mode is real and documented | `.claude/rules/lora-scale-default-or-die.md` — `scale=20` adapter lifted voice, destroyed instruction-following; ~2 weeks of zero usable replies. |

Three things I expected and confirmed, which de-risk the design:
1. **The fidelity metric is already deterministic** (shape classifier, no LLM
   judge). Reusable verbatim across generations — the precondition for a valid
   trajectory.
2. **Held-out prompts already load from JSONL** with a date-stratified, training-
   contamination-avoiding convention. The trajectory reuses the same loader.
3. **No existing tool tracks per-generation history.** The trajectory artifact is
   genuinely new; we are not duplicating an existing scorecard.

## Prior art (citations verified by research pass 2026-05-28)

These ground the claim's vocabulary and the external anchor (AC-8). All links
were verified against real, published sources; the *novel* contribution is the
on-device **trajectory + base-capability guard**, not the underlying methods.

**Preference learning (the loop's training step, already implemented):**
- DPO — Rafailov et al. 2023, [arXiv 2305.18290](https://arxiv.org/abs/2305.18290)
- KTO — Ethayarajh et al. 2024, [arXiv 2402.01306](https://arxiv.org/abs/2402.01306)
- SimPO — Meng et al. 2024, [arXiv 2405.14734](https://arxiv.org/abs/2405.14734)

**Public personalization benchmarks (external anchor, AC-8):**
- LaMP — Salemi et al. 2024, [ACL](https://aclanthology.org/2024.acl-long.399/)
  — 7 personalized tasks with **time-based splits** (the closest public analog
  to a longitudinal claim). **Primary anchor.**
- PersonaGym / PersonaScore — Samuel et al., [arXiv 2407.18416](https://arxiv.org/abs/2407.18416)
  — persona-adherence scoring. **Secondary anchor.**
- PerLTQA — Du et al. 2024, [ACL](https://aclanthology.org/2024.sighan-1.18/)
  — semantic+episodic personal memory QA (memory axis, not generation).

**Activation steering (NON-GOAL here; documented for the follow-up spike):**
- Representation Engineering — Zou et al. 2023, [arXiv 2310.01405](https://arxiv.org/abs/2310.01405)
- Inference-Time Intervention — Li et al. 2023, [arXiv 2306.03341](https://arxiv.org/abs/2306.03341)
- Anthropic Persona Vectors — 2025, [arXiv 2507.21509](https://arxiv.org/abs/2507.21509) /
  [anthropic.com/research/persona-vectors](https://www.anthropic.com/research/persona-vectors)
- MLX feasibility: no native hook API; community `mlx_fun`
  ([github.com/dexloom/mlx_fun](https://github.com/dexloom/mlx_fun)) demonstrates
  residual-stream steering via Python wrappers → **feasible-with-work, not free.**

> Note: a prior research pass returned a fabricated "PERSONA framework
> arXiv 2602.15669". Treat that ID as **UNVERIFIED / likely hallucinated** — it
> is NOT cited here. The real, citable persona-vector work is Anthropic 2025.

## Components

### C1 — `eval_personalization_trajectory.py` (the curve)
Orchestrates the longitudinal measurement. Inputs: an ordered list of adapter
generations (`gen0` = base/None, `gen1…genK` = adapter paths, each with metadata:
training-pair count, train timestamp). For each generation it calls the existing
fidelity machinery (reuse `run_eval_pass` + `compute_persona_fidelity_scores`
from `eval_fidelity_nightly.py` / `eval_fidelity_helpers.py`) and records a
time-series row. Emits `trajectory.json`:

```json
{
  "generations": [
    {"gen": 0, "label": "base", "adapter_path": null,
     "fidelity_mean": 0.586, "fidelity_ci": [0.49, 0.68],
     "base_capability": 0.94, "train_pairs": 0, "ts": "..."},
    {"gen": 1, "label": "v4-repair", "adapter_path": ".../seth-lora-v4-repair",
     "fidelity_mean": 0.856, "fidelity_ci": [0.78, 0.92],
     "base_capability": 0.93, "train_pairs": 1963, "ts": "..."}
  ],
  "gate": { ... see C3 ... },
  "verdict": "PASS|FAIL|SKIP|DEFERRED", "exit_code": 0
}
```
Caches per-generation results (keyed by adapter path + fixture hash) so a nightly
run only computes the *newest* generation and appends — it never re-runs the
whole history (compute constraint).

### C2 — base-capability probe set + deterministic scorer
A fixed, version-pinned set of ~10–15 prompts that exercise *general* ability
(NOT persona): structured extraction ("return only valid JSON: ..."), simple
translation, arithmetic/sorting, format-following ("answer in exactly one word").
Each probe has a **deterministic checker** (regex / JSON-parse / exact-match /
numeric-equality) returning 0/1 — no LLM judge. Lives as
`scripts/data/base-capability-probes.jsonl` + a `score_base_capability()` helper
in a new `scripts/eval_base_capability.py`. The probe set is FROZEN and hashed;
its hash is recorded in `trajectory.json` so a changed probe set invalidates
cross-generation comparison loudly.

### C3 — curve gate (pure, unit-testable)
A pure function `evaluate_trajectory_gate(generations, cfg) -> verdict` with no
I/O, so every branch is testable without running a model. Logic:
- **Curve rising (AC-2):** for each `gen_i (i≥1)`, require
  `fidelity_mean[i] >= running_max(fidelity_mean[0..i-1]) - stderr[i]`.
  Any generation that drops more than 1 stderr below the running max → fail,
  naming the offending generation.
- **Base-capability preserved (AC-3):** for every gen,
  `base_capability[i] >= base_capability[0] - epsilon` (default ε=0.05).
- **Final threshold (AC-4):** `fidelity_mean[K] >= final_floor` (default 0.80).
- **Verdict:** PASS iff all three; else FAIL with the specific failing axis +
  generation index. SKIP when <2 generations exist (no curve yet).
Implemented in Python (`eval_personalization_trajectory.py`). Mirrors the
predicate-extraction discipline of `security-predicate-extraction.md`: the
*decision* is a pure function; the *measurement* (running a model) is separate.

### C4 — scorecard "personalization trajectory" section
Extend `eval_sota_scorecard.py` to read `trajectory.json` and render a new
section: a per-generation table (gen, label, fidelity mean ± CI, base-capability,
train-pairs), the combined verdict, and the failing-axis note when not PASS.
Works in both plain and `--markdown` modes (AC-6).

### C5 — reproducibility: protocol + synthetic fixture
- `protocol.md` (this dir): the seeded, step-by-step run recipe + how the
  internal fidelity metric relates to LaMP/PersonaGym (AC-8).
- Extend `build_heldout_corpus.py` (already PII-redacting) with a
  `--synthetic` mode that emits a generated, PII-free `heldout-synthetic.jsonl`
  (invented contacts/banter, no real handles). The trajectory harness run on the
  synthetic fixture must produce a verdict end-to-end (AC-5), proving the
  methodology is replicable without private data.

### C6 — loop wiring
A thin driver (extend `live_fire_m3_full_loop.sh` or add
`scripts/m3_trajectory_advance.sh`) that, after a nightly retrain
(`outcomes_to_dpo → dpo_mlx_train → genK adapter`), invokes the trajectory
harness to append `genK` and refresh `trajectory.json` (AC-7). No new training
logic — it sequences existing steps.

## Data flow

```
nightly:
  tapbacks/edits/outcomes  ──> outcomes_to_dpo.py ──> dpo pairs
                                      │
                                      ▼
                            dpo_mlx_train.py ──> adapter genK  (lora_nightly.c / training_loop.py)
                                      │
                                      ▼
        eval_personalization_trajectory.py:
          for genK only (cached gens 0..K-1):
            fidelity_mean,ci  = compute_persona_fidelity_scores(run_eval_pass(genK))   [C1]
            base_capability   = score_base_capability(run_probes(genK))                [C2]
          append row -> trajectory.json
          verdict = evaluate_trajectory_gate(all_gens)                                 [C3]
                                      │
              ┌───────────────────────┴───────────────────────┐
              ▼                                                ▼
   PASS+rising -> swap genK in (hu_mlx_admin_swap_adapter)   FAIL(base-cap) -> block swap,
   curve advances on the scorecard            [C4]            keep gen K-1, log failing axis
                                                              (this is the scale=20 catch)
```

## Decisions (each tied to ≥1 AC)

| # | Decision | Rationale | ACs |
|---|---|---|---|
| **D1** | Reuse the shape classifier as the fidelity metric; do NOT introduce a new judge. | It is already validated + deterministic; holding it FIXED is what makes cross-generation comparison valid. A moving metric makes a "rising curve" meaningless. | AC-1, AC-2 |
| **D2** | Base-capability probes are deterministically gradeable (no LLM judge). | Keeps the gate cheap, reproducible, drift-free across months of generations. An LLM-judged base axis would itself drift and confound the curve. | AC-3 |
| **D3** | Curve gate is "monotone within 1 stderr of running max," not strict monotonicity. | Real adaptation on 20–30 fixtures is noisy; strict monotonicity would false-fail on sampling noise. CI-tolerant monotonicity is the statistically honest gate. | AC-2 |
| **D4** | Base-capability floor is `gen0 − ε`, and a `scale=20`-replay fixture must FAIL. | Directly encodes the `lora-scale-default-or-die.md` incident as a regression test: a voice-gain that costs base capability is a FAIL, not SOTA. | AC-3, AC-4 |
| **D5** | Curve gate is a pure function separate from model execution. | Per `security-predicate-extraction.md`: pin every gate branch with unit tests without running a 31B model. | AC-2, AC-3, AC-4 |
| **D6** | Per-generation results are cached; nightly computes only the newest gen. | Compute bound: K generations × 2 passes × 30 prompts on 31B would be hours if recomputed nightly. Append-only keeps each night to minutes. | AC-1, AC-7 |
| **D7** | Ship a synthetic PII-free fixture + protocol; real curve stays on-device. | Reproducibility (the SOTA claim must be replicable) without leaking private data (privacy-by-architecture thesis). | AC-5 |
| **D8** | Anchor to LaMP (time-based splits) as the primary public benchmark; document the relationship rather than submit to a leaderboard. | LaMP's time-based personalization splits are the closest public analog; submitting is out of scope but a named anchor makes the metric comparable, not private. | AC-8 |
| **D9** | Activation steering is explicitly a separate follow-up spike, not in this spec. | Verified feasible-with-work on MLX (`mlx_fun`), but it is orthogonal (inference-time, local-tier-only) and would bloat a measurement spec. | (non-goal) |

## Telemetry & observability

- `trajectory.json` is the durable artifact; `~/.human/logs/trajectory-<date>.json`
  the nightly snapshot.
- The scorecard's new section is the human-facing curve.
- A non-PASS verdict names the failing axis + generation so an operator sees
  *why* the curve stalled (fidelity plateau vs base-capability dip vs below
  final-floor) — per `silent-config-gated-subsystems.md`, the failure must be
  legible, not silent.

## Risks & mitigations

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| **R1** | Goodharting the shape classifier — the loop learns to game the metric, not to sound like Seth. | Medium | High (fake SOTA) | Hold the classifier FIXED; add the orthogonal base-capability axis; periodic (non-gating) LLM-judge spot audits recorded alongside, per `classifier-score-plus-flag-gate.md` hybrid discipline. |
| **R2** | Small-N noise (20–30 fixtures) makes the curve jittery → false PASS/FAIL. | Medium | Medium | Bootstrap CI + the within-1-stderr monotone rule (D3); require ≥20 prompts (already enforced in `eval_fidelity_nightly.py`). |
| **R3** | Reward hacking from implicit signals ("no reply" ≠ "bad reply"). | Medium | High | Out of scope to fix here, but the base-capability guard + curve gate *catch* the symptom (a hacked adapter that degrades base capability or plateaus fidelity fails the gate and is not swapped in). Signal-quality is a tracked upstream follow-up. |
| **R4** | Catastrophic forgetting across many generations. | Medium | High | The base-capability axis is exactly this guard; a forgetting generation drops base score below floor → FAIL → swap blocked. |
| **R5** | Compute creep as K grows. | Low | Medium | Append-only caching (D6); nightly cost is O(1) generation, not O(K). |
| **R6** | Synthetic fixture doesn't represent real distribution → protocol "works" but real curve differs. | Low | Low | Synthetic fixture proves *methodology* replicability only; the real claim is always reported on real held-out data. protocol.md states this explicitly. |

## What ships in what commit (independently revertable)

1. **C2 + C3** (base-capability probes + pure curve-gate + unit tests) — no model
   needed; fully CI-testable. Lands first.
2. **C1** (trajectory orchestrator) — depends on C2/C3; runnable on the live box.
3. **C4** (scorecard section) — depends on `trajectory.json` shape.
4. **C5** (synthetic fixture + protocol.md) — independent; can parallel C4.
5. **C6** (loop wiring) — last; sequences the existing nightly steps.

After C2+C3 alone: the gate logic is provable in CI (including the `scale=20`
regression catch) even before any live trajectory runs.

## Approval gate

design.md MUST be approved (or amended) before tasks.md is finalized.
