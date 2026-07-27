# A Stage That Cannot Measure Must Refuse to Emit a Number

When a pipeline stage writes a score, verdict, or metric that something
downstream trusts — a gate file, the adapter registry, a promotion decision —
and the measurement turns out to be **impossible** (dead dependency, empty
input, mismatched operands, unparseable output), it must emit a distinct
"could not measure" outcome with a non-zero exit and **write nothing**.

Emitting a plausible default instead produces the worst artifact in the
system: a number that is indistinguishable from a real measurement, trusted by
every consumer, and invisible in every log that looks healthy.

## The hazard

Five instances in ~two weeks, across at least three sessions. Every one was
green, silent, and wrong:

| Date | Stage | What it emitted | What had actually happened |
|---|---|---|---|
| 07-12→07-25 | fidelity nightly | `SKIP, score 1.0` × 13 nights | every generate() hit the 180s timeout; the literal `"[timeout]"` string scores **1.0** on the shape classifier, so pre = post = 1.0 |
| 07-26 | DPO regression gate | `Regression verdict: PASS (val_loss=None)` | no `valid.jsonl` in the data dir → mlx_lm never reported a Val loss → the gate had nothing to judge and passed anyway |
| 07-27 | fidelity nightly | `SKIP, score null` | asymmetric fallback paired a GLM adapter with a gemma base; a cross-family LoRA cannot bind, so **no delta was ever applied** |
| 07-26 | `score.py` | a verdict under the `human` key of `blind_ab_gate.json` | a *synthetic* judge produced it; the key name claims a human rater |
| 07-26 | `gen_direct.py` | exit 0, "finished" run | 55 rows were connection-refused; the server was down for most of it |

Cost: the v5 adapter served **unmeasured for 13 nights** while the registry
showed perfect 1.0 scores. The human gate briefly carried a machine verdict.
Each fault was found by accident or by an adversarial re-read — never by the
pipeline itself, because every one of them looked like a result.

The shape is always the same: **the code answers a question it was never able
to ask.** A timeout becomes a response, a missing loss becomes a pass, an
unbindable adapter becomes "no improvement," a dead server becomes a
completed run.

## Why the obvious fixes are wrong

❌ **"Return the safe/neutral default."** There is no neutral number. `SKIP`
reads as *"we measured, and it didn't help"* — a claim about the adapter.
`PASS` reads as *"we checked, nothing regressed."* Downstream cannot tell a
default from a datum, and the registry keeps it forever.

❌ **"Log a warning and continue."** The 13-night streak logged plenty; nobody
reads a healthy-looking nightly. A warning next to a written verdict is
strictly worse than no verdict — it creates the artifact *and* the false
impression it was reviewed.

❌ **"Fail hard on any anomaly."** Too brittle for a 3am job: a legitimately
absent adapter, a genuinely tiny batch, or an off-night with no traffic must
still exit cleanly. The distinction is not error-vs-success, it's
**measured-vs-not-measured**.

❌ **"The tests cover it."** All five stages had passing tests. Two of the
tests actively pinned the broken behavior (`fallback == DEFAULT_MODEL`,
`assert verdict == PASS`), which is worse than none — see
`tests-that-pin-bugs.md`.

## The right shape

1. **Give "cannot measure" its own outcome.** Distinct verdict string,
   distinct non-zero exit. This repo uses `DEFERRED` (exit 2) for "the
   measurement was not possible" and `INCONCLUSIVE` (exit 1) for "ran, but
   produced no judgeable evidence." Neither is `SKIP`, which means *measured,
   no effect.*
2. **Refuse to write.** No registry entry, no gate file, no results row. A
   missing row is recoverable and obvious; a fabricated row poisons the
   history that later regression checks compare against.
3. **Assert the preconditions that make measurement possible** — not only the
   output. Before scoring, verify: the operands are compatible
   (`base_adapter_family_mismatch`), enough responses are real (≥80% non-
   sentinel), the evidence field exists (`val_loss is not None`).
4. **Emit a greppable marker.** `FIDELITY_DEFERRED`, `FIDELITY_SKIP`,
   `FIDELITY_SCORER_DEGRADED`. Silence is not a signal; a marker is what the
   caretaker's loop-liveness check can actually find.
5. **Record provenance in the artifact.** `scorer: {mode, weights, model_path}`,
   `--rater`, resolution source. A number whose origin is unrecorded cannot be
   audited later — that is exactly how a synthetic verdict ended up under a
   `human` key.

## Detection — the cheapest signatures

Check these before believing any verdict:

- **Identical pre/post to full precision.** `0.3178` vs `0.3178` is not a weak
  effect; it is the treatment never being applied. Real measurements have
  noise.
- **A verdict beside a null evidence field.** `PASS (val_loss=None)`,
  `score: null`, `n: 0`.
- **Exit 0 with zero valid rows**, or a run whose per-item errors are never
  aggregated into the exit status.
- **A run of identical verdicts.** N consecutive `SKIP`s is a degenerate
  measurement until proven otherwise; the loop-liveness check flags 3.
- **A score under a key naming a source that stage cannot produce** (human vs
  synthetic, blind vs self-judged).

## When this applies / does NOT

APPLIES to any stage whose output is consumed by a gate, registry, promotion
decision, or training trigger: the fidelity nightly, blind-A/B scoring, DPO/KTO
quality gates, the outcome driver, adapter registry writes.

DOES NOT apply to exploratory analysis, ad-hoc probes, or diagnostics nobody
gates on — and not to defaults that are genuinely correct and documented (a
config fallback is fine *when it is recorded as the source*, which is the fix
that closed the 07-27 fault).

## Related

- `.claude/rules/feature-gate-requires-measurement.md` — promotions require a
  measurement; this rule protects the integrity of the measurement it consumes.
- `~/.claude/rules/integration-done-contract.md` — the same anti-vacuity
  discipline for tests (non-vacuous assertions, a real caller).
- `.claude/rules/tests-that-pin-bugs.md` — two of these faults were pinned in
  place by their own tests.
- `.claude/rules/ground-truth-over-proxy-signals.md` — a green stage is a
  proxy; the artifact it wrote is the ground truth.
- `.claude/rules/silent-config-gated-subsystems.md` — the sibling case: a
  subsystem that silently does nothing, rather than silently fabricating.
