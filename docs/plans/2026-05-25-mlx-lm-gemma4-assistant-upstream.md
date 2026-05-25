---
title: mlx-lm Upstream PR — Gemma4Assistant Model Class
created: 2026-05-25
owner: TBD (external upstream)
parent: docs/plans/2026-05-24-gemma-throughput-program.md
related:
  - scripts/check-mlx-lm-cb-upstream.sh
  - docs/guides/radix-attention-upstream-watch.md
status: deferred
last_audit: 2026-05-25
---

# mlx-lm Upstream — Add `gemma4_assistant` model_type

## Status (2026-05-25)

**Scoped. Not started.** This document captures the survey + scope from the
2026-05-25 session so the work can resume cold without redoing the
investigation. Required if we want to use the purpose-built
`google/gemma-4-31B-it-assistant` draft for spec decode against
gemma-4-31B targets.

## Why this matters

Speculative decoding's acceptance rate is highest when the draft model
is purpose-built for the target. Google ships
`gemma-4-31B-it-assistant`:

- 470 MB (~120M params at FP32)
- 4 transformer layers (vs 60 for the 31B target)
- 1024 hidden size (vs 5376)
- vocab_size 262144 (matches target — tokens line up)
- model_type: `gemma4_assistant` (config.json)
- architecture: `Gemma4AssistantForCausalLM` (config.json)
- NOT gated on HuggingFace (immediate access)

Today, `mlx_lm.load(\"google/gemma-4-31B-it-assistant\")` fails because
mlx_lm 0.31.2 (latest as of 2026-05-25) doesn't have a handler for
`gemma4_assistant`. The next-best option is `google/gemma-4-E2B-it`
(~2B params, model_type=`gemma4`) which works but is 4× the assistant's
size and proportionally slower per draft token.

## Current mlx_lm.models structure

Verified by inspecting `mlx_lm.models.gemma4` in 0.31.2:

```python
# mlx_lm/models/gemma4.py (≈70 lines)
@dataclass
class ModelArgs(BaseModelArgs):
    model_type: str = "gemma4"
    text_config: dict = None
    vocab_size: int = 262144
    # ...

class Model(nn.Module):
    def __init__(self, args: ModelArgs):
        super().__init__()
        self.language_model = gemma4_text.Model(
            gemma4_text.ModelArgs.from_dict(args.text_config)
        )
    def __call__(self, inputs, cache=None, ...):
        return self.language_model(inputs, cache=cache, ...)
    def sanitize(self, weights):
        # strip vision_tower, multi_modal_projector, audio_tower
        # rename language_model. → language_model.model.
        ...
```

The pattern is: `gemma4` is a thin wrapper that delegates to
`gemma4_text` (the actual transformer) and handles the multimodal
plumbing (vision/audio towers stripped, weight prefix rewriting).

## Expected PR shape

A `gemma4_assistant` handler will follow the same wrapper pattern:

```python
# mlx_lm/models/gemma4_assistant.py (≈40-70 lines, estimate)
from . import gemma4_text
from .base import BaseModelArgs

@dataclass
class ModelArgs(BaseModelArgs):
    model_type: str = "gemma4_assistant"
    text_config: dict = None
    vocab_size: int = 262144
    # If the assistant uses different defaults, override here

class Model(nn.Module):
    def __init__(self, args: ModelArgs):
        super().__init__()
        self.language_model = gemma4_text.Model(
            gemma4_text.ModelArgs.from_dict(args.text_config)
        )
    def __call__(self, inputs, cache=None, ...):
        return self.language_model(inputs, cache=cache, ...)
    def sanitize(self, weights):
        # TODO — verify assistant's weight naming convention by
        # inspecting the actual .safetensors file. The assistant is
        # purpose-built so the prefix may differ from gemma4's
        # language_model.* convention.
        ...
```

## Open questions (requires downloading the model)

1. **Weight naming.** The 939 MB `model.safetensors` in the assistant
   repo likely uses keys like `language_model.model.layers.{0..3}.*`
   but the assistant architecture might use a different prefix
   (`assistant.layers.*` or similar). Need to load the safetensors
   index and dump key names before writing `sanitize()`.

2. **Architectural deltas.** A 4-layer model with hidden_size=1024
   may have non-standard attention or MLP shapes. The `text_config`
   in the assistant's config.json captures most, but a quick read of
   the safetensors weight shapes vs gemma4_text's expected layout
   would catch any incompatibilities.

3. **Tokenizer.** vocab_size matches (262144) so the SentencePiece
   tokenizer SHOULD be byte-identical to gemma-4-31B's. Verify with
   `tokenizer.json` diff between assistant and 31B repos.

4. **Generation config.** Spec decode in mlx_lm uses the same
   sampling config for draft and target. The assistant might ship
   with a different default temperature / top_p that we'd want to
   respect or document.

## Path to PR

1. **Operator step** (one-time): `huggingface-cli download
   google/gemma-4-31B-it-assistant` (~939 MB, no gating).
2. **Inspection** (~30 min): dump weight keys, compare to gemma4_text
   convention, draft `sanitize()`.
3. **Implementation** (~1-2 hr): write `gemma4_assistant.py` modeled
   on `gemma4.py`. Most likely the only real delta is `sanitize()`
   and the `model_type` field.
4. **Local validation** (~30 min): `mlx_lm.load("path/to/assistant")`
   should now succeed. Run a smoke generate against a short prompt
   to confirm the weights produce non-garbage tokens.
5. **Upstream PR**: fork mlx-explore/mlx-lm, add the file, write a
   minimal test (canned config.json + zero-init weights, asserts the
   Model class instantiates without error). PR with the rationale
   linking back to Google's gemma-4-assistant model card.

## Estimated effort

- Best case (assistant is structurally identical to gemma4_text with
  different prefix): **2–3 hours total**, mostly the round-trip with
  upstream review.
- Worst case (assistant has novel attention layout — multi-query at
  a different head split, or a different RoPE config): **1–2 days**
  to add a `Gemma4AssistantModel` class in `gemma4_text.py` or as a
  new file.

## When this unblocks us

Once the PR lands and a new mlx_lm release ships:

- `mlx_lm.load("google/gemma-4-31B-it-assistant")` works
- Swap default draft in `scripts/train-persona-draft.sh` from
  `google/gemma-4-E2B-it` (~2B params) to the assistant (470M, 4
  layers). Same Gemma 4 family; assistant is also trained to predict
  the 31B's distribution at a much lower per-token cost.
- The `scripts/check-mlx-lm-cb-upstream.sh` watch could be extended
  to detect this dimension (currently watches for continuous
  batching only).

## Not in scope for THIS document

- The actual implementation. This is a scoping pass — sized to be
  cold-startable later.
- Negotiating with mlx-explore maintainers on acceptance criteria
  for the PR.
- Compiling and benchmarking the draft model once it loads.
