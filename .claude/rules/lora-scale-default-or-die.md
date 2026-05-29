# LoRA Scale — ALWAYS Set `scale=2.0` Explicitly; mlx_lm's Default (20.0) Is Catastrophic

When fine-tuning with `mlx_lm.lora`, **always set `lora_parameters.scale`
to `2.0` explicitly via a config file, and verify `adapter_config.json`
after every run.** Do NOT rely on the framework default — mlx_lm 0.31.2's
default is **`20.0`**, the catastrophic over-amplification value that
destroys the base model's instruction-following (see hazard below).

> **Empirically verified 2026-05-29 (mlx_lm 0.31.2):** the source default
> is `scale: float = 20.0` (in `mlx_lm.tuner.lora`), and a training run
> with NO explicit scale produced `adapter_config.json` → `scale: 20.0`.
> An earlier version of this rule wrongly claimed "the default is 2.0" —
> that claim cost a wasted ~4-minute training run that collapsed to val
> loss 0.825 (the over-amplification signature). The default has either
> changed across mlx_lm versions or was never 2.0. Lesson: pin the scale,
> never trust the default, and re-check `adapter_config.json` every time.

## The hazard

LoRA's forward pass is:

    output = x @ W + scale * (x @ A) @ B

where `A`, `B` are the trainable low-rank matrices and `scale` is a
multiplier baked into both training AND inference/fusion. **mlx_lm
0.31.2's default is `scale=20.0`** — the catastrophic value. Some
HuggingFace-PEFT configs use `scale = alpha / rank` with `alpha = 2 *
rank` (yielding 2.0 for rank=8), which is the sane target; the spread
of community configs runs from 0.5 to 32. The danger is that mlx_lm's
own default sits at the top of that range, so omitting the field gives
you the worst case, not a safe one.

A few people in the community report "use scale = 16 or 20 for
stronger adapter influence." Those reports usually concern
**instruction-following adapters on small bases** where the base
already follows instructions well and you want the adapter to add a
specific style.

When you copy that recommendation to a **persona fine-tune of a 31B
base** — where the goal is style + voice + recall of specific facts
— scale=20 over-amplifies the persona delta to the point that it
**dominates the base model's instruction-following circuits**. The
result: the model still recognizes the persona patterns, but its
ability to actually FOLLOW the user's instruction collapses. Output
becomes repetitive, meta-commentary, or empty-after-thought-block.

Real-world cost (2026-05-25 reactive-iMessage diagnosis): the
`gemma-4-31b-seth-v3-fused` model was trained with `scale=20.0` and
exhibited "catastrophic instruction-following collapse." Surface
symptom: every reply got rejected by `response_guard` for repetition,
retry produced meta-commentary ("REWRITE (1) The response is not
helpful..."), system fell back to canned "I will look into that and
get back to you shortly." The persona's voice was learned correctly;
the base's ability to reason and reply was destroyed.

Cost of the bug: **the local model produced ZERO usable replies in
production for ~2 weeks** until the diagnosis traced it back to
`adapter_config.json`. Surfaced only when an end-to-end probe
captured the raw model output + response_guard rejection reason.

## Why the obvious fix is wrong

❌ **"Just re-fuse with scale=2.0."** Won't work. The LoRA matrices
`A`, `B` were TRAINED to produce their effect at the chosen scale.
If you trained at scale=20.0, the learned weights have ~1/10 the
magnitude they'd have at scale=2.0 (training compensates). Re-fusing
the same weights at scale=2.0 yields an adapter contribution 10× too
weak — the persona barely registers.

The scale at training time IS the scale at fusion time. You can't
correct the mistake after the fact without retraining.

❌ **"Crank `dropout` to compensate for over-fitting."** This addresses
a different failure mode (memorization). Scale over-amplification
isn't over-fitting — the loss curve looks fine, the val loss
converges. The damage is in the magnitude of the delta applied to
the base, not in the adapter's relationship to its training data.

❌ **"Add more iterations to wash out the over-scaling."** The base
weights aren't being modified during LoRA training — only `A` and
`B` are. More iterations train `A` and `B` better but they still
get multiplied by `scale` at fusion.

## The right shape

1. **Set `scale=2.0` explicitly in a config file — NEVER omit the
   field.** Omitting it inherits mlx_lm 0.31.2's `20.0` default, which
   is the catastrophic value. After every run, `cat
   adapter_config.json` and confirm `scale` reads `2.0`.

2. **If you MUST override**, validate the chosen scale against a
   held-out base capability test BEFORE fusing into production:
   - Take 5–10 prompts that the BASE model handles well (instruction
     following, math, code, multi-turn reasoning).
   - Run them through `(base + adapter at chosen scale)` via
     `mlx_lm.generate --adapter-path <adapter> --extra-eos-token ...`.
   - Compare: does it still follow instructions, or does it degenerate
     into persona-flavored gibberish?
   - If any of the base capabilities collapse, the scale is too high.

3. **When fusing**, capture `scale_at_fusion` in the model card. The
   2026-05-25 model had no record of what scale it was fused at — we
   had to infer from `adapter_config.json` alone.

## Recipe (corrected — 2026-05-25)

The repair shipped as `seth-lora-v4-repair-<runid>`:

```yaml
# adapter_config.json equivalent
lora_parameters:
  rank: 8
  scale: 2.0      # EXPLICIT — mlx_lm 0.31.2 default is 20.0 (catastrophic). Verify in adapter_config.json.
  dropout: 0.0
```

Other hyperparams unchanged from v2:
- model: mlx-community/gemma-4-31b-it-4bit
- iters: 500 (bumped from v2's 100 to see more of 1963 training examples)
- learning_rate: 1e-5
- batch_size: 1
- num_layers: 8
- max_seq_length: 2048

## Detection signal during training

A correctly-scaled fine-tune should show:
- Initial Val loss in the 2–8 range (depends on base)
- After ~100 iters at this LR: Val loss in the 1.5–3.5 range
- Val loss plateaus or descends slowly — does NOT collapse to 0
  (which would indicate over-fitting / memorization)

After fusing, smoke-test with **3 categories** of prompts:
- **Persona prompts** — should respond in voice
- **Instruction prompts** ("Translate this to French: ...") — base
  capability, MUST still work
- **Multi-turn reasoning** — base capability, MUST still work

If any base capability collapses, scale is wrong OR num_layers too
high. Don't ship.

## Audit checklist for existing adapters

Run this when an h-uman release lands a new fine-tune:

```bash
# 1. Inspect adapter_config.json for the recipe used
cat ~/.human/training-data/adapters/<name>/adapter_config.json \
  | python3 -c "import json,sys; c=json.load(sys.stdin); print('scale:', c['lora_parameters']['scale'])"

# 2. If scale > 4.0, REQUIRE the validation tests above before fusion
# 3. If scale > 8.0, REJECT. Retrain with default.
```

## Related

- `~/.claude/rules/audit-verify-before-allege.md` — the spec labeled
  the model "broken with catastrophic instruction-following collapse"
  but didn't pin the root cause. Verification-first audit found it.
- `docs/plans/2026-05-24-reactive-imessage-recovery/` — the spec that
  triaged around this bug while it remained undiagnosed for weeks.
- `~/.claude/rules/quality-gates.md` "behavior verification" — a
  fine-tune passing val-loss is NOT enough. Base-capability
  preservation testing is required before fusion.
