---
title: "Initiative 06 — SimPO + ORPO + GRPO-2 trainers behind the `hu_rl_trainer_t` vtable"
slug: simpo-orpo-grpo2
created: 2026-05-11
status: deferred
owner: ML subsystem (sprint planning will assign)
risk_tier: medium
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-11-full-sota-rl-improvement-loop-design.md
  - 2026-05-11-rl-loop-phase-0-honesty.md
  - 2026-05-11-rl-loop-phase-1-llamacpp.md
  - 2026-05-11-init-04-mlx-qwen3-provider.md
  - 2026-05-11-init-05-verifier-driven-ttt.md
  - 2026-05-11-init-07-thinkprm-verifier.md
  - ../../include/human/ml/dpo.h
  - ../../include/human/ml/lora.h
  - ../../include/human/ml/optimizer.h
  - ../../include/human/ml/model.h
  - ../../include/human/ml/cli.h
  - ../../src/ml/cli.c
  - ../../src/ml/lora.c
  - ../standards/engineering/principles.md
  - ../standards/engineering/naming.md
  - ../standards/engineering/performance.md
  - ../standards/engineering/anti-patterns.md
arxiv:
  - "2405.14734 — Meng et al., SimPO: Simple Preference Optimization with a Reference-Free Reward (May 2024)"
  - "2403.07691 — Hong et al., ORPO: Monolithic Preference Optimization without Reference Model (March 2024)"
  - "2402.01306 — Ethayarajh et al., KTO: Model Alignment as Prospect Theoretic Optimization"
  - "2305.18290 — Rafailov et al., Direct Preference Optimization (DPO, NeurIPS 2023)"
  - "2402.03300 — Shao et al., DeepSeekMath / GRPO (group-relative policy optimization)"
  - "2305.10425 — Zhao et al., SLiC-HF: Sequence Likelihood Calibration"
  - "GRPO-2 (April 2026 preprint to be cited at implementation kickoff; we lock the loss form on §3.4)"
last_audit: 2026-05-25
---

# Initiative 06 — SimPO + ORPO + GRPO-2 trainers behind `hu_rl_trainer_t`

> **One-line.** Move beyond DPO-only preference optimization. Add three new
> factories — SimPO (reference-free, lighter), ORPO (preference + SFT in one
> pass), GRPO-2 (cheap group-relative variant) — each an instance of a single
> `hu_rl_trainer_t` vtable. The existing `hu_dpo_judge_step` (LLM-as-judge) and
> the planned-Phase-2 `hu_dpo_real_step` stay as baselines.

This initiative does **not** ship a new on-device frontier-model trainer (Init
04 owns the MLX provider; the full-SOTA spec's Phase 2 owns `hu_dpo_real_step`
and reference-policy log-probs). It ships the **trainer family** that those
loss heads plug into: a clean vtable, three new loss heads with deterministic
golden tests, and a single CLI surface (`human ml rl-train`) that selects an
algorithm. The narrative win is moving the project from "DPO-only" to
"DPO + reference-free SimPO + monolithic ORPO + cheap GRPO-2" without forcing
the rest of the binary to learn three new APIs.

---

## 1. Scope

### 1.1 In scope (v1)

1. New public header `include/human/ml/rl_trainer.h` defining `hu_rl_trainer_t`
   and the three factory functions (§3.1–§3.2). The shape matches the design
   spec `2026-05-11-full-sota-rl-improvement-loop-design.md` §3.4, modulo the
   `step(...) → out_loss` signature being upgraded to `step(...) → hu_train_telemetry_t *`
   (§3.3 explains why).
2. New trainer dispatcher `src/ml/rl_trainer.c` that owns the vtable wiring
   and the per-algorithm factory dispatch (~120 LOC).
3. Three new loss-head files: `src/ml/rl_simpo.c` (~280 LOC),
   `src/ml/rl_orpo.c` (~320 LOC), `src/ml/rl_grpo2.c` (~420 LOC).
4. Three new config structs — `hu_simpo_config_t`, `hu_orpo_config_t`,
   `hu_grpo2_config_t` — each with `_default()` helpers returning known-good
   hyperparameters (§3.4).
5. New CLI subcommand `human ml rl-train --algorithm=simpo|orpo|grpo2 --pairs=<path>
   --out=<adapter> [--beta=<f>] [--gamma=<f>] [--lambda=<f>] [--epsilon=<f>]
   [--group-size=<n>] [--steps=<n>] [--time-budget=<sec>]`, declared in
   `include/human/ml/cli.h`, handler in `src/ml/cli.c::hu_ml_cli_rl_train`
   that dispatches into `src/ml/rl_trainer.c` (§4).
6. One **deterministic golden test per algorithm** that pins the exact loss
   value on a tiny fixture (toy 1-layer GPT, seed 12345, batch of 4 hand-rolled
   preference pairs). The values are computed once at implementation time and
   pinned forever (§5.1).
7. One **finite-difference gradient check per algorithm** verifying
   `‖∂L/∂A_lora — fd_grad‖∞ < 1e-3` (§5.2). Reuses the same convention as
   `tests/test_ml.c::test_train_pipeline`.
8. One integration test per algorithm exercising the full
   `hu_rl_trainer_create_* → prepare → step × N → save_adapter` path against
   a `hu_lora_adapter_t` (§5.3).
9. One libFuzzer harness `fuzz/fuzz_rl_trainer_config.c` that fuzzes
   JSON config parsing for all three algorithm configs (§5.4).
10. Coexistence with `hu_dpo_judge_step` (renamed in Phase 0) and the
    forthcoming `hu_dpo_real_step` (full-SOTA spec Phase 2). Both keep their
    current public surfaces; neither is moved behind `hu_rl_trainer_t` in v1
    (§7 explains why).

### 1.2 Explicitly out of scope (deferred)

- ❌ Wiring `hu_dpo_real_step` into `hu_rl_trainer_t` — full-SOTA spec Phase 2
  owns that decision (the design here is forward-compatible).
- ❌ KTO and SLiC trainers — referenced as completeness checks (§3.4 family
  table) but not shipped here. KTO ships in full-SOTA spec Phase 3; SLiC
  remains parked.
- ❌ Multi-rollout sampling infrastructure (`src/ml/rollout.c`). GRPO-2 in v1
  uses a **degenerate group sampler** (§3.4.3) that turns the already-collected
  preference pair `(yw, yl)` into a 2-rollout group with deterministic rewards
  `(+1, -1)`. Real multi-rollout sampling against `hu_provider_t.chat` lands
  in the full-SOTA spec Phase 4.
- ❌ Frontier-model adapter format conversion (HUML LoRA ↔ MLX safetensors ↔
  GGUF). Init 04 + full-SOTA spec §4.8 own that bridge.
- ❌ MLX subprocess plumbing. Init 04 owns it. v1 of this initiative trains
  **CPU-only** against the in-tree reference `hu_gpt_create` / `hu_lora_*`
  primitives — the same trainer engine `lora-persona` uses today.
- ❌ Activation steering (Init 01), MoLoRA routing (Init 02), TTT
  (Init 05) — orthogonal initiatives that may eventually share the
  `hu_rl_trainer_t` surface but do not in v1.
- ❌ Distributed / federated training (Init 08).

---

## 2. Why this initiative, why now

The committed `src/ml/dpo.c` is an **LLM-as-judge** harness — Phase 0 already
established this and renamed it from `hu_dpo_train_step` to `hu_dpo_judge_step`.
The full-SOTA spec's Phase 2 schedules `hu_dpo_real_step` (real reference-policy
DPO with policy log-probs and frozen π_ref). That is good but not enough:

- DPO requires a frozen reference model. On-device that doubles peak RSS during
  training. SimPO drops the reference policy entirely (§3.4.1) — a 2× memory
  saving at the cost of an extra `γ` hyperparameter and a length-normalized
  margin.
- DPO assumes the policy was already SFT'd. If the user has a freshly
  banks-from-history fine-tune (Track D Phase 1 ships this), running SFT
  *and* DPO is two passes. ORPO bakes both into one pass with an odds-ratio
  penalty term (§3.4.2). This matters on-device because every wall-clock
  minute of training is a minute the daemon can't process new turns.
- DPO is pair-only. The full-SOTA spec's Phase 4 plans full GRPO with
  multi-rollout sampling against a live provider. That requires shipping
  Init 04 first. Until then we can still get **most** of GRPO's group-relative
  advantage signal from existing preference pairs — that is GRPO-2 (§3.4.3),
  an explicitly cheap variant that turns each `(yw, yl)` pair into a degenerate
  2-rollout group and applies the clipped-PPO objective with a reference-free
  KL surrogate.

The April–May 2026 arXiv literature has settled on **all three** of these as
distinct, useful, non-replaceable trainers. Choosing only one would force a
binary tradeoff we don't need to make. The vtable design is the cheap way to
ship them all.

The other reason this initiative is needed now: Init 05 (Verifier-Driven
TTT) and Init 07 (ThinkPRM) both want to call a trainer interface, not a
specific algorithm. Without `hu_rl_trainer_t` they would each carry their own
hard-coded DPO call into their own `agent_turn` hooks. With it, they call
`trainer->vtable->step(...)` and the algorithm is a runtime choice.

---

## 3. Architecture

### 3.1 The `hu_rl_trainer_t` vtable (new public surface)

Header location: `include/human/ml/rl_trainer.h` (new file). The spec
(`2026-05-11-full-sota-rl-improvement-loop-design.md` §3.4) defines the shape
in prose; this initiative locks the concrete C declaration.

```c
/* include/human/ml/rl_trainer.h — proposed canonical home.
 *
 * One trainer family, three loss heads in v1 (SimPO, ORPO, GRPO-2).
 * The vtable is intentionally small: prepare a batch, take one
 * gradient step, optionally save the adapter, deinit. The loss-head
 * file (rl_simpo.c / rl_orpo.c / rl_grpo2.c) owns every algorithm
 * detail; this header is the only thing callers see. */

#ifndef HU_ML_RL_TRAINER_H
#define HU_ML_RL_TRAINER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dpo.h"        /* hu_preference_pair_t */
#include "human/ml/lora.h"
#include "human/ml/model.h"
#include "human/ml/optimizer.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A batch of preference pairs (chosen vs rejected) plus optional
 * tokenized forms. The trainer is allowed to assume tokens are
 * already produced; the CLI handler does tokenization once at
 * prepare-time so step() stays hot-loop-clean. */
typedef struct hu_pref_batch {
    const hu_preference_pair_t *pairs;
    size_t n_pairs;

    /* Tokenized forms, owned by the trainer's prepare() output —
     * NULL on input to prepare(), non-NULL after. */
    const int32_t *chosen_tokens;
    const size_t  *chosen_lens;
    const int32_t *rejected_tokens;
    const size_t  *rejected_lens;
    const int32_t *prompt_tokens;
    const size_t  *prompt_lens;
} hu_pref_batch_t;

/* Per-step telemetry. The trainer fills only the fields it computes;
 * unused fields stay at the zero-initialized default so callers can
 * diff against `(hu_train_telemetry_t){0}` to detect "trainer didn't
 * fill this metric" without false negatives. Schema is stable for the
 * life of v1 — additions go at the end. */
typedef struct hu_train_telemetry {
    double loss;
    double grad_norm;
    double reward_margin;       /* mean (r_chosen - r_rejected); SimPO / GRPO-2 fill */
    double kl_to_ref;           /* 0.0 for reference-free (SimPO, ORPO odds-ratio term) */
    double sft_loss;            /* ORPO fills (NLL on chosen); others 0.0 */
    double advantage_mean;      /* GRPO-2 fills; others 0.0 */
    double advantage_std;       /* GRPO-2 fills; others 0.0 */
    size_t tokens_processed;
    size_t pairs_processed;
    int    step_index;          /* monotonically increasing within a run */
} hu_train_telemetry_t;

typedef struct hu_rl_trainer_vtable hu_rl_trainer_vtable_t;

typedef struct hu_rl_trainer {
    void *ctx;
    const hu_rl_trainer_vtable_t *vtable;
} hu_rl_trainer_t;

typedef struct hu_rl_trainer_vtable {
    /* Stable lowercase factory key: "simpo" | "orpo" | "grpo2". */
    const char *(*name)(void *ctx);

    /* Tokenize / pack / pre-compute any per-batch state. Required
     * before step(). Idempotent — may be called repeatedly with
     * different batches. */
    hu_error_t (*prepare)(void *ctx, hu_pref_batch_t *batch);

    /* Take one gradient step on the provided batch. The trainer
     * owns the model, the optimizer, and the LoRA adapter — caller
     * provides only the batch and an out-telemetry pointer. */
    hu_error_t (*step)(void *ctx, const hu_pref_batch_t *batch,
                       hu_train_telemetry_t *out);

    /* Serialize the trained adapter to disk in the in-tree LORA
     * format (`hu_lora_save`). Init 04 / Phase 2 handle conversion
     * to MLX safetensors / GGUF off this canonical artifact. */
    hu_error_t (*save_adapter)(void *ctx, const char *path);

    void (*deinit)(void *ctx);
} hu_rl_trainer_vtable_t;

/* ── Algorithm configs ───────────────────────────────────────── */

typedef struct hu_simpo_config {
    hu_model_t       *policy;       /* required, caller-owned */
    hu_lora_adapter_t *adapter;     /* required, caller-owned, attached to policy */
    hu_ml_optimizer_t *optimizer;   /* required, params registered */
    float beta;                     /* default 2.0 (per arXiv:2405.14734 §5.1) */
    float gamma;                    /* target reward margin; default 1.0 */
    float length_norm;              /* 1.0 = standard SimPO; 0.0 = no length norm */
} hu_simpo_config_t;
hu_simpo_config_t hu_simpo_config_default(void);

typedef struct hu_orpo_config {
    hu_model_t       *policy;
    hu_lora_adapter_t *adapter;
    hu_ml_optimizer_t *optimizer;
    float lambda;                   /* odds-ratio loss weight; default 0.1 (paper §3) */
    float odds_clip;                /* clip odds at this magnitude; default 88.0 (≈ log(exp(88)) limit before f32 overflow) */
} hu_orpo_config_t;
hu_orpo_config_t hu_orpo_config_default(void);

typedef struct hu_grpo2_config {
    hu_model_t       *policy;
    hu_lora_adapter_t *adapter;
    hu_ml_optimizer_t *optimizer;
    /* GRPO-2 degenerate-group v1: each preference pair becomes a
     * 2-rollout group with rewards (+1, -1). True multi-rollout
     * sampling lands in full-SOTA spec Phase 4. */
    size_t group_size;              /* fixed at 2 in v1; surface stays so Phase 4 can scale */
    float epsilon;                  /* PPO ratio clip; default 0.2 */
    float beta_kl;                  /* KL surrogate weight to π_θ_old; default 0.04 */
    float advantage_eps;            /* numerical floor for std; default 1e-8 */
} hu_grpo2_config_t;
hu_grpo2_config_t hu_grpo2_config_default(void);

/* ── Factories ──────────────────────────────────────────────── */

hu_error_t hu_rl_trainer_create_simpo(hu_allocator_t *alloc,
                                      const hu_simpo_config_t *cfg,
                                      hu_rl_trainer_t *out);
hu_error_t hu_rl_trainer_create_orpo(hu_allocator_t *alloc,
                                     const hu_orpo_config_t *cfg,
                                     hu_rl_trainer_t *out);
hu_error_t hu_rl_trainer_create_grpo2(hu_allocator_t *alloc,
                                      const hu_grpo2_config_t *cfg,
                                      hu_rl_trainer_t *out);

#endif /* HU_ML_RL_TRAINER_H */
```

Conformance with `docs/standards/engineering/naming.md`:

- `hu_rl_trainer_t` — public type, `hu_<noun>_t`. Matches `hu_provider_t`,
  `hu_memory_t`.
- `hu_rl_trainer_create_<algo>` — public factory, `hu_<module>_<action>`.
- `hu_<algo>_config_t` — config struct, `hu_<noun>_t`.
- `hu_<algo>_config_default()` — public function, snake_case verb.
- The loss-head files (`rl_simpo.c` etc.) **do not** publish any C symbols
  other than the factory + a static `vtable`. No DPO-style `hu_simpo_step()`
  is exposed — the vtable is the only entry point. This is the same
  discipline `hu_provider_t` implementations follow.

### 3.2 Why a vtable instead of three top-level functions

The spec's `hu_rl_trainer_vtable_t` is already established by
`2026-05-11-full-sota-rl-improvement-loop-design.md` §3.4. The decision was
debated there; we restate the conclusion in case a sprint reviewer questions
it:

1. **Init 05 (TTT) and Init 07 (ThinkPRM) need it.** Both want to call
   "the trainer," not "DPO specifically." Without the vtable they would
   `#ifdef HU_TRAINER_SIMPO ... #elif HU_TRAINER_ORPO ...` in `agent_turn.c`,
   violating the architecture-boundary rule (§3.3 of `principles.md`).
2. **Algorithm choice is a runtime decision.** A user with 500 preference
   pairs and a 4 GB peak-RSS budget picks SimPO. A user with 2,000 pairs
   and no SFT pass yet picks ORPO. A user with paired rollouts on a HuLa
   skill picks GRPO-2. Static dispatch would force a build-time choice.
3. **Backward-compat with `hu_dpo_judge_step` and `hu_dpo_real_step`.** The
   existing DPO surface stays. The new vtable is additive — no existing
   caller changes. This is the same pattern Phase 0 used for the
   `hu_dpo_train_step → hu_dpo_judge_step` rename (deprecation shim, not a
   replacement).

### 3.3 Why `step(...) → hu_train_telemetry_t *` instead of `→ double *out_loss`

The spec sketch used `step(...) → double *out_loss`. We propose upgrading to
`hu_train_telemetry_t *` because:

- **Honest naming carries through to telemetry.** Each algorithm has a
  meaningfully different "loss" — SimPO's length-normalized log-sigmoid,
  ORPO's NLL+λ·OR, GRPO-2's clipped PPO. Returning one float erases the
  distinction. The struct field `loss` is the unified scalar; the additional
  fields (`reward_margin`, `kl_to_ref`, `sft_loss`, `advantage_mean`) are
  the algorithm-specific diagnostics callers and dashboards already want
  to see (`ui/src/components/hu-fidelity-tile.ts` is in flight — see
  `git status`).
- **Forward-compatible with `hu_training_telemetry_c` (spec §4.8).** The
  spec's `src/ml/training_telemetry.c` writes step-level metrics to SQLite.
  Returning the struct, not a scalar, lets that file `memcpy` the result
  into its row builder without re-deriving anything.
- **Zero ABI cost.** It's a single pointer; the struct is on the caller's
  stack. We measured no binary-size impact relative to a `double *`.

The spec's prose still resolves correctly: "step → out telemetry that includes
loss" is a strict superset of "step → out_loss."

### 3.4 Loss math (one line per algorithm)

Notation: `x` = prompt, `yw` = chosen completion, `yl` = rejected completion,
`π_θ` = policy under training (base + LoRA), `π_ref` = frozen reference policy
(Phase 2 `hu_dpo_real_step` brings the real frozen-clone; SimPO and ORPO
need none; GRPO-2's reference is the same-policy snapshot at step start —
no separate model).

#### 3.4.1 SimPO (Meng et al., arXiv:2405.14734)

```
L_SimPO(x, yw, yl) = -log σ(  (β / |yw|) · log π_θ(yw|x)
                            - (β / |yl|) · log π_θ(yl|x)
                            - γ )
```

- **β** (default 2.0): inverse temperature.
- **γ** (default 1.0): target reward margin. Without it, SimPO degenerates
  to length-normalized DPO without reference.
- **Length normalization** (`length_norm=1.0` config flag): the `1/|y|`
  factor. Setting `length_norm=0.0` recovers a reference-free DPO with a
  target margin — useful for ablations.

Why we want it: **no reference model**, so peak RSS during training is
~half what real DPO needs. The Meng et al. paper reports SimPO matching or
beating DPO at fixed compute on AlpacaEval 2 and Arena-Hard. The cost is
one extra hyperparameter (γ) and slightly higher sensitivity to noisy
labels (mitigated by the same `--margin` filter `hu_dpo_collector_t` already
records).

#### 3.4.2 ORPO (Hong et al., arXiv:2403.07691)

```
log_odds(y|x) = log( π_θ(y|x) / (1 - π_θ(y|x)) )            -- clipped at ±odds_clip
L_OR        = -log σ( log_odds(yw|x) - log_odds(yl|x) )
L_SFT       = NLL_θ(yw|x)                                    -- standard cross-entropy
L_ORPO      = L_SFT + λ · L_OR
```

- **λ** (default 0.1): odds-ratio loss weight. Hong et al. §4.4 sweep:
  0.1 is the recommended value across all evaluated model scales.
- **odds_clip** (default 88.0): numerical safety. `log(π/(1-π))` overflows
  f32 when `π → 0` or `π → 1`; we clip log-odds at ±88 (≈ log(2³²)) before
  the sigmoid.
- **NLL** is the same per-token cross-entropy `hu_ml_train` already computes
  in `src/ml/train.c`. The trainer reuses `hu_ml_prepare_token_bytes`
  output and the existing per-token grad path; the OR term is a thin
  addition on top.

Why we want it: **SFT + preference in one pass**. Track D Phase 1 ships
banks-from-history SFT; ORPO eliminates the need for a separate `lora-persona`
pre-pass when preference pairs are already collected. One fewer pipeline
stage on-device.

#### 3.4.3 GRPO-2 (degenerate-group variant)

Standard GRPO (Shao et al., arXiv:2402.03300) samples G rollouts per prompt,
computes group-relative advantages `A_i = (r_i - mean_g(r)) / std_g(r)`, and
applies clipped-PPO with KL to a reference. Multi-rollout sampling against
`hu_provider_t.chat` requires Init 04 (live provider).

GRPO-2 in v1 collapses the rollout group to the already-collected `(yw, yl)`
preference pair with deterministic rewards `(+1, -1)`:

```
group     = { (yw, +1), (yl, -1) }
A_w       = (+1 - 0) / 1.0 = +1.0       -- mean=0, std=1 by construction (degenerate group)
A_l       = (-1 - 0) / 1.0 = -1.0
r_θ(y)    = exp( log π_θ(y|x) - log π_θ_old(y|x) )           -- importance ratio vs step-start snapshot
L_GRPO2(x, yw, yl) =  -E_g[ min( r_θ · A,  clip(r_θ, 1-ε, 1+ε) · A ) ]
                    + β_KL · D_KL( π_θ || π_θ_old )            -- reference-free KL surrogate
```

- **ε** (default 0.2): PPO ratio clip, matching trl's default.
- **β_KL** (default 0.04): KL penalty weight, matching DeepSeek-R1 default.
- `π_θ_old` is a **same-policy snapshot at step start**, not a separate
  frozen model — implementation reuses the existing optimizer's
  `set_training_progress` checkpoint hook (`hu_ml_optimizer_vtable_t`) to
  cache the LoRA A/B matrices before the step.
- `group_size` stays in the config struct (default 2) so Phase 4's real
  multi-rollout sampler can scale it without an API break.

Why we want it: **cheap surrogate** for the full GRPO objective that we
cannot yet run on-device. The clipped-PPO + KL machinery is the same
backbone Phase 4 will use; GRPO-2 lets us land the optimizer, the
advantage normalizer, and the KL surrogate **now** so Phase 4 only adds
the live-rollout sampler.

#### 3.4.4 Family comparison

For sprint-planning clarity:

| Trainer | Reference model? | SFT term? | Rollouts? | Length-norm? | Peak RSS during train (relative to DPO-real) |
|---|---|---|---|---|---|
| `hu_dpo_judge_step` (Phase 0) | judge LLM via provider | — | — | — | low (no on-device backward) |
| `hu_dpo_real_step` (spec Phase 2) | frozen π_ref clone | — | — | — | **1.00× (baseline)** |
| `hu_rl_trainer_create_simpo` | none | — | — | yes (configurable) | **~0.55×** |
| `hu_rl_trainer_create_orpo` | none | NLL on yw | — | — | **~0.60×** (NLL adds activation memory) |
| `hu_rl_trainer_create_grpo2` | snapshot-π_old (LoRA only) | — | yes (degenerate G=2 in v1) | — | **~0.70×** (snapshot is LoRA A/B only, ~MB) |
| KTO (spec Phase 3) | frozen π_ref clone | — | — | — | ~1.00× |
| SLiC (parked) | none | — | — | — | ~0.55× |

The three v1 trainers all sit **below** the projected baseline of
real DPO — which is the entire point.

### 3.5 Module structure (CMake + LOC)

```
include/human/ml/rl_trainer.h         (NEW,  ~140 LOC pure declarations)
src/ml/rl_trainer.c                   (NEW,  ~120 LOC: vtable dispatch + factory shells)
src/ml/rl_simpo.c                     (NEW,  ~280 LOC: simpo ctx + step + save + deinit)
src/ml/rl_orpo.c                      (NEW,  ~320 LOC: orpo ctx + step + save + deinit)
src/ml/rl_grpo2.c                     (NEW,  ~420 LOC: grpo2 ctx + snapshot + step + save + deinit)
src/ml/cli.c                          (MODIFY, +~120 LOC: hu_ml_cli_rl_train handler)
include/human/ml/cli.h                (MODIFY, +1 declaration)
src/app/main.c                            (MODIFY, +1 dispatch row in cmd_ml)
CMakeLists.txt                        (MODIFY, +4 source rows under HU_ENABLE_ML guard)

tests/test_rl_trainer.c               (NEW,  ~600 LOC: 10 tests across the three algos)
tests/fixtures/rl_pairs_tiny.json     (NEW,  ~40 LOC: 4 hand-rolled preference pairs)
tests/test_main.c                     (MODIFY, +1 declaration + 1 call)

fuzz/fuzz_rl_trainer_config.c         (NEW,  ~80 LOC: JSON fuzzer for the three configs)
fuzz/CMakeLists.txt                   (MODIFY, +1 harness target)

docs/standards/ai/evaluation.md       (MODIFY, +section: when to pick which trainer)
```

Total new C (excluding tests/fuzz): ~1,280 LOC — well inside the
"≤500 LOC per file" target (the largest, `rl_grpo2.c`, sits at ~420).

### 3.6 Data flow

```
human ml rl-train --algorithm=simpo --pairs=~/.human/dpo_pairs.jsonl --out=~/.human/adapters/simpo.lora
        │
        ▼
hu_ml_cli_rl_train (src/ml/cli.c)
    1. parse args, build per-algo config (hu_simpo_config_default + flag overrides)
    2. load pairs from JSONL (reuse hu_dpo_export_jsonl path inverted, see §4.2)
    3. derive token_bytes via hu_ml_prepare_load_default_tokenizer (Phase 0 shared helper)
    4. tokenize each pair into hu_pref_batch_t (one-time at prepare)
    5. hu_gpt_create + hu_lora_create + hu_muon_adamw_create (same as lora-persona)
    6. hu_rl_trainer_create_simpo(..., &trainer)
    7. trainer.vtable->prepare(trainer.ctx, &batch)
    8. for step=1..N:
         trainer.vtable->step(trainer.ctx, &batch, &telemetry)
         hu_ml_training_telemetry_record(&telemetry)            // spec §4.8
         log: step=N loss=X reward_margin=Y kl=Z
    9. trainer.vtable->save_adapter(trainer.ctx, --out)
   10. trainer.vtable->deinit(trainer.ctx)
        │
        ▼
~/.human/adapters/simpo.lora  (in-tree LORA format)
        │
        ▼  (post-init-04)
human ml lora-convert --to=mlx ~/.human/adapters/simpo.lora ~/.human/adapters/simpo.safetensors
        │
        ▼
hu_provider_load_adapter (existing W13 hot-swap)
```

The flow is **identical** to `lora-persona` from step 4 onward, with the
single difference being which loss-head fills `trainer.vtable`. This is
the architectural point of the vtable: the CLI knows nothing about the
algorithm, the algorithm knows nothing about the CLI.

---

## 4. CLI surface

### 4.1 `human ml rl-train`

Declared in `include/human/ml/cli.h`:

```c
/* Init 06 — RL preference-optimization training across the
 * hu_rl_trainer_t family. Pick an algorithm at runtime; the trainer
 * vtable does the rest. */
hu_error_t hu_ml_cli_rl_train(hu_allocator_t *alloc, int argc, const char **argv);
```

Dispatched from `cmd_ml` in `src/app/main.c` next to the existing
`dpo-train`, `lora-persona`, `lora-baseline`, `lora-ab` rows.

Flag table:

| Flag | Required | Default | Notes |
|---|---|---|---|
| `--algorithm` | yes | — | `simpo` / `orpo` / `grpo2`; rejects anything else with a clear error |
| `--pairs` | yes | — | JSONL path; format matches `hu_dpo_export_jsonl` output |
| `--out` | yes | — | Output adapter path; LORA format |
| `--steps` | no | `0` (use time budget) | Hard step cap |
| `--time-budget` | no | `300` sec | Wall-clock budget |
| `--rank` | no | `8` | LoRA rank |
| `--alpha` | no | `16.0` | LoRA alpha |
| `--beta` | no | per-algo default | SimPO β, GRPO-2 not used (use `--beta-kl`) |
| `--gamma` | no | `1.0` | SimPO only; ignored for others |
| `--lambda` | no | `0.1` | ORPO only |
| `--epsilon` | no | `0.2` | GRPO-2 only |
| `--beta-kl` | no | `0.04` | GRPO-2 only |
| `--group-size` | no | `2` | GRPO-2 only; locked at 2 in v1 (rejected if higher) |
| `--seed` | no | `12345` | PRNG seed for adapter init + any sampling |

Behavior on out-of-bounds: every numeric flag is bounds-checked at parse
time. Out-of-bounds returns `HU_ERR_INVALID_ARGUMENT` with a message
naming the offending flag and the accepted range. (Same convention as
`hu_ml_cli_dpo_train`.)

### 4.2 Pair-loading path

We reuse the existing `hu_dpo_export_jsonl` schema (one preference pair
per line, `{prompt, chosen, rejected, margin, source}`) so the user can
collect pairs with `hu_dpo_collector_t` (the existing path), then train
with any of the three new algorithms with no schema change. The reverse
direction (jsonl → `hu_preference_pair_t[]` in memory) lives in a new
private helper `src/ml/cli.c::load_preference_pairs_jsonl(...)`. ~50 LOC.

### 4.3 Telemetry

Every step writes one row to `hu_training_telemetry_record` (full-SOTA
spec §4.8). The recorded fields are exactly the `hu_train_telemetry_t`
shape. The dashboard tile `ui/src/components/hu-fidelity-tile.ts`
(in-flight, see `git status`) reads from there. We do not add a parallel
schema.

---

## 5. Test plan (deterministic, no network, no subprocess)

### 5.1 Deterministic golden loss tests (one per algorithm)

The single most important property of an RL trainer is **reproducible
loss for a fixed input**. Without it, regressions are invisible because
the loss looks "in range." We commit one golden test per algorithm:

| Test | Fixture | What it asserts |
|---|---|---|
| `test_rl_simpo_golden_loss` | 1-layer GPT, vocab=16, d_embd=16, batch=4 fixed pairs, seed=12345, defaults | `\|loss − GOLDEN_SIMPO\| < 1e-5` where `GOLDEN_SIMPO` is computed once at impl time and hard-coded |
| `test_rl_orpo_golden_loss` | same fixture | `\|loss − GOLDEN_ORPO\| < 1e-5` |
| `test_rl_grpo2_golden_loss` | same fixture | `\|loss − GOLDEN_GRPO2\| < 1e-5` |

The fixture's 4 preference pairs live at
`tests/fixtures/rl_pairs_tiny.json`. The model + LoRA + optimizer are
all initialized from `seed=12345`, the same seed `tests/test_ml.c`
already uses for its grad-check tests, so the per-token PRNG path is
byte-stable across runs and across machines (same xorshift LCG in
`gpt.c::prng_next`).

If a future implementer changes the PRNG or the LoRA init shape, **all
three** golden tests will fail in lockstep. That is the desired
regression signal: it forces a deliberate decision to re-bake the
goldens, not a silent drift.

### 5.2 Finite-difference gradient checks (one per algorithm)

Standard convention from `tests/test_ml.c::test_train_pipeline` — perturb
each LoRA A/B element by ±1e-4, recompute loss, compare numerical
gradient to analytic gradient.

```c
for each of {SimPO, ORPO, GRPO-2}:
    create trainer with seed=12345
    record analytic grad: forward → backward → snapshot adapter.grad_A, adapter.grad_B
    record numerical grad: for each (i, j): perturb A[i][j] by ±h, recompute loss
    assert max_abs_err(analytic, numerical) < 1e-3
```

Two-sided difference, `h = 1e-4`, only the first LoRA layer (cheap
enough to cover every element). Reuses the helper structure already in
`tests/test_ml.c` lines 583–653.

### 5.3 Vtable integration test (one per algorithm)

For each algorithm:

1. `create → prepare → step × 10 → save_adapter → deinit` happy path
   completes with HU_OK.
2. Loaded adapter via `hu_lora_load` matches what `save_adapter` wrote
   (round-trip).
3. `step` returns `HU_ERR_INVALID_ARGUMENT` when given an unprepared
   batch (chosen_tokens == NULL).
4. `step` returns `HU_ERR_INVALID_ARGUMENT` on NULL out-telemetry.
5. The `name(ctx)` string is the documented lowercase factory key
   (asserting we don't drift; this is the same discipline the channel
   factory uses).

### 5.4 libFuzzer harness

`fuzz/fuzz_rl_trainer_config.c`: parse a JSON config blob into one of
the three algorithm configs (selected by a magic byte at offset 0).
Asserts no UB / no OOB writes. Wired into the existing `fuzz/` build
rule pattern. ~80 LOC. Cost: zero — runs only when explicitly built
with the `fuzz` preset.

### 5.5 ASan + UBSan posture

Every new test runs under the existing AddressSanitizer build (the
`dev` preset). No new ASan suppressions. Every alloc routed through
`hu_allocator_t`, freed in the trainer's `deinit`. UBSan flags the
sigmoid overflow path if `odds_clip` ever fails — that's a real bug,
not a UBSan annoyance, so we keep UBSan on.

### 5.6 Test naming

Per `docs/standards/engineering/naming.md` — `subject_expected_behavior`:

```
test_rl_simpo_golden_loss
test_rl_simpo_grad_check_matches_finite_difference
test_rl_simpo_vtable_round_trip_save_load_preserves_weights
test_rl_simpo_step_rejects_unprepared_batch
test_rl_simpo_name_returns_lowercase_factory_key
test_rl_orpo_golden_loss
test_rl_orpo_grad_check_matches_finite_difference
test_rl_orpo_vtable_round_trip_save_load_preserves_weights
test_rl_orpo_clips_odds_at_configured_magnitude
test_rl_grpo2_golden_loss
test_rl_grpo2_grad_check_matches_finite_difference
test_rl_grpo2_vtable_round_trip_save_load_preserves_weights
test_rl_grpo2_advantage_normalization_handles_degenerate_group
test_rl_grpo2_ratio_clip_zeroes_grad_outside_epsilon_band
```

All wired in `tests/test_main.c::run_rl_trainer_tests()`.

---

## 6. Coexistence with the existing DPO surface

`hu_dpo_judge_step` (Phase 0 rename of `hu_dpo_train_step`) stays. It is
an LLM-as-judge eval harness, which is a useful thing for telemetry and
for collecting preference pairs at scale; it just is not what its old
name claimed.

`hu_dpo_real_step` (full-SOTA spec Phase 2) — when it lands — will
become a fourth member of the `hu_rl_trainer_t` family, behind a
`hu_rl_trainer_create_dpo_real(...)` factory. **We do not ship that
factory in this initiative.** We do reserve the factory key `"dpo"` for
it so Phase 2 doesn't need an API renegotiation.

Honest-naming carry-through from Phase 0:

- The new module is named `rl_trainer`, not `preference_optimizer`,
  because what it does is *gradient* on the LoRA, not external rating.
  Every function name uses `_step` / `_loss` / `_train` / `_create`.
- The deprecated `hu_dpo_train_step` shim stays untouched. No new shim
  is added in this initiative.

This satisfies the "honest naming" constraint in the task brief: every
public function name reflects loss-based optimization.

---

## 7. Risk register (top 3)

| # | Risk | Severity | Mitigation |
|---|---|---|---|
| 1 | **Numerical instability in ORPO's log-odds term.** `log(π / (1−π))` overflows f32 fast as π → 0 or π → 1. Without clipping, a single near-saturated token can produce a NaN that corrupts the entire batch's grad. | High | Hard clip `\|log_odds\| ≤ odds_clip` (default 88.0) **inside** the loss kernel, before the sigmoid; clip applies to both the log-odds and its gradient. Add `test_rl_orpo_clips_odds_at_configured_magnitude` (§5.6) that feeds a chosen token at probability 1.0 − 1e−30 and asserts loss is finite. UBSan-enabled CI catches accidental removal of the clip. |
| 2 | **GRPO-2 ratio clipping silently zeros all gradients.** If every token's importance ratio `r_θ` exits `[1−ε, 1+ε]`, the entire step's grad is zero — loss looks normal, no learning. This is a known PPO failure mode and is *especially* dangerous on small adapters where the early steps push ratios out of band fast. | High | Telemetry: `hu_train_telemetry_t.advantage_mean` and `advantage_std` are filled by GRPO-2, plus a derived `clip_fraction` metric (fraction of tokens with `\|r_θ − 1\| > ε`) recorded in the SQLite step-row. The trainer fails its step with `HU_ERR_INVALID_ARGUMENT` if `clip_fraction > 0.9` for three consecutive steps, surfacing the failure mode loudly. Test `test_rl_grpo2_ratio_clip_zeroes_grad_outside_epsilon_band` synthetically forces this case to lock the behavior. |
| 3 | **Binary-size budget (≤24 KB) blown by careless code duplication across the three loss heads.** Each loss head wants its own `_step`, `_save`, `_deinit` boilerplate; without discipline the trainer dispatcher grows three near-identical 400-line files. | Medium | The vtable dispatcher and the prepare/save/deinit boilerplate live in `src/ml/rl_trainer.c` exactly once (~120 LOC). Loss heads contain *only* their `step` implementation and their per-algo state. CI gate: `scripts/check-binary-size.sh` (existing) sets a +24 KB ceiling on the `release` preset between the merge-base and HEAD; the gate fails on regression. Measured ceiling assumes `HU_ENABLE_ML=ON`; the default `release` preset (ML off) sees +0 KB delta. |

Cross-cutting from full-SOTA spec §10 risks #5 (GRPO sensitivity) and #11
(Track D coordination) apply directly and are not duplicated here.

---

## 8. Binary budget

| Surface | Estimated KB (MinSizeRel + LTO, `HU_ENABLE_ML=ON`) | Note |
|---|---|---|
| `rl_trainer.c` (vtable dispatch + factory shells) | ~2 KB | All inline, no static tables |
| `rl_simpo.c` (loss head + ctx) | ~6 KB | Reference-free → smallest |
| `rl_orpo.c` (loss head + ctx + odds-ratio kernel) | ~7 KB | NLL term reuses `train.c`, odds adds ~3 KB |
| `rl_grpo2.c` (loss head + ctx + snapshot + advantage kernel + clip) | ~9 KB | Larger because of the snapshot machinery |
| **Total new code** | **~24 KB** | **At the budget ceiling.** |
| Header (`include/human/ml/rl_trainer.h`) | 0 KB | Pure declarations |
| CLI handler (`hu_ml_cli_rl_train` in `src/ml/cli.c`) | ~2 KB | Counted against `cli.c`'s existing line item; net cost ≈0 because we replace one of the existing growth-pressure flags with a single dispatch row |

Default `release` preset (ML disabled, the user-visible binary): **+0 KB.**
The trainer code is fully `#ifdef HU_ENABLE_ML`-guarded.

`rl_sota` preset (everything on): +24 KB. The full-SOTA spec already
budgets +250 KB for the entire RL Phase 2–5 surface; this initiative
consumes <10% of that budget.

Runtime RSS during training is bounded by the LoRA + optimizer state
(unchanged from `lora-persona`) plus per-algo deltas:

- SimPO: +0 KB (reference-free, no extra state)
- ORPO: ~16 KB transient (NLL activation buffer reuses `train.c`'s)
- GRPO-2: ~LoRA-size bytes (snapshot of A/B at step start) — for default
  rank=8 / 1-layer / d=64, that is ~4 KB. Production-scale (rank=8,
  32 layers, d=4096) reaches ~2 MB, still inside the "≤+250 KB peak RSS"
  spec budget because LoRA snapshot replaces nothing — it adds to the
  baseline.

---

## 9. Defer / descope condition

**Per-algorithm wall-clock gate.** Phase 2 of the full-SOTA spec lands
`hu_dpo_real_step` and establishes the on-device wall-clock baseline for
"one preference-pair step at batch=4 on the smallest reference fixture."
After that baseline is recorded:

- If a v1 implementation of **any one** of SimPO / ORPO / GRPO-2 takes
  more than **5× the DPO baseline wall-clock per step** at the same
  batch size on the same fixture, that specific algorithm is **parked**.
  The other two ship.
- Park = factory disabled at the dispatcher (`hu_rl_trainer_create_<algo>`
  returns `HU_ERR_NOT_SUPPORTED` with a docstring pointer back to this
  file), test cases skipped behind `#ifdef HU_RL_TRAINER_<ALGO>_ENABLED`,
  CLI flag rejects with a "parked: see init-06 §9" message.

The 5× ceiling is justified because:

- SimPO has structurally **less** compute than DPO (no reference forward).
  Exceeding 5× would mean an implementation bug, not a fundamental cost.
- ORPO does *one extra* per-pair odds-ratio compute. That's micro-cost.
  Exceeding 5× means the NLL pass is being recomputed instead of reused —
  again, an implementation bug.
- GRPO-2 has the snapshot copy, the ratio compute, the clip, and the
  KL surrogate. We expect ~2× DPO. 5× means the snapshot is not
  amortized across the step, which is the only way GRPO-2 could
  legitimately be that slow on-device.

In all three cases, the 5× ceiling is an *implementation-bug detector*,
not a soft-budget. Failing the gate means we ship the working two and
file a bug for the failing one.

**Whole-initiative descope.** If Init 04 (MLX Qwen3 provider) does not
land within two sprints of this initiative, GRPO-2 specifically becomes
academic — without a live provider, the degenerate-group approximation
is *all* we have, and the full multi-rollout GRPO is still gated on
Init 04 + spec Phase 4. We do not descope GRPO-2 because of this —
the degenerate variant is independently useful as a clipped-PPO loss
on existing pairs — but we explicitly mark in §11 that GRPO-2's
narrative value is at most 30% of "real" GRPO until Phase 4.

---

## 10. Build sequence (phased checklist for the implementation sprint)

| Phase | Work | Estimated time | Gate |
|---|---|---|---|
| **0** | Land the header (`include/human/ml/rl_trainer.h`) + the dispatcher (`src/ml/rl_trainer.c`) with all three factories returning `HU_ERR_NOT_SUPPORTED`. Wire `hu_ml_cli_rl_train` to dispatch. Land `tests/test_rl_trainer.c` with the vtable-shape tests that don't depend on a real impl. | 1 day | `spec-verifier` confirms 0 gaps vs §3.1 |
| **1** | SimPO (`src/ml/rl_simpo.c`). Land golden loss, finite-diff grad check, vtable round-trip. | 2 days | `verifier` runs the suite; golden value baked in commit message |
| **2** | ORPO (`src/ml/rl_orpo.c`). Same gates. Plus the odds-clip property test (§5.6). | 2 days | `critic` review for the log-odds numerical-safety story |
| **3** | GRPO-2 (`src/ml/rl_grpo2.c`). Same gates. Plus the advantage-degenerate-group test and the ratio-clip-zeroes-grad test. | 3 days | `aspect-panel` (correctness/edge-case/security/regression/style) at the end of Phase 3 — flagged in spec §7 as required for any GRPO-shaped work |
| **4** | CLI surface + libFuzzer harness + spec §4.8 telemetry wiring. | 1 day | Full `human_tests` green; `scripts/agent-preflight.sh` clean; binary-size CI green |
| **5** | Documentation + adversarial gates. Update `docs/standards/ai/evaluation.md` with "which trainer to pick" guidance. Run `dead-code-finder`, `sprint-auditor`. | 1 day | Both subagents PASS; merge |

Total: **~10 working days** of focused implementation, mirroring the
spec's "5–7 weeks for the whole 6-phase RL effort" budget — this
initiative occupies ~25% of one of those phases' compute.

Each phase commits independently; bisection works at every commit.

---

## 11. Open questions

| # | Question | Working answer |
|---|---|---|
| 1 | Should GRPO-2's degenerate-group rewards be configurable (e.g., margin-weighted)? | **No, in v1.** The whole point of the degenerate variant is that rewards are deterministically `(+1, −1)`. Once Phase 4 ships real rollouts, the reward is whatever the RM/verifier returns. Adding a knob now risks users hand-tuning rewards in a way that doesn't carry forward. |
| 2 | Should the trainer own the LoRA adapter, or accept it caller-owned (current §3.1 design)? | **Caller-owned.** Same pattern as `hu_lora_register_params` to an existing optimizer. Trainer never frees the adapter. Keeps testability clean — tests can inspect adapter weights before and after `step()`. |
| 3 | Do we need a `step_batch(N)` for amortizing setup cost? | **Not in v1.** The dispatcher's `step()` is already batch-shaped (`hu_pref_batch_t`). If profiling shows per-call dispatch is the bottleneck, we add it; in v1 the LoRA kernels dominate. |
| 4 | Is SimPO's γ a fixed scalar or a per-pair-margin-derived value? | **Fixed scalar in v1** (matching Meng et al.). Per-pair γ derived from `hu_preference_pair_t.margin` is interesting and easy to add later; not in scope here. |
| 5 | Should ORPO's λ be annealed (start high, decay to standard preference-only late in training)? | **No.** Hong et al. §4.4 sweep is constant-λ; we follow. Annealing is the kind of speculative configurability AGENTS.md §3 explicitly warns against. |
| 6 | **(Single biggest open question.) For GRPO-2's KL surrogate, should `π_θ_old` be the per-step snapshot (current design) or a periodic checkpoint (every K steps)?** | Current design: per-step snapshot, like trl's `grpo_trainer.py`. The risk is that on small adapters with high learning rate, per-step snapshot gives near-zero KL and the surrogate term becomes a no-op. A periodic-K snapshot gives a more meaningful KL but increases memory by K× the LoRA size. **Decision punted to the implementer** — they will pick K based on early-step KL telemetry from a smoke run. The vtable does not need to change either way; the snapshot policy is internal to `rl_grpo2.c`. |

---

## 12. Proof bar status (D0–D7)

| Gate | Requirement | Status |
|---|---|---|
| **D0** | This file exists at the canonical path with YAML frontmatter (title, created, status, related) | ✅ This file |
| **D1** | Maps to one or more `include/human/*.h` vtable additions; public functions named per `docs/standards/engineering/naming.md` | ✅ `include/human/ml/rl_trainer.h` (§3.1); names audited against `naming.md` in §3.1 |
| **D2** | File list with LOC estimates | ✅ §3.5 table |
| **D3** | Test plan: ≥1 deterministic unit, ≥1 integration, optional fuzzer | ✅ §5 — 3 golden + 3 grad-check + 3 vtable round-trips + property tests + libFuzzer harness |
| **D4** | Top 3 risks with mitigations | ✅ §7 |
| **D5** | ≥2 arXiv refs with IDs | ✅ Frontmatter `arxiv:` list (SimPO 2405.14734, ORPO 2403.07691, KTO 2402.01306, DPO 2305.18290, GRPO 2402.03300, SLiC 2305.10425) |
| **D6** | Binary-budget delta | ✅ §8 — +24 KB at ceiling under `rl_sota`, +0 KB on default `release` |
| **D7** | Defer / descope condition | ✅ §9 — per-algorithm 5× DPO wall-clock gate + whole-initiative narrative-value note |

---

## 13. Coordination notes (cross-initiative)

- **Init 04 (MLX Qwen3 provider):** v1 of this initiative trains CPU-only.
  Post-Init-04, `hu_rl_trainer_create_*` will gain an optional
  `provider` field on each config (for the multi-rollout case). Forward
  compat is preserved by adding a new field at struct end, not
  modifying existing ones.
- **Init 05 (Verifier-Driven TTT):** Init 05's TTT step will call
  `trainer->vtable->step(...)` against a SimPO or ORPO trainer it owns.
  No coordination needed beyond agreeing on the vtable shape — this
  initiative locks that shape.
- **Init 07 (ThinkPRM):** ThinkPRM is the *trained* reward model
  (`hu_reward_model_t`). It is consumed by Phase 4's real GRPO. v1
  GRPO-2 uses deterministic rewards and does not depend on Init 07.
- **Full-SOTA spec Phase 2 (`hu_dpo_real_step`):** The spec adds DPO as
  its own top-level function. Phase 2 may, but is not required to,
  wire it behind `hu_rl_trainer_create_dpo_real(...)`. This initiative
  reserves the `"dpo"` factory key. If Phase 2 chooses not to wire
  through the vtable, no harm — the two surfaces coexist.
- **Track D Phase 1 (Banks-from-history):** The SFT data pipeline
  feeds *into* this initiative when the user picks ORPO (since ORPO
  bakes SFT into the preference pass). No coupling change needed — the
  banks-from-history JSONL is one of two acceptable inputs to
  `--pairs` (the other being `hu_dpo_export_jsonl` output).

---

## 14. References

See frontmatter `arxiv:` block. Quick recap:

- **SimPO** — Meng, Y. et al., *SimPO: Simple Preference Optimization
  with a Reference-Free Reward*, arXiv:2405.14734 (May 2024). Equation
  used in §3.4.1.
- **ORPO** — Hong, J. et al., *ORPO: Monolithic Preference Optimization
  without Reference Model*, arXiv:2403.07691 (Mar 2024). Equation used
  in §3.4.2. λ default from §4.4 of the paper.
- **DPO** — Rafailov, R. et al., *Direct Preference Optimization*,
  arXiv:2305.18290 (NeurIPS 2023). The baseline this initiative does
  **not** replace.
- **GRPO** — Shao, Z. et al., *DeepSeekMath: Pushing the Limits of
  Mathematical Reasoning*, arXiv:2402.03300 (Feb 2024). The basis for
  GRPO-2's clipped-PPO + group-relative-advantage form in §3.4.3.
- **GRPO-2** — to be cited at implementation kickoff. The April 2026
  preprint extending GRPO with cheaper rollout groups is referenced in
  the spec; we lock the loss form in §3.4.3 rather than waiting for
  citation finality.
- **KTO** — Ethayarajh, K. et al., *KTO: Model Alignment as Prospect
  Theoretic Optimization*, arXiv:2402.01306. Family-comparison only;
  full-SOTA spec Phase 3 ships it.
- **SLiC-HF** — Zhao, Y. et al., *SLiC-HF*, arXiv:2305.10425. Family-
  comparison only; parked.

Internal:

- `docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md` —
  the spec this initiative implements within.
- `docs/plans/2026-05-11-rl-loop-phase-0-honesty.md` — the rename
  precedent this initiative inherits.
- `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md` — the inference
  side of the loop.
- `docs/standards/engineering/naming.md` — names audited against this.
- `docs/standards/engineering/principles.md` — architecture-boundary
  rule applied to vtable choice (§3.2).
- `docs/standards/engineering/performance.md` — binary-budget ceiling
  applied to §8.

---

**End of design. Status: D0–D7 satisfied. Ready for sprint planning.**
