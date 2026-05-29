# Audit — Why Does the DPO Judge Loss Stay at ln(2)?

**Date:** 2026-05-25
**Question asked:** "Why does the daemon report `DPO judge step: loss=0.6931, alignment=0.00, pairs=32` every time? That's exactly random."
**Answer:** **Two independent bugs compound** — (1) the JUDGE'S prompt rewards generic helpfulness, not Seth-style naturalness; (2) the corpus ITSELF has 41.5% inverted labels (previously documented).

## Finding 1 — The judge's implicit prior is wrong-direction

`hu_dpo_judge_step` ([dpo.c:811–945](../../../src/ml/dpo.c:811)) is NOT a training step. It's a sanity check that asks Gemini flash-lite to RATE each pair on a 0-100 helpfulness scale, then computes DPO-shape loss from the score difference.

The judge prompt ([dpo.c:862–864](../../../src/ml/dpo.c:862)):
```c
static const char score_sys[] =
    "Rate how well this response answers the prompt on a scale of 0-100. "
    "Output ONLY a number.";
```

This is a **generic-helpfulness judge**, not a **Seth-naturalness judge**.

### What that does to your real corpus

Sample pair from `~/.human/memory.db::dpo_pairs` row 35:
```
prompt:   "hey what's up"
chosen:   "not much, just got home. you?"           ← Seth's natural edit
rejected: "Hey there! I'm doing great, thanks for asking! How are you doing today?"
```

A judge asked "how well does this ANSWER the prompt" prefers the verbose AI-style response because:
- It's more complete (mentions doing well, returns the question)
- It's more enthusiastic (exclamation marks, "great")
- It demonstrates more "effort"

The terse natural reply scores LOWER because it provides less information. Same dynamic for the other 281 pairs.

### The math signature

When the judge consistently prefers `rejected` over `chosen`:
- `chosen_score - rejected_score` is negative for every pair
- `log_ratio = (negative) / 100` → small negative
- `sigmoid(beta * log_ratio)` ≈ `0.5 - tiny`
- `-log(0.5)` = `ln(2)` = **0.6931**

**Loss = 0.6931 with alignment_score = 0.00 is the diagnostic signature of "judge always wrong-direction."**

If the judge were random-direction, alignment would be ~0.50. If the judge were right-direction, alignment would be > 0.50 and loss would decrease.

`0.00 alignment` is not weak signal — it's an inverted signal. Worth distinguishing from "data is noisy" because the fix is different.

## Finding 2 — The corpus itself has 41.5% inverted labels

Previously documented in [`2026-05-19-dpo-corpus-inverted.md`](../2026-05-19-dpo-corpus-inverted.md). Per the existing miner script `scripts/dpo_pair_persona_miner.py`, 153 of 369 rows have P(Seth-shape) HIGHER for the "rejected" column than for "chosen."

| Source | n | mean_margin | inverted | diagnosis |
|---|---:|---:|---:|---|
| reflection_retry | 10 | **−0.507** | 9 | Logger logic is backwards |
| outbound_edit | 70 | −0.014 | 34 | Definition of "kept my draft" vs "rewrote" is conflated |
| user_feedback | 69 | +0.032 | 23 | Empty-string + "SKIP" pattern in chosen column |
| generated_v2 | 220 | +0.073 | 87 | Synthesis isn't reliably picking Seth-shape as chosen |

## Compound effect

Both bugs are present today. They compound multiplicatively:

| Data quality | Judge quality | Training outcome |
|---|---|---|
| ❌ 41.5% inverted | ❌ Wrong-direction judge | Loss stays at ln(2), no learning, possibly anti-learning |
| ✅ Data cleaned | ❌ Wrong-direction judge | Loss still wrong-direction; promotion gate rejects every adapter |
| ❌ Data still inverted | ✅ Right-direction judge | Judge correctly identifies 41.5% of pairs as inverted → loss high but adapter promotion still wrong-shape |
| ✅ Both fixed | ✅ Both fixed | **Real training signal possible** |

## What to do

### Short-term (this sprint)
1. **Fix the judge prompt.** Two options:
   - Rewrite to: `"Rate how much this response sounds like Seth's natural texting style — terse, lowercase, conversational, doesn't perform helpfulness. 0-100, output only a number."`
   - Better: use embedding similarity to Seth's historical outbound (not an LLM rating at all). The `cognition.db` has enough historical text to build a reference vector.
2. **Re-run the existing `dpo_pair_persona_miner.py`** and DROP all rows with `margin < 0` (the inverted ones). This is the remediation already drafted in `2026-05-19-dpo-corpus-inverted.md` but not yet executed.
3. **Re-instrument `hu_dpo_judge_step`** to log per-pair detail when alignment is < 0.40, so future inversions are observable instead of silent.

### Medium-term
4. **Build a proper preference judge** that doesn't rely on a generic-helpfulness LLM. Candidate signals:
   - Embedding distance from Seth's historical replies in same conversational context
   - Reward model trained on `outbound_edit` rows AFTER data cleanup
   - Length/style heuristics as guardrails (Seth's median outbound is N tokens, casing distribution X, emoji frequency Y)
5. **Fix the logger paths** that produce inverted labels (`reflection_retry` has 90% inversion — almost certainly a column-swap bug somewhere in [dpo.c::hu_dpo_record_from_retry](../../../src/ml/dpo.c:202)).

### Long-term (after data + judge are fixed)
6. **Actually run LoRA training** end-to-end with non-toxic data + correct-direction judge. Verify loss decreases over iterations. Verify promotion gate accepts the adapter. Verify the adapter measurably improves persona fidelity on a held-out eval set (CLAUDE.md M3 acceptance criterion).
7. **Replace the broken `seth-v3-fused`** with a freshly trained adapter from the clean pipeline.

## Connection to the bigger picture

The CLAUDE.md M3 mission claims "LoRA adapter that measurably improves persona fidelity on inference." With this audit's findings:

- The infrastructure to train IS wired (`hu_lora_training_runner`, eval gate, adapter promotion)
- The data IS being collected (282 real rows in `dpo_pairs`)
- But the **training signal is inverted at two layers** — data labels AND judge prompt
- So the existing `seth-v3-fused` was trained against wrong-direction signal AND probably promoted via a broken eval gate
- **Result:** catastrophic instruction-following collapse (the markdown-bullet bug we found 2026-05-24)

Fixing both layers is the path to a non-broken personalization fine-tune. This is M3 sprint shaped work, multiple sessions, deserves its own three-file spec.

## Files referenced
- [`src/ml/dpo.c::hu_dpo_judge_step`](../../../src/ml/dpo.c:811) — the misaligned judge
- [`src/agent/lora_training_runner.c`](../../../src/agent/lora_training_runner.c) — actual LoRA trainer (separate from judge)
- [`docs/plans/2026-05-19-dpo-corpus-inverted.md`](../2026-05-19-dpo-corpus-inverted.md) — the data-inversion finding (complementary bug)
- [`scripts/dpo_pair_persona_miner.py`](../../../scripts/dpo_pair_persona_miner.py) — the existing miner that identified the data inversion
