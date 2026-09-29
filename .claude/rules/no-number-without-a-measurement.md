# A Measurement Pipeline Must Never Emit a Number It Didn't Measure

When a stage of an eval / scoring / generation pipeline cannot do its job, it
must **DEFER loudly** — not return a well-formed value. A number that survives
into a results file, a gate file, or a registry is indistinguishable downstream
from a real one, and it reads as *evidence about the model* when it is actually
evidence about the plumbing.

## The hazard

Seven instances in ~two weeks (2026-07-12 → 07-27), across at least three
sessions, all the same shape: **the pipeline produced output on a path where no
measurement occurred.**

| Where | What was emitted | What actually happened |
|---|---|---|
| `score.py` | `RESULT=PASS`, gate written | Sheet had **0 scored rows** — detection 0.000 on n=0 satisfies every PASS criterion vacuously (guarded 07-25) |
| `score.py` | `human: {detection 0.4, n 160}` | A **synthetic** gemma-judge run overwrote the human certification bar; promotion logic would have read 160 "human" ratings (fixed `b3720b553` + `fa0008b79`) |
| `gen_direct.py` | `DONE … 160 triples`, exit **0** | The server had died; 55 rows were `ECONNREFUSED` and written as empty strings. A dead server looked like a finished run |
| `eval_fidelity_nightly` | `pre == post == 0.3178`, verdict SKIP | Server-down fallback paired a **GLM adapter with a gemma base**; a cross-family LoRA can't bind, so the delta was structurally 0 (fixed `32d1011b4`) |
| `ab_driver` `complete_p` | "gemma_v5: 60/60 — complete" | A run killed mid-flight checkpointed only processed rows, so a **truncated 60-row file self-certified** against its own length |
| `eval_fidelity_nightly` | `SKIP, score 1.0` — **13 consecutive nights** | Every generate() hit the 180s timeout; the literal `"[timeout]"` string scores **1.0** on the shape classifier, so pre = post = 1.0. The v5 adapter served unmeasured for two weeks behind perfect registry scores (fixed `f66863e15`) |
| DPO regression gate | `Regression verdict: PASS (val_loss=None)` | No `valid.jsonl` in the data dir, so mlx_lm never reported a Val loss — the gate had no evidence and passed anyway (fixed: `INCONCLUSIVE` now blocks the swap) |

Every one of these was *green*. None threw. Six of seven were caught by a human
noticing the number looked odd — which does not scale.

The cost is worse than a crash: `pre == post` reads as "the adapter didn't
help," and a PASS on n=0 reads as "promote it." A pipeline fault gets
attributed to the model.

## Why the obvious fixes are wrong

❌ **"Add a check for that case."** Each fix above was a one-case patch, and the
shape recurred four more times in different files. The class needs a contract,
not five patches.

❌ **"The exit code will tell us."** It didn't — `gen_direct.py` exits 0 with
every request refused; `score.py` exited 0 on n=0. Exit status reflects "the
script ran", not "a measurement happened."

❌ **"A green test suite covers it."** All five paths were green. Tests assert
the happy path computes correctly; none asserted *refusal* on the degenerate
input, because nobody had imagined it yet.

❌ **"Log a warning and continue."** A warning in a 2000-line nightly log next
to a well-formed number loses. The number is what gets read.

## The contract

1. **Refuse, don't fall back, when the inputs are not what you measured.** If
   the base/adapter families disagree, the sheet is empty, the server is
   unreachable, or the row count is short — exit non-zero and write **nothing**
   (no results JSON, no gate file, no registry row). A missing verdict is
   recoverable; a fabricated one is not.
2. **Asymmetric fallbacks are the bug.** `eval_fidelity_nightly` resolved the
   adapter from config and the base from a constant, so a server-down night
   silently paired mismatched halves. If one half falls back to source X, the
   other must too.
3. **Never self-certify against your own output.** Completeness is measured
   against the *source of truth* (the input corpus), never the artifact's own
   length — a truncated file has n == len(itself) and passes.
4. **Provenance is part of the value.** A synthetic judge's number is not a
   human's. Carry the rater/judge identity with the score and key it separately;
   never let one overwrite the other's slot.
5. **Distinguish "0" from "absent".** `detection = 0.0, n = 0` must not be
   representable as a verdict. Guard `n == 0` explicitly wherever a rate is
   computed from a denominator that can be empty.

## Detection — the cheap signatures

Before believing any verdict, check for the tells this class leaves behind.
Each one below actually appeared above:

- **Identical values to full precision.** `pre == post == 0.3178` is not a weak
  effect — it is the treatment never being applied. Real measurements have
  noise; a structural zero does not.
- **A verdict beside a null/empty evidence field**: `PASS (val_loss=None)`,
  `score: null`, `n: 0`.
- **A run of identical verdicts.** N consecutive `SKIP`s is a degenerate
  measurement until proven otherwise (the caretaker's loop-liveness check flags
  3; the fidelity nightly reached 13 before anyone looked).
- **Exit 0 with zero valid rows**, or per-item errors that never aggregate into
  the exit status.
- **A number under a key naming a source that stage cannot produce** — synthetic
  under `human`, self-judged under `blind`.

## How to verify a guard actually guards

A guard that has never failed is not known to work. Prove it discriminates:

```bash
# 1. It passes on the honest path.
# 2. It FAILS against a deliberately broken copy — strip the fallback, empty the
#    sheet, point HOME at an empty dir — and fails for the RIGHT reason.
```

Precedent: the hermetic follow-up to `32d1011b4` was validated by running the
test against a resolver with the config fallback removed and confirming it
returned the exact gemma constant that caused the fault. Green alone would not
have shown that the assertion had stopped discriminating.

## When this applies / does not

- **APPLIES** to anything writing `~/.human/blind_ab_gate.json`, the adapter
  registry, `docs/plans/**/results/*.json`, `sprints/**/evidence/`, or any file a
  promotion gate reads; and to any generator whose partial output is later
  treated as complete.
- **DOES NOT apply** to exploratory/one-off analysis that never persists a
  verdict, or to genuinely optional enrichment (a missing axis score is fine —
  a missing *denominator* is not).

## Related

- `.claude/rules/feature-gate-requires-measurement.md` — activation is gated on a
  measurement; this rule protects the integrity of the measurement it gates on.
- `.claude/rules/tests-that-pin-bugs.md` — the test-side twin: an assertion that
  can't fail is the same lie in a different file.
- `.claude/rules/ground-truth-over-proxy-signals.md` — trust the run, not the
  report; here the *pipeline itself* is the unreliable reporter.
- `~/.claude/rules/integration-done-contract.md` — non-vacuous assertions.
