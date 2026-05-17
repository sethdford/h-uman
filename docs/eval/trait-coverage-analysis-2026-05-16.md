---
title: Trait Coverage Axis Analysis 2026-05-16
---

# Trait Coverage Axis — Analysis and Recommendation (2026-05-16)

## The observation

The first composite-fidelity baseline ([baseline-2026-05-16.md](baseline-2026-05-16.md))
reported a trait_coverage axis score of **0.200** on the
`lora_baseline_fixture` persona — well below the 0.5–0.7 range expected
of a persona whose example bank was AUTHORED to match the persona.

The 0.20 looks like a bug. It is not. It is the floor of the scorer
when the persona has abstract traits, no `preferred_vocab`, and the
0.2 weight of the "no avoided vocab" inverse always contributes.

This doc proves that with math and one experiment, and proposes how to
respond.

## The scorer's math

`hu_consistency_score_prompt_alignment` at
[src/eval/consistency.c:60-127](../../src/eval/consistency.c#L60)
weights three sub-scores:

```
prompt_alignment = trait_score * 0.4 + vocab_score * 0.4 + (1 - avoid_penalty) * 0.2
```

- `trait_score` = (response contains a trait-word ≥3 chars) / total_words
- `vocab_score` = (distinct preferred_vocab terms found) / preferred_count
- `avoid_penalty` = (distinct avoided_vocab terms found) / avoided_count

For `lora_baseline_fixture`:
- traits = `["direct", "warm", "low-formality"]`
- preferred_vocab = (empty)
- avoided_vocab = (empty)

The 5 responses are short greetings ("hey, sounds good lmk if u want
anything else from me today"). None contain "direct", "warm", "low",
or "formality". So:

- `trait_score = 0` (no trait words appear)
- `vocab_score = 0` (no preferred_vocab to check)
- `avoid_penalty = 0` (no avoided_vocab to violate) → `(1-0) * 0.2 = 0.2`

**Total = 0.2.** Exactly the measured value. The scorer is not broken;
it is honestly reporting that this persona's lexical fingerprint is
unmeasurable by substring match.

## The experiment

Add 6 `preferred_vocab` entries that DO appear in the bank's responses
(`lmk btw rn ty yeah yep`) and re-run:

```
[lora-baseline]   composite:       0.646 (style=0.923 traits=0.307 line=0.459 stderr=0.003 n=5)
```

- trait_coverage: 0.200 → **0.307** (+0.107)
- composite: 0.614 → **0.646** (+0.032)

`trait_score` is still 0 (abstract trait words don't appear in casual
chatter). But `vocab_score` lifted from 0 to ~0.267 — the average
"distinct preferred_vocab terms per response, divided by 6" was the
visible signal.

## What this means

The trait_coverage axis measures **lexical conformity** — how many
specific words from the persona's authored vocabulary appear in
responses. It does NOT measure abstract trait fidelity ("does this
response feel direct?"). That's a categorically different question
that requires either an LLM judge (already wired via
`hu_persona_fidelity_judge`) or an embedding-similarity model.

For chatter-style personas (short greetings, varied topics) the
substring-based trait_coverage will always be low, by design.

## Recommendation

Three options, in order of cost.

### Option A — Document and live with it (recommended for now)
- Composite floor of 0.55 already accounts for the 0.2 trait floor.
- The axis is stable run-to-run (stderr=0.003), so regressions show up.
- Future M3 work that produces matching LoRA outputs will see
  trait_coverage move proportionally (the bank-matching responses score
  ~0.307; a LoRA that produces those responses will hit ~0.307; a LoRA
  that loses the abbreviation register will fall toward 0.20).

### Option B — Update the fixture to include preferred_vocab (small win)
- Edit `tests/fixtures/lora_baseline_persona.json` to add the 6 vocab
  terms above. Composite baseline moves to 0.646.
- Update [baseline-2026-05-16.md](baseline-2026-05-16.md) and the gate
  floor accordingly (0.58 with headroom).
- Pro: gives a more honest "best possible" ceiling for this persona shape.
- Con: rewrites the fixture; check-lora-baseline.sh callers in CI need
  to see the new mean simultaneously.

### Option C — Re-weight the composite (larger change)
- Adjust `HU_PF_W_TRAITS` in `src/eval/persona_fidelity.c` from 0.30
  down to 0.15 (acknowledging the axis has narrow dynamic range for
  short-message personas).
- Increase `HU_PF_W_LINE` and `HU_PF_W_STYLE` to fill the 0.15.
- Pro: makes composite less hostage to a single axis with structural
  limits.
- Con: changes every baseline number simultaneously. Should be done
  ONCE the eval corpus is large enough to optimize over (see tuning
  protocol note in `src/eval/persona_fidelity.c`).

## Decision

**Option A for this PR.** The composite is now load-bearing in CI and
the trait_coverage signal is real, just bounded. Defer Option B/C
until either (a) a real LoRA run produces a baseline shift large enough
to motivate a fixture refresh, or (b) the persona corpus grows past
the fixture-of-one and we can compare axis dynamic ranges across
personas.

## Action items captured

- [ ] When the next persona is added to the eval corpus, capture its
  individual composite + axis scores in `docs/eval/` and update this
  doc with a comparison table.
- [ ] When M3 runs first LoRA A/B, record `pre.traits`, `post.traits`,
  and delta separately from the composite — this is the axis that will
  flag whether the adapter learned the user's CONCRETE vocabulary (high
  signal) vs only their case/length register (medium signal).
- [ ] Consider an `hu_persona_fidelity_judge`-based "abstract trait
  fidelity" L2 scorer for nightly runs — substring matching will never
  answer "does this response feel direct?" but a judge can.
