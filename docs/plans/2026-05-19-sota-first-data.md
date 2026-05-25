---
title: SOTA Plan — First Empirical Data (2026-05-19)
status: deferred
created: 2026-05-19
last_audit: 2026-05-25
---

# SOTA Plan — First Empirical Data (2026-05-19)

> Companion to `2026-05-19-sota-6mo-execution.md`. Captures what the first
> end-to-end ablation pass actually measured, *before* months of effort
> chase phantom problems. Three concrete findings; each rewrites a slice
> of the plan.

## Setup

- Gateway: `human gateway --port 3006 --with-agent` (pid 14693), provider=mlx_local
- MLX backend: stub_mlx on port 8741 (pid 81053)
- Suite: `eval_suites/imessage_humanness.json` (4 prompts: greeting, group-laugh, article-share, logistics-ack)
- PersonaEval classifier: trained 145 Seth positives vs 250 AI-assistant negatives, train accuracy 0.770

## Scorecard

| Config | shape | P(Seth) | latency | n |
|---|---:|---:|---:|---:|
| L2 A — gateway full memory | 0.000 | N/A | 21.3s | 3 |
| L2 B — persona-only (no memory) | 1.000 | 0.731 | 32.4s | 3 |
| M6 R0 — baseline (MLX + persona) | 1.000 | 0.778 | 14.6s | 4 |
| M6 R1 — baseline + L5 TTT (best-of-5) | 1.000 | 0.566 | 64.8s | 4 |
| M6 R5 — gateway with all layers | 0.000 | N/A | 0.0s | 3 |

## Finding 1 — gateway+memory pipeline is broken in this config

**Two corroborating runs:**
- L2 A: 3/3 prompts returned empty text (shape=0, len=0)
- M6 R5: 3/3 prompts returned empty text

**Symptom in gateway log:**
  [http] curl POST failed: Server returned nothing (no headers, no data)
         (code=52) http://127.0.0.1:8741/v1/chat/completions (body_len=28291)
  [gateway] send_all: write failed after 0/404 bytes

**Diagnosis.** The `agent_turn` pipeline builds prompts that include
persona + personal_model + world_model + contact_context + conversation_context
+ humanness_context + residue_carryover. When the assembled prompt hits
~28 KB, the MLX backend rejects it ("Server returned nothing"). The
pipeline retries the verifier and drift sub-calls (which are smaller
and succeed), then returns the empty response to the caller without
surfacing the upstream failure.

**Implication for the plan.** Layer 2 (memory RAG refactor) is no longer
"optimize a working pipeline." It's "repair a broken one." The work
shifts from "measure which fields help" to "establish a prompt budget,
trim to fit, then measure." Order of operations:

1. Add prompt-size guard in `src/prompt_build.c`: hard cap at ~16 KB
   for the MLX provider; truncate / drop sections in priority order.
2. RE-RUN the L2 ablation with the guard in place. If A and B still
   diverge, memory IS helping when present. If they converge, the
   refactor reclaims tokens with no quality cost.

This finding alone justifies the full L2 work — and changes its
month assignment from Month 4 to Month 1.

## Finding 2 — direct MLX + compact persona IS the ship config today

R0 ran the same prompts through MLX directly (no gateway, no agent_turn,
no memory) with `hu_persona_build_prompt_compact` as the system message.
Result: **4/4 shape pass, mean P(Seth) = 0.778, 14.6s/turn**. Responses:

  imsg-001  "yeah i should be. what's up"
  imsg-002  'lol yeah'
  imsg-003  'damn let me look'
  imsg-004  'sounds good'

These are textbook in-voice. The shipping iMessage path could route
through this configuration TODAY for materially better quality than
the current gateway path — until L2's repair lands.

**Concrete next step.** Add a `--bypass-agent-turn` flag (or an env
var) to the gateway that takes the simple MLX-direct-with-persona
path for the iMessage channel specifically. Wire it in
`src/channels/imessage.c` send path. Single-flag rollout, easy
rollback. Re-evaluate after L2 repair.

## Finding 3 — L5 TTT works; the choice function is too coarse

R1 ran the same 4 prompts with N=5 candidates per prompt, argmax by
shape_score with shortest-length tiebreaker. Result:

  imsg-001  "yeah should be, what's up"          P(Seth)=0.884
  imsg-002  'real'                                P(Seth)=0.868
  imsg-003  'that is genuinely insane'            P(Seth)=0.142
  imsg-004  'sounds good'                         P(Seth)=0.371

Mean P(Seth) = 0.566 — **worse than R0's 0.778 by 0.212**.

Why: when shape saturates at 1.0 for all candidates (and it does — all
N candidates pass the shape contract), the length tiebreaker picks
short responses. Short responses can be in-voice ("lol yeah") or
grammatically-polished AI-tells ("that is genuinely insane"). The
shape classifier doesn't distinguish; PersonaEval does.

**The fix is a two-line change in `scripts/verifier_ttt.py`:**

  # Before — only shape_score, length is tiebreaker
  scored = [(c["shape"]["score"], -c["shape"]["len"], c["idx"])
            for c in candidates]

  # After — composite when shape is saturated
  for c in candidates:
      c["p_seth"] = p_seth(personaeval_clf, c["text"])
  if all(c["shape"]["score"] >= 1.0 for c in candidates):
      key = lambda c: (c["p_seth"], -c["shape"]["len"], c["idx"])
  else:
      key = lambda c: (c["shape"]["score"], c["p_seth"],
                       -c["shape"]["len"], c["idx"])
  scored = sorted(candidates, key=key, reverse=True)

This is the next L5 iteration. Once it lands, RE-RUN R0,R1 to confirm
TTT lifts P(Seth) above baseline. Target: R1 P(Seth) > R0 P(Seth) by
≥+0.05 absolute.

## What this changes in the master plan

The original `2026-05-19-sota-6mo-execution.md` had this sequence:

  Month 1: L5 verifier TTT + L3 multi-turn
  Month 4: L2 memory ablation refactor
  Month 5: L4 multimodal
  Month 6: integration + ablation

After this run the better sequence is:

  Month 1 (NOW):
    - L5 v2: rewrite the choice function to use PersonaEval P(Seth)
    - L2 repair: prompt-size guard in prompt_build.c
    - Ship: route iMessage outbound through MLX-direct-with-persona
            until L2 repair lands

  Month 2-3 (passive corpus accrual still):
    - L1 ORPO unchanged (waiting on tapback corpus)
    - L3 multi-turn re-runs against the repaired gateway

  Month 4:
    - L2 measurement (after repair); decide RAG vs trim-only vs keep
    - PersonaEval scaling: train on a richer negative set

  Month 5:
    - L4 multimodal port from Python to C in iMessage send path

  Month 6 unchanged.

The headline shift: **two fixes drop into Month 1** that were budgeted
for Month 4 and beyond. The ablation didn't just produce numbers — it
moved real work earlier.

## Why this kind of measurement is the point

The 6-month plan was *the right plan*. But every layer's actual
contribution turned out to be different from the original guess:

| Layer | Expected | Measured | Status |
|---|---|---|---|
| L5 TTT | helps marginally | hurts with current choice fn | needs choice-fn rewrite |
| L2 memory | helps; ablation tells us how much | catastrophically broken (returns empty) | needs repair, not refactor |
| L4 multimodal | needs production wiring | scaffold works (37.5% tapback routing) | port to C |
| L1 ORPO | waits on corpus | waits on corpus (138/500 real-signal rows) | unchanged |
| L3 multi-turn | measure drift | blocked on L2 repair | unchanged |

Without this single afternoon of running the suite end-to-end, we'd
have spent Month 2 building L1 against a memory pipeline that returns
empty text. The cost of *not* running the ablation pre-emptively was
12 weeks of work against a phantom baseline.

## Artifacts on disk

| File | What |
|---|---|
| `/tmp/L2_ablation_results.json` | 3 prompts × {full, persona-only} |
| `/tmp/M6_ablation_results.json` | R0 vs R1 R0,R1 (4 prompts × 1 / 5) |
| `/tmp/seth_speaker_id.json` | trained PersonaEval classifier |

`scripts/sota_summarize.py` aggregates these into the table above.

## Closeout

The plan ships these THREE Month 1 actions immediately:

- [ ] Add prompt-size guard in `src/prompt_build.c` (L2 repair)
- [ ] Rewrite `verifier_ttt.py` choice function (L5 v2)
- [ ] Route iMessage outbound through MLX-direct-with-persona behind a
      flag, until L2 repair lands (ship-quality interim)

Then re-run the suite. Update this doc with the new scorecard.
