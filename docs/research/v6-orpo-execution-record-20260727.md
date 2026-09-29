---
title: "seth-glm-air-v6 — ORPO preference round, execution record"
created: 2026-07-27
status: snapshot
audience: maintainers, research
---

# seth-glm-air-v6 — ORPO preference round, execution record (2026-07-27)

**Status: adapter TRAINED and REGISTERED. NOT smoke-tested, NOT rated, NOT promoted.**
Production was never repointed; it serves `seth-glm-air-v5-20260725-093742` throughout.

Adapter: `~/.human/training-data/adapters/seth-glm-air-v6-orpo-20260727-063621`
Registry: `seth-glm-air-v6-orpo-20260727-063621` (`human_gate: PENDING`, `promoted: false`)

## What v6 targets, and why it is a preference round

The three failure modes came from unblinded human cycle-2/cycle-3 sheets, not from a
synthetic judge: over-elaboration, register/warmth mismatch, assistant-disclosure.

`docs/research/v6-corpus-regeneration-scope-20260727.md` says **do not run v6** — but it
scopes v6 as a *corpus-regeneration SFT* against a stale-facts premise it measured and
disproved (0.7% contamination). It also names the condition for revisiting: a detection
source that is "genuinely weights-shaped — voice/register, not facts." Over-elaboration
now measures as exactly that:

| Measure over all 160 cycle-4 items | Value |
|---|---|
| median len(model reply) / len(Seth reply) | **2.25×** |
| p75 / p90 | 4.26× / 7.67× |
| items where the model is longer than Seth | **82%** |

That is systematic, not exemplar-specific — and the live head already *instructs*
"Default 5-15 words", which it ignores. Instruction losing to demonstration is the same
finding as the c4-129 bio-dump.

## Corpus — 415 pairs, default-deny admission

`scripts/build_v6_preference_corpus.py`. Of 700 rows in `dpo_pairs`, **327 were rejected**:

| Source | Rows | Verdict |
|---|---:|---|
| `generated_v2` | 217 | admitted — terse-vs-assistant-prose, targets modes 1 & 3 |
| `arena` | 156 | admitted — self-play, terser/less self-referential chosen |
| `auto_correction` + `outbound_edit` | 228 | **rejected** — 109 are exact duplicates of each other; 21% show the sliding-window signature (`prompt[N] == chosen[N-1]`), i.e. "chosen" is the NEXT thread message, not a better reply |
| `user_feedback` | 51 | **rejected** — 51/51 have an empty chosen or rejected side; empty-chosen into a preference objective is the 2026-05 ORPO blank-collapse |
| `implicit_feedback` | 38 | **rejected** — inverted on inspection (assistant-register replies on the chosen side) |
| `reflection_retry` | 10 | **rejected** — echoes, literal "GOOD", a truncated fragment as chosen |
| cycle-4 curated | 42 | admitted — hand-curated from 160 (see below) |

### The cycle-4 curation, and a methodology defect it exposed

75 of the 160 cycle-4 items tripped a target-mode detector. All 75 were read by hand:
**42 kept, 33 dropped.** The detector cannot tell "the model over-elaborated" from
"Seth's logged next message answered a different message" — because
`export_seth_triples.py` pairs a context with Seth's next SENT message, and in real
threads that is frequently a non-sequitur.

Concrete poison the length filter alone would have admitted:
- `c4-013` — context "Are you going to be home?", Seth "No I am not home", model "Yeah
  I'll be around". Polarity inversion; a *grounding* bug, and training it teaches the
  model to assert a different falsehood.
- `c4-155` — the model's reply is warmer and better than Seth's mid-thread fragment.
  Training it would remove warmth, the opposite of target 2.

**This defect affects the blind A/B itself, not just training.** An unknown share of
every cycle's items pair a context with a reply that does not answer it, so some
"rater picked the real Seth" signal is raters detecting a non-sequitur rather than
detecting voice. Worth fixing in `export_seth_triples.py` before cycle 5 is rated.

## Result — the run completed, and it under-trained

200/200 iterations, ORPO, beta 0.05, lr 5e-6, rank 8, **scale 2.0 verified in
`adapter_config.json`**, num_layers 8, peak memory 63.6 GB.

| Metric | iter 50 | iter 200 |
|---|---|---|
| train loss | 0.114 | 0.070 |
| train accuracy | 0.200 | 0.300 |
| train margin | −0.097 | **−0.048** |

The margin is **negative at every point**: the model still scores the *rejected*
continuation above the chosen one. It moved toward the right sign without crossing it,
and accuracy peaked at 0.30. Going deliberately gentle to avoid the 2026-05
blank-output collapse overshot in the safe direction — **this adapter is
under-trained, and is unlikely to differ much from v5.**

### Validation did not measure anything

Every `Val` reading was bit-identical across all 9 checkpoints —
`loss 0.108, accuracy 0.136, margin −0.095` at iters 1, 25, 50 … 200 — while train
metrics moved. Per `.claude/rules/no-number-without-a-measurement.md`, identical values
to full precision are the treatment never being applied: `evaluate_orpo` is not seeing
the LoRA updates. The registry therefore records
`validation: {status: NOT_MEASURED}` and **no `val_loss` key at all**, so no promotion
gate can later read an inert number as evidence.

## Three toolchain landmines found (all now guarded)

1. **YAML keys are silently ignored unless their argparse default is `None`.**
   `mlx_lm_lora/train.py` merges with `if getattr(args, k, None) is None`. Measured:
   `train_mode` (default `"sft"`) and `beta` (default `0.1`) are the only two affected
   keys we use. `train_mode: orpo` in YAML produced "Unsupported data format for SFT
   training"; `beta: 0.05` would have **silently trained at 0.1** — double the intended
   penalty — with no error. Both are now CLI flags, and the run asserts the
   `Training Mode: ORPO` banner.
2. **`CONFIG_DEFAULTS['fuse'] = True`.** With no `fuse` key, the trainer writes a fused
   46-shard full model into the adapter dir after training. It got 13 shards / 58 GB out
   before `RuntimeError: [write] Unable to write 1476395008 bytes` and left the root
   volume at **2.1 GB free** — a production hazard. Config now pins `fuse: false`.
3. **`mlx-lm-lora` is not installed in the serving venv, and must not be.** Training runs
   from an isolated `~/.human/venvs/train312` pinned to the same `mlx==0.31.2` /
   `mlx-lm==0.31.3`; the production venv was verified unchanged.

## Production impact (disclosed in full)

Training needs ~56 GB and the box has 128 GB with prod holding ~44 GB, so prod was
stopped for the window per explicit approval. Three attempts:

| # | Outcome | Prod impact |
|---|---|---|
| 1 | Refused by the guard — `pgrep -f mlx-server` matched an unrelated orphan on :8743 | stopped ~1 min, restored by trap |
| 2 | Failed — `train_mode: orpo` silently ignored, ran as SFT | stopped ~2 min, restored by trap |
| 3 | Trained 200/200, then the fuse write filled the disk | stopped ~8 min, restored by trap |

Afterwards production **fell out of launchd** ~06:47 and was down ~80 s until manually
bootstrapped at 06:48:20. Cause not definitively established; nine sessions were running
concurrently. The restore path is now hardened: `bootstrap` before `kickstart`, and it
requires the launchd **job to be loaded**, not merely the port to be listening — a
listening socket alone is what let this go unnoticed.

Not done, deliberately: no promotion, no repoint, no config edit, no touching the
unattributed :8743 orphan.

## SUPERSEDING EVENT: the n=40 human gate landed mid-session and re-ranked the targets

At 06:18:26, while this run was in flight, a 4th human cycle scored at **n=40,
detection 0.225, Wilson CI [0.123, 0.350], PASS** (`~/.human/blind_ab_gate.json`;
rated sheet `~/.human/blind_ab_human/rating_sheet.csv`, 40/40). That is 3.3× the power
of the n=12 evidence v6 was designed against, and it supersedes the 0.167/n=12 estimate
of the same arm. (It measures the **generic 3621 B prompt** arm, not the deployed
compact head — cycle 3 measured that at 0.500/n=12.)

Note `docs/evaluation/blind_ab_gate.json` in the repo still carries the stale
`n=12 / 0.500` from 07-26; `~/.human/` is the current one.

**All 9 detections were read individually rather than taken from summary.** They do not
form one cluster:

| Cluster | n | Example |
|---|---:|---|
| wrong event-state facts | 3 | `bab_005` "done moving all settled in now" (he was still moving); `bab_114` "last day's friday" (it was the 31st) |
| performed wit | 2 | `bab_090` answers "500 right?" with "i can spot a bot from a mile away" — it does not answer at all |
| generic platitude / coach register | 2 | `bab_053` "yeah that's the corporate grind for you" vs Seth naming Vanguard, LinkedIn, incoming executives |
| flat where Seth is warm | 1 | `bab_150` "i never mentioned divorce" vs "Ha ha no no, let's get it done! ASAP" |
| over-elaboration | 1 | `bab_013` three paragraphs vs "Old as dirt" |

**This invalidates v6's corpus weighting.** Over-elaboration was the headline and is
~1/9 at n=40. My 42 curated pairs were selected almost entirely by the length detector
(74 of the 75 flagged on LEN), and `generated_v2` is terse-vs-verbose — so nearly the
whole 415-pair corpus targets the demoted axis. Combined with the negative margin, there
are two independent reasons not to rate this adapter.

**3 of the 9 are not LoRA-addressable at all.** Wrong event-state facts need
memory/retrieval; the "chosen" reply asserts a date the model has no way to know, so
training it teaches confident assertion rather than accuracy. They are excluded by id.

A re-weighted corpus is built and ready at `~/.human/training-data/glm-v61-pref`:
**463 pairs**, adding the 6 trainable human detections at weight ×8 (48 rows) so the
only human-certified pairs in the set do not drown in 400 synthetic ones.

## Recommended next step: v6.1, not a rating cycle for this adapter

Asking for human ratings is the expensive resource. Spending a cycle on an adapter whose
own training signal says "barely moved" would spend it for little information.

    beta 0.05 -> 0.15      # margin needs to cross zero; 0.05 could not
    iters 200 -> 400       # 2026-05 over-corrected at 400, but on different data
                           # and with no mid-run detector; add real validation first
    fuse: false            # already pinned
    keep lr 5e-6, rank 8, scale 2.0, num_layers 8

Fix `evaluate_orpo` (or validate out-of-band against the saved checkpoints, which exist
at iters 50/100/150/200) *before* the re-run, so collapse is detectable mid-flight
rather than inferred afterwards.

## Ready but not run

- `scripts/gen-v6-arm.sh <adapter>` — one dark window (~20 min): serves the candidate on
  :8747, runs the HTTP base-capability probe, generates the 16-item arm, tears down,
  restores prod. Refuses to generate an arm if the capability probe fails.
- `~/blind_ab_run/cycle5-20260727/contexts_c5.json` — 16 contexts, stratified 4/4/4/4
  across short / general / personal / logistics, drawn from a pool of 79 after excluding
  the 42 trained-on ids, the 24 in the **still-unrated** cycle-4 sheet, and 26
  documented-defective pairs.
- `~/blind_ab_run/cycle5-20260727/head_live.txt` — the live 5287 B production head,
  captured before training so the arm is generated under the same prompt config the
  pending cycle-4 sheet measured. It still contains the "CIO succession plan" exemplar,
  i.e. the pending fix has not landed.

## Note for whoever rates cycle-4

8 of the 42 pairs now in v6's training data are also in the pending cycle-4 human sheet:
`c4-015, c4-028, c4-071, c4-079, c4-082, c4-113, c4-129, c4-160`. That sheet is
**unaffected** — it contains v5 outputs generated before v6 existed. But those 8 items,
including the flagship `c4-129`, can never again serve as held-out tests for v6.

---

## ADDENDUM 2026-07-30 — the ORPO adapters were NO-OPS; v6.2 (SFT) is the first real one

**v6 and v6.1 never trained.** Every `lora_b` is exactly 0.0 in both, and all five v6
checkpoints (iters 50/100/150/200/final) are byte-identical (`d4b0f412…`). Since LoRA
computes `x@W + scale·(x@A)@B`, a zero `B` makes the adapter mathematically identical to
the base model. Working reference: prod v5 has 80/80 `lora_b` non-zero, max|B| 2.03e-02.

This **supersedes the "under-trained, margin stayed negative" reading above**. That
margin, and the bit-identical validation, were the frozen base model plus batch noise —
one fact, not two. Both are marked `FAILED_NO_OP` in the registry.

### Root cause: the package, not the objective or the base

60-second repro on gemma-2-2b, no production involved:

| `--train-mode` | `lora_b` non-zero | |
|---|---|---|
| `orpo` | 0/28 | NO-OP, exits 0 |
| `cpo` | 0/28 | NO-OP, exits 0 |
| `dpo` | 28/28, max 4.77e-03 | trains |

`mlx-lm-lora` 3.0.0's ORPO and CPO emit no-op adapters. DPO works but
`load_reference_model()` always loads a second full copy — 2×56 GB for GLM-4.5-Air,
which does not fit in 128 GB; its `ref_model is None` branch merely zeroes the reference
scores, so there is no legitimate reference-free path. **The objective that fits is
broken; the objective that works does not fit.**

### v6.2 — chosen-only SFT via mlx_lm.lora

`seth-glm-air-v62-sft-20260730-054131`, 463-pair corpus, 500 iters, lr 1e-5,
`mask_prompt: true`, scale 2.0. This is the trainer that produced the live v5 adapter.

| Evidence | Value |
|---|---|
| `lora_b` non-zero | **80/80**, max|B| 7.529e-03 |
| Val loss | **5.029 → 1.923** (real descent; ORPO's was inert) |
| Train loss | 5.551 → 1.044 |
| Base-capability smoke vs v5 | **12/16 vs 12/16 — tie** (3 regressions, 3 fixes) |
| Served capability probe | **6/6** (persona 2/2, instruction 2/2, reasoning 2/2) |

**What SFT gives up:** it rewards the chosen reply without penalising the rejected one,
so it teaches "sound like this" but not "and not like that". The n=40 re-weighting
survives (the 6 human-detected pairs appear 8× each).

### The arm looks better on target — with two new risks

16/16 generated. Median length ratio **1.38×** (cycle-4's v5 arm was 2.25×); longer than
Seth in **10/16** (was 82%). Replies are terse and ask concrete questions
("Scheduled to unpack what?", "What's the issue?") rather than reaching for platitudes.

Two defects a length metric would not catch:

- **`c5-010` volunteered a street address**: "1215 Bayshore Drive NE, unit 407. Why you
  trying to stalk me?" The persona guidance is city-level only, never the street address.
  Real or hallucinated, both are bad — privacy leak or confabulation. **This should block
  promotion, and the item should not go to raters as-is.**
- **`c5-005` fabricated an action**: "I checked with HR, they won't release anything
  without approval from legal." The model asserts it did something it did not do.

Plausible mechanism: the corpus rewards "specific over generic", and specificity without
a boundary produces volunteered addresses and invented actions.

### Confound for cycle 5

The persona head changed between cycle 4 and now — the bio-dump exemplar is gone
(now "Yeah, St Pete. Finally on the water") and the emoji rule tightened to "none";
5287 B → 5055 B. The arm was generated under the CURRENT head, so cycle-5 vs cycle-4
differs in **two** ways (adapter and head). Report it as "v5+old-head vs v6.2+new-head",
not as a clean adapter A/B.

### Guards added

- `scripts/train-glm-adapter.sh` now asserts `lora_b` is non-zero after every run and
  refuses to accept a no-op, refuses `--train-mode orpo|cpo` up front, and gates the
  arena-overlap check on estimated run length rather than a flat constant.
- `scripts/register_v6_adapter.py` repeats the no-op check at registration (a registry
  row is what a promotion gate reads) and detects the objective from the log instead of
  assuming it.

## mlx-tune path (contract C6, 2026-09-02)

Added an [mlx-tune](https://github.com/ARahim3/mlx-tune) (v0.6, reference-free
SimPO/KTO/ORPO, per-expert LoRA for MoE) trainer path as `--trainer mlx_tune
--train-mode simpo|kto|orpo` in `scripts/train-glm-adapter.sh`, driven by the new
`scripts/mlx_tune_train.py`. **Not run against the base model** — this section
documents the dry-run validation and the exact command for the real run, which
needs Seth's prod-stop window (same 56 GB-can't-coexist-with-:8741 constraint as
every other trainer in this file).

### Environment

New, separate venv — `~/.human/venvs/mlxtune312` — NOT `train312` (which stays
mlx_lm/mlx_lm_lora only). `mlx` and `mlx-lm` are pinned to the exact versions
already verified against GLM-4.5-Air-4bit in `train312` (`mlx==0.32.0`,
`mlx-lm==0.31.3`), so `mlx-tune` sits on top of the identical MLX/mlx_lm behavior
the v5/v6/v6.1/v6.2 adapters were trained and served against. Full freeze +
provenance in `scripts/mlx_tune_env.txt`.

### Architecture-support finding

The task assumed mlx-tune exposes an importable "supported architectures
registry" naming `glm4_moe`. **It does not** — grepping `mlx_tune/*.py` for
`SUPPORTED`/`ARCH_REGISTRY`/`registry`/`ARCHITECTURES` finds nothing LLM-related
(only small per-feature allowlists in the OCR/embeddings/audio submodules).
Text-model architecture support is inherited entirely from `mlx_lm`: mlx-tune
loads via `mlx_lm.load()`, which dispatches on `config.json`'s `model_type` to
`mlx_lm.models.<model_type>` via `mlx_lm.utils._get_classes()` (an **import**,
not a weight load), and applies LoRA — including per-expert MoE LoRA — via
dynamic introspection over the model's actual live module types
(`mlx_tune.model._resolve_target_modules` scans for
`nn.Linear`/`nn.QuantizedLinear`/`SwitchLinear`/`QuantizedSwitchLinear`), not a
name-keyed table.

Given that, `scripts/mlx_tune_train.py::resolve_architecture_support()` answers
the real question — "will mlx-tune's per-expert LoRA correctly reach every GLM
MoE expert" — with zero weight loading: read the cached `config.json`
(`model_type=glm4_moe`, `architectures=[Glm4MoeForCausalLM]`), confirm
`mlx_lm.models.glm4_moe` imports (it does, in the pinned 0.31.3), and confirm
that module builds MoE experts on `mlx_lm.models.switch_layers.SwitchLinear`
(it does: `Glm4MoeMoE.switch_mlp = SwitchGLU(...)` for routed experts, plus a
dense `shared_experts` MLP and dense early layers before
`first_k_dense_replace` — the exact mixed dense+MoE shape mlx-tune's resolver
documents handling). **Conclusion: glm4_moe is supported end-to-end** — via
mlx_lm's `model_type` dispatch, not a literal name in an mlx-tune registry.

### KTO dataset-shape mismatch (found, handled)

mlx-tune's `KTOTrainer.train()` only recognizes TRL format
`{prompt, completion, label}` or legacy `{text, label}` — **not**
`{prompt, chosen, rejected}`. Passing our preference-pair jsonl straight
through would hit its `continue` branch for every sample (empty
`tokenized_data`, then a `ZeroDivisionError` — a loud crash, not a silent
no-op, but still the wrong shape). `scripts/mlx_tune_train.py::to_kto_examples()`
translates each `(prompt, chosen, rejected)` pair into two labeled TRL-format
examples (chosen → `label=True`, rejected → `label=False`) before handing the
dataset to `KTOTrainer`. ORPO and SimPO both accept `{prompt, chosen,
rejected}` directly (confirmed by reading `ORPOTrainer._tokenize_preference_pair`
and `SimPOTrainer._tokenize_pair`) — no translation needed for those two modes.

### num_layers is NOT honored by mlx-tune's trainers unless applied first

mlx-tune's RL trainers call `self.model._apply_lora()` (no `num_layers` arg)
internally at the start of `train()`, which would apply LoRA to **every**
transformer layer regardless of the config's `num_layers: 8`. The driver
calls `model._apply_lora(num_layers=cfg["num_layers"])` itself before
constructing the trainer; `_apply_lora()`'s own `_lora_applied` guard then
makes the trainer's internal call a no-op, so the config's `num_layers` is
what actually takes effect.

### Dry-run output (verbatim, zero weights loaded)

```
$ ~/.human/venvs/mlxtune312/bin/python scripts/mlx_tune_train.py --dry-run \
    --config ~/.human/training-data/glm-v61-orpo-config.yaml --train-mode kto

======================================================================
mlx_tune_train.py --dry-run
======================================================================
[dry-run] config: /Users/sethford/.human/training-data/glm-v61-orpo-config.yaml
[dry-run] model: mlx-community/GLM-4.5-Air-4bit
[dry-run] lora_parameters: rank=8 scale=2.0 dropout=0.0 -> mlx-tune lora_alpha=16.0 (alpha = rank * scale, inverting mlx-tune's scale = alpha / rank)
[dry-run] data dir: /Users/sethford/.human/training-data/glm-v61-pref
[dry-run]   train.jsonl: 426 pairs (0 malformed)
[dry-run]   valid.jsonl: 37 pairs (0 malformed)
[dry-run] --train-mode kto: translating 426 (prompt, chosen, rejected) pairs -> 852 TRL-format (prompt, completion, label) examples -- mlx-tune's KTOTrainer does not accept the chosen/rejected shape directly
[dry-run] architecture support for 'mlx-community/GLM-4.5-Air-4bit' (ZERO weights loaded):
[dry-run]   config source:            HF hub cache: /Users/sethford/.cache/huggingface/hub/models--mlx-community--GLM-4.5-Air-4bit/snapshots/60837794f3caafc4682dd1a9188a82c55a9100ef/config.json
[dry-run]   model_type:                glm4_moe
[dry-run]   mlx_lm module importable:  True
[dry-run]   mlx_lm module file:        mlx_lm.models.glm4_moe
[dry-run]   has SwitchLinear/MoE:      True
[dry-run]   SUPPORTED:                 True
[dry-run]   note: model_type='glm4_moe' resolves to mlx_lm.models.glm4_moe.Model, which builds MoE experts on mlx_lm.models.switch_layers.SwitchLinear -- mlx-tune's dynamic target-module resolver (mlx_tune.model._resolve_target_modules) will target every routed-expert gate/up/down projection plus any dense shared-expert / early-layer projections in the same pass.
----------------------------------------------------------------------
[dry-run] PASS -- config, data, and architecture support all validated
[dry-run] NOTE: no model weights were loaded during this dry-run
```

Pair count: 426 train / 37 valid (the same `glm-v61-pref` corpus v6.1's ORPO
round used), 852 KTO-translated examples.

The `train-glm-adapter.sh --dry-run --trainer mlx_tune --train-mode <mode>`
path delegates to the same driver after its own prod-stop-independent
preflight (config exists, corpus non-empty, `lora_parameters` nested +
`scale: 2.0`) — verified end to end with `--train-mode orpo`, same output
shape.

### Exact command for the real run (NOT executed — needs Seth's prod-stop window)

```
scripts/train-glm-adapter.sh \
    --trainer mlx_tune --train-mode simpo \
    --config ~/.human/training-data/glm-v61-orpo-config.yaml \
    --beta 2.0 --gamma 0.5 \
    --tag v63-simpo --est-minutes 30
```

(swap `--train-mode kto` or `--train-mode orpo` for the other two objectives;
`--beta`/`--gamma` are SimPO's own defaults from `mlx_tune.rl_trainers.SimPOConfig`
— re-tune once a first run's margin/loss curve is in hand, the same way v6→v6.1
re-tuned ORPO's `beta`/`iters`). This goes through the exact same prod-stop /
full-reap / memory-headroom / arena-overlap guards as the `mlx_lm` and
`mlx_lm_lora` paths, plus the same post-training `lora_b` non-zero guard and
`adapter_config.json` scale=2.0 verification — nothing about those guards
changed for this trainer.

### Test coverage

`scripts/test_mlx_tune_train.py` (29 tests, pytest, run inside
`mlxtune312` — no model weights loaded by any test): config translation
(`require_lora_parameters` scale=2.0 enforcement, `lora_alpha_from_scale`
inversion), data-dir validation (happy path + missing keys + empty + missing
file), the KTO dataset-shape translation, the `adapter_config.json`
writer/verifier (create / rewrite-wrong-scale / preserve-resolved-keys /
no-op-when-correct), the `lora_b` non-zero guard (pass / all-zero / majority-zero
/ missing-file, using a real tiny `mlx.core.save_safetensors` fixture — not a
mock, since this is the exact function `assert_lora_b_nonzero` reads), and
`resolve_architecture_support` with the `mlx_lm`/`huggingface_hub` boundary
mocked (uncached model, MoE architecture, non-MoE architecture).

### What is NOT done

- The real training run — gated on Seth's prod-stop window, per the NO-GO.
- A registered/scored adapter from any of simpo/kto/orpo — none exists yet.
- Tuning `--beta`/`--gamma` against an actual loss/margin curve — the values
  above are mlx-tune's own library defaults, unvalidated against this corpus.
- `scripts/register_v6_adapter.py` was not touched — it will need a
  `mlx_tune` case if/when one of these adapters is promoted (it currently
  detects the objective from the `mlx_lm_lora` log banner format; the
  `mlx_tune` driver prints the same `Training Mode: <mode>` banner, so this
  is likely a small addition, not investigated further here).
