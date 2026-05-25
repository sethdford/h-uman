---
title: "Init #02 — MoLoRA per-channel persona routing"
created: 2026-05-11
status: deferred
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-10-m3-frontier-model-bridge.md
  - 2026-05-10-w14-sleep-compute.md
  - 2026-05-10-master-follow-through-program.md
  - 2026-05-11-init-04-mlx-qwen3-provider.md
  - 2026-05-11-init-05-verifier-driven-ttt.md
  - 2026-05-11-init-06-simpo-orpo-grpo2.md
  - ../standards/engineering/principles.md
  - ../standards/engineering/naming.md
  - ../standards/security/threat-model.md
risk: medium
binary_budget_kb: 8
last_audit: 2026-05-25
---

# Init #02 — MoLoRA per-channel persona routing

> One base on-device model + 4–8 tiny LoRA experts (one per channel + one per
> persona macro-mode), gated by a learned ≤16K-param MLP router at inference
> time. Replaces the current "one LoRA per persona" assumption with a sparse
> mixture-of-experts that lets Telegram-casual, iMessage-warm, Slack-terse, and
> the always-on persona macro-mode all live as **separate, small** adapters on
> top of a single quantized base.

## Honest one-paragraph thesis

Today the persona system has one structural style knob per channel
(`hu_persona_overlay_t`) and exactly one LoRA artefact (the W13 daemon
auto-loads a single `personalization.lora_adapter_path`). That works while
"one persona = one adapter", but it forces every channel through the same
weight set: an adapter tuned on a Telegram-casual bank silently bleeds into
the iMessage thread with mom and the work-Slack thread with the on-call
engineer. The fix that has converged in the April–September 2026 arXiv
literature is **mixture-of-LoRA-experts (MoLoRA)** — N small adapters trained
independently, mixed at inference time by a tiny learned router. We adopt the
*static-per-turn* slice of that idea: pick a sparse set of 1–3 experts per
turn at the level of (channel, message_class, persona_macro_mode), apply
them via the modern llama.cpp `llama_set_adapters_lora(ctx, A[], n, w[])`
array hook, and reuse the W14 idle scheduler to train the router offline. The
expensive token-level differentiable routing (LD-MoLE) and the layer-level
hybrid-entropy routing (DynMoLE) stay deferred behind the same single seam.

## Glossary (precision matters here)

| Term | Definition |
|------|-----------|
| **Base** | The quantized open-weight model loaded once (Qwen3-4B, Llama-3-8B-q4, etc.) — initiative #04 owns the MLX path; this initiative is provider-agnostic. |
| **Expert** | One LoRA adapter (rank 8–16, Q/V default targets) trained on a *single* (channel) or (macro-mode) bank. 4–16 MB on disk per expert. |
| **Slot** | A logical id in [0..7] mapped to one expert at runtime. Slot 0 is reserved for the persona macro-mode adapter (always-on baseline); slots 1..6 are channel experts; slot 7 is reserved for initiative #05 verifier-driven TTT. |
| **Macro-mode** | A persona-wide *mode* — `default`, `crisis`, `playful`, `analytical`, etc. Persona-derived, channel-independent. |
| **Message class** | Heuristic-tagged at agent turn entry: `ack`, `chitchat`, `question`, `emotional`, `task`, `crisis`. Reuses the `hu_cognitive_tier_t` ladder from `model_router.h`. |
| **Router** | The learned MLP that maps `(channel_id, message_class, macro_mode)` → softmax over slots → top-k sparse mixture. |
| **Mixture** | The runtime selection: a vector of `(slot, weight)` pairs with at most `HU_MOLORA_MAX_ACTIVE = 3` entries summing to 1.0. |

## What changes in the user-visible product

| Before | After |
|--------|-------|
| `personalization.lora_adapter_path` = ONE file, applied to every turn on every channel. | `personalization.molora.experts_dir` = a directory of N adapters + `router.bin`. Daemon picks 1–3 per turn. |
| Telegram casual style bleeds into iMessage warm thread with mom. | Per-channel expert dominates the mixture on its own channel; macro-mode adapter stays as a 30–50% floor for persona continuity. |
| Adding a new channel's tone requires retraining the single persona LoRA from a unified bank — expensive, and the existing channels degrade. | New channel = new expert = additive, isolated training; existing channels' weights unchanged. |
| No A/B knob below the persona level — can't compare "casual-Telegram" vs "warm-Telegram" without two whole personas. | Each expert can have a vs-N variant; the router treats them as siblings during eval. |

# 1 · Architectural decision

## 1.1 Why a new vtable hook, not a wider `hu_provider_load_adapter`

The brief lists two options:

- **(A)** Extend `hu_provider_load_adapter` to accept an *array* of paths + a router blob.
- **(B)** Introduce a new optional `hu_provider_load_adapter_mixture(...)`.

**We pick (B). Justification:**

1. **Stable contract.** `hu_provider_load_adapter` is declared optional in `include/human/provider.h:252` and *six* call sites already depend on its current shape: the W13 daemon auto-load (`src/daemon.c`), the W14 lora-runner adapter hot-swap (`src/agent/lora_training_runner.c`), the `lora-runner` CLI hot-load path, the huml provider implementation, the llamacpp provider implementation, and `tests/test_provider_all.c::test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`. Widening its signature breaks every cloud provider whose `load_adapter` is `NULL` today (they'd silently start returning `HU_ERR_INVALID_ARGUMENT` on a malformed array).
2. **KISS / explicit `#ifdef` over hidden dynamic behavior** (`docs/standards/engineering/principles.md` §KISS). One adapter and N adapters are different concepts; conflating them in a `count == 1 ? legacy : mixture` switch buries the semantics.
3. **YAGNI for cloud providers.** OpenAI / Anthropic / Gemini will *never* support mixture-of-LoRA. Their entries stay NULL on the new hook. Single-adapter providers (e.g. early MLX from #04) can leave the mixture hook NULL until they're ready and the caller transparently falls back to single-adapter via the existing helper. The principle "keep unsupported paths explicit (`HU_ERR_NOT_SUPPORTED`)" applies cleanly.
4. **Backward-compatible composition.** The single-adapter path keeps working unchanged for everyone — initiatives #04 and #05 can land their first slice without ever touching the mixture seam.
5. **Modern llama.cpp already takes an array.** `llama_set_adapters_lora(ctx, struct llama_adapter_lora **adapters, int n, float *scales)` (b3000+) is *natively* a mixture call. The single-adapter `hu_provider_load_adapter` wraps it as `n=1, scale=1.0`; the mixture hook surfaces the full API. Choosing (A) would force the wrapper to fan out a one-shot to N — same code, more pretence.

The shape:

```c
/* include/human/provider.h — added next to the existing W13 triple. */

/* MoLoRA Phase 1 — Mixture-of-LoRA-Experts adapter binding.
 *
 * Providers that support **multiple resident adapters with weighted
 * mixing** implement the triple below. The single-adapter triple
 * (load_adapter / unload_adapter / active_adapter) stays untouched
 * and remains the only path for cloud providers and the early MLX
 * provider in initiative #04.
 *
 * Contract (all three or none — same convention as the W13 triple):
 *
 *   load_adapter_mixture: bind all N adapters into the provider's
 *                         resident pool. `slots` is the logical id
 *                         array; one of the slots MUST be 0
 *                         (the always-on macro-mode baseline).
 *                         Caller may unload via unload_adapter on
 *                         each adapter_id, OR call this again with
 *                         the new set (semantics: full replace).
 *   set_adapter_mixture:  hot-swap the **active mixture** on the
 *                         provider's context. Each call replaces
 *                         the previous active set atomically.
 *                         `weights[i]` MUST be in [0.0, 1.0]; sum
 *                         is NOT required to be 1.0 (provider may
 *                         normalize or pass through verbatim).
 *                         n == 0 means "deactivate all" — falls
 *                         back to the base.
 *   active_mixture:       fills `out_slots[]` / `out_weights[]` /
 *                         `*out_n` with the current selection.
 *                         Caller-owned arrays of length
 *                         HU_MOLORA_MAX_ACTIVE. */

#define HU_MOLORA_MAX_ACTIVE 3
#define HU_MOLORA_MAX_SLOTS  8

typedef struct hu_lora_mixture_adapter {
    uint8_t slot;                /* logical slot id [0..HU_MOLORA_MAX_SLOTS-1] */
    const char *adapter_path;    /* path to .lora / .gguf-lora */
    size_t adapter_path_len;
    const char *adapter_id;      /* opaque label, persona-derived */
    size_t adapter_id_len;
} hu_lora_mixture_adapter_t;

hu_error_t (*load_adapter_mixture)(void *ctx, hu_allocator_t *alloc,
                                   const hu_lora_mixture_adapter_t *adapters,
                                   size_t adapter_count);

hu_error_t (*set_adapter_mixture)(void *ctx,
                                  const uint8_t *slots, const float *weights,
                                  size_t n);

hu_error_t (*active_mixture)(void *ctx,
                             uint8_t *out_slots, float *out_weights,
                             size_t *out_n);
```

Three small public helper wrappers mirror the W13 helpers (`hu_provider_load_adapter_mixture` / `_set_adapter_mixture` / `_active_mixture`), each returning `HU_ERR_NOT_SUPPORTED` when the vtable method is NULL. Same shape as `hu_provider_load_adapter` today.

## 1.2 Why a learned MLP router, not heuristics

We *already* have a heuristic router (`include/human/agent/model_router.h`,
9 enum tiers + keyword/score classifier + LLM-as-judge fallback). It picks
*models*, not adapters, but the structure is the same. We deliberately do NOT
reuse it for expert selection:

1. **Different objective.** The model router optimizes for cost vs.
   capability ("send this to flash-lite or to pro"). The expert router
   optimizes for *style fidelity* ("which of these 4 LoRA stamps most matches
   the target casual-Telegram distribution?"). The features overlap (channel,
   message class) but the labels don't.
2. **Trainability matters.** The LoRA router has a ground-truth signal we can
   collect cheaply: for each historical turn, score each expert via the
   existing `hu_communication_style_fidelity_score` (already wired into
   `human ml lora-baseline` and `lora-ab`). The argmax-fidelity expert per
   turn is the supervision label. Heuristic rules can't beat that signal once
   we have ≥1K labelled turns per channel.
3. **Sparsity is cheap at this scale.** ≤16K params × FP32 = 64 KB max on
   disk, ≤4.3 KB at the spec'd size below. The forward pass is one matrix-
   vector multiply per turn (sub-µs even on a Cortex-A53).

**Spec'd router architecture** (≤16K params, 4.3 KB FP32):

```
Input  (24 floats)                 = channel_onehot[8]
                                   ⊕ message_class_onehot[8]
                                   ⊕ macro_mode_onehot[8]
Hidden (32 ReLU)                   = W1 (24 × 32) + b1 (32)        →  800 params
Output (HU_MOLORA_MAX_SLOTS = 8)   = W2 (32 × 8)  + b2 (8)         →  264 params
                                                    Total: 1064 floats = 4.3 KB FP32
```

Output is softmax over 8 slots; we then take **top-`HU_MOLORA_MAX_ACTIVE` = 3**
and renormalize. Slot 0 (macro-mode) is *floored* to `expert_weight_floor` (default 0.2) so the persona macro-mode adapter is always in the mixture — this is the persona-continuity guarantee that prevents the channel expert from drifting the agent off-persona.

We pick MLP over Transformer over linear because:

- Linear is *too* small (LD-MoLE arXiv:2509.25684 §4.2 found one hidden layer is the cheapest model that produces non-trivial channel separation).
- Transformer routing over tokens (DynMoLE arXiv:2504.00661, LD-MoLE) is the right *future* shape but blows the 8 KB budget and forces the router into the same allocator/precision regime as the base, which our CPU-default provider can't take on.

## 1.3 Decision summary

| Question | Decision | Rationale |
|---------|----------|-----------|
| Wide `load_adapter` or new mixture hook? | **New mixture hook** (B). | Preserves the stable single-adapter contract; KISS; cloud providers stay NULL. |
| Router model class? | **Tiny MLP, 1 hidden layer of 32, ReLU.** | 4.3 KB params, sub-µs forward. |
| Routing granularity? | **Per-turn, sparse top-3.** | Per-token (LD-MoLE) deferred to a future initiative. |
| Slot 0 reserved? | **Yes** — persona macro-mode baseline. | Persona continuity floor; coexists cleanly with #05 TTT (TTT writes slot 7). |
| Where does the router live? | **`src/persona/lora_router.c`**. | Persona system already owns overlay selection (`hu_persona_find_overlay`); the router is the same shape one rung deeper. |
| When is the router trained? | **Offline, W14 idle job (`HU_JOB_MOLORA_ROUTER_TRAIN`).** | Inference must remain pure-forward. Reuses the existing scheduler. |

# 2 · Component design

## 2.1 New types (`include/human/persona/lora_router.h`)

```c
/* New header. ~85 LOC. */

#define HU_LORA_ROUTER_INPUT_DIM   24    /* 8 channel + 8 msg-class + 8 macro-mode */
#define HU_LORA_ROUTER_HIDDEN_DIM  32
#define HU_LORA_ROUTER_OUTPUT_DIM  HU_MOLORA_MAX_SLOTS   /* 8 */
#define HU_LORA_ROUTER_PARAM_COUNT (24 * 32 + 32 + 32 * 8 + 8)  /* 1064 */

/* On-disk magic for router blobs. Same shape as HU_M3_ADAPTER_MAGIC. */
#define HU_LORA_ROUTER_MAGIC "HU_MLRT\x01"

typedef enum hu_message_class {
    HU_MSG_CLASS_ACK = 0,
    HU_MSG_CLASS_CHITCHAT,
    HU_MSG_CLASS_QUESTION,
    HU_MSG_CLASS_EMOTIONAL,
    HU_MSG_CLASS_TASK,
    HU_MSG_CLASS_CRISIS,
    HU_MSG_CLASS_OTHER,
    HU_MSG_CLASS_MAX = 8   /* keep stable for one-hot dim */
} hu_message_class_t;

typedef enum hu_persona_macro_mode {
    HU_PERSONA_MODE_DEFAULT = 0,
    HU_PERSONA_MODE_PLAYFUL,
    HU_PERSONA_MODE_ANALYTICAL,
    HU_PERSONA_MODE_VULNERABLE,
    HU_PERSONA_MODE_CRISIS,
    HU_PERSONA_MODE_PROFESSIONAL,
    HU_PERSONA_MODE_OTHER,
    HU_PERSONA_MODE_MAX = 8
} hu_persona_macro_mode_t;

/* Routing context. Deliberately NOT promoted to a generic
 * `hu_request_ctx_t` (YAGNI — there is exactly one consumer today).
 * If init #05 / #07 grow a need we'll promote then. */
typedef struct hu_lora_router_ctx {
    uint8_t channel_id;          /* canonical: see hu_lora_router_channel_id() */
    hu_message_class_t message_class;
    hu_persona_macro_mode_t macro_mode;

    /* Persona overlay-derived overrides (zero-init = no override). */
    uint8_t preferred_slot;       /* from hu_persona_overlay_t.expert_id; 0 means "none" — i.e. slot 0 is implicit, you cannot pin slot 0 explicitly (that's already the macro floor). */
    float   preferred_weight_floor;  /* from hu_persona_overlay_t.expert_weight_floor; clamped [0,1]. */
} hu_lora_router_ctx_t;

typedef struct hu_lora_mixture {
    uint8_t slots  [HU_MOLORA_MAX_ACTIVE];
    float   weights[HU_MOLORA_MAX_ACTIVE];
    size_t  n;                   /* 0 = base only (router refused) */
} hu_lora_mixture_t;

typedef struct hu_lora_router hu_lora_router_t;

/* Lifecycle. The router is small enough that we keep the entire
 * weight set resident on the heap; no mmap/lazy load. */
hu_error_t hu_lora_router_open   (hu_allocator_t *alloc, const char *path, size_t path_len,
                                  hu_lora_router_t **out);
hu_error_t hu_lora_router_open_default (hu_allocator_t *alloc, hu_lora_router_t **out);  /* zero-weights uniform router */
hu_error_t hu_lora_router_save   (const hu_lora_router_t *r, const char *path, size_t path_len);
void       hu_lora_router_close  (hu_allocator_t *alloc, hu_lora_router_t *r);

/* Inference — pure, no allocation, no I/O. ≤ 5 µs on M-series.
 * Always succeeds: on null inputs or unconfigured router returns
 * a degenerate mixture with n=1, slot=0, weight=1.0. */
int hu_lora_router_select(const hu_lora_router_t *r,
                          const hu_lora_router_ctx_t *ctx,
                          hu_lora_mixture_t *out);

/* Canonical channel id mapping. Public so trainers/tests share it. */
uint8_t hu_lora_router_channel_id(const char *channel, size_t channel_len);
hu_message_class_t hu_lora_router_classify_message(const char *msg, size_t msg_len);

/* Training (used by the W14 runner). Lives in same file for cache
 * locality of router constants. Backprops through softmax-CE
 * against `target_slot` for each labelled (ctx, slot) pair. */
typedef struct hu_lora_router_train_config {
    float lr;             /* default 0.05 — tiny model, no Adam needed */
    int   epochs;         /* default 4 */
    int   batch_size;     /* default 64 */
    int   budget_ms;      /* enforced by the runner, not by the trainer */
} hu_lora_router_train_config_t;

typedef struct hu_lora_router_train_sample {
    hu_lora_router_ctx_t ctx;
    uint8_t              target_slot;     /* argmax-fidelity slot */
    float                weight;          /* per-sample down-weight; 0 = drop */
} hu_lora_router_train_sample_t;

hu_error_t hu_lora_router_train(hu_lora_router_t *r,
                                const hu_lora_router_train_sample_t *samples,
                                size_t n_samples,
                                const hu_lora_router_train_config_t *cfg,
                                int64_t budget_ms);
```

## 2.2 New types (`include/human/persona.h`, additive only)

Two new fields on `hu_persona_overlay_t`. Backward-compatible because zero-init
means "no preference, router decides" — exactly the spec'd contract:

```c
typedef struct hu_persona_overlay {
    /* ... existing 14 fields unchanged ... */

    /* MoLoRA per-channel routing (init #02).
     *
     * `expert_id` (0 = no preference): when non-zero, the router floors
     * the weight of slot `expert_id` to `expert_weight_floor` (clamped
     * [0,1]) before top-k selection. Lets an operator pin "this channel
     * always uses at least 40 % of the iMessage expert" even when the
     * router disagrees, without bypassing the router entirely.
     * Slot 0 is always the persona macro-mode and cannot be the
     * channel preferred slot (the macro floor lives on its own knob).
     */
    uint8_t expert_id;
    float   expert_weight_floor;
} hu_persona_overlay_t;
```

Zero-init is the existing default for every loader path
(`hu_persona_load_json`, `hu_persona_examples_load_json`, every test
fixture). The persona JSON gains two optional fields under each overlay; absence is the documented default. Validator unchanged.

## 2.3 New vtable members (`include/human/provider.h`, additive only)

See §1.1 for the full surface. Adds 3 function pointers (24 bytes on 64-bit)
to `hu_provider_vtable_t`. Existing providers set them to NULL → helpers
return `HU_ERR_NOT_SUPPORTED`, callers fall back to single-adapter.

## 2.4 New job kind (`include/human/agent/scheduler.h`)

```c
typedef enum hu_job_kind {
    /* ... existing entries unchanged ... */
    HU_JOB_MOLORA_ROUTER_TRAIN,   /* NEW: train hu_lora_router_t offline */
    HU_JOB_KIND_MAX
} hu_job_kind_t;
```

Adding a value to a non-`MAX` slot in `hu_job_kind_t` is a stable
enum extension: SQLite `scheduler_jobs.kind` is an `INTEGER`, the
`MAX` sentinel grows by 1, all existing rows keep their meaning. New
runner at `src/persona/molora_runner.c`, registered in
`hu_persona_subsystem_init` next to `hu_scheduler_register_runner`.

## 2.5 New config struct (`include/human/config_types.h`)

```c
/* Init #02 — MoLoRA per-channel routing. Lives next to (not inside)
 * `hu_personalization_config_t` so the legacy single-adapter path can
 * coexist for one release cycle. */
typedef struct hu_molora_config {
    bool enabled;                  /* default false */
    char *experts_dir;             /* default "~/.human/molora/" */
    char *router_path;             /* default "<experts_dir>/router.bin" */
    char *manifest_path;           /* default "<experts_dir>/manifest.json" */
    uint8_t  active_slot_cap;      /* default 3 (HU_MOLORA_MAX_ACTIVE) */
    float    macro_mode_floor;     /* default 0.2 — slot-0 weight floor */
    bool     train_router_on_idle; /* default true */
    int      train_min_samples;    /* default 256 (don't train on noise) */
} hu_molora_config_t;
```

The manifest JSON shape (one file per persona, in `experts_dir/manifest.json`):

```jsonc
{
  "schema_version": 1,
  "persona": "seth",
  "experts": [
    {"slot": 0, "kind": "macro_mode",  "mode": "default",      "path": "macro_default.lora",  "trained_at": 1715... },
    {"slot": 1, "kind": "channel",     "channel": "telegram",  "path": "ch_telegram.lora",    "trained_at": 1715... },
    {"slot": 2, "kind": "channel",     "channel": "imessage",  "path": "ch_imessage.lora",    "trained_at": 1715... },
    {"slot": 3, "kind": "channel",     "channel": "slack",     "path": "ch_slack.lora",       "trained_at": 1715... },
    {"slot": 4, "kind": "channel",     "channel": "discord",   "path": "ch_discord.lora",     "trained_at": 1715... },
    {"slot": 7, "kind": "ttt_scratch", "channel": "*",          "path": null,                 "owned_by": "init-05" }
  ]
}
```

The manifest is the only file the agent reads at startup; experts are
opened lazily on first use (and held resident for the daemon's lifetime —
mixing requires every active slot to be already loaded).

## 2.6 Implementation file map (full list, with LOC estimates)

| # | File | New? | ~LOC | Purpose |
|---|------|------|------|---------|
| F1  | `include/human/persona/lora_router.h`             | **new** | 85   | Public router types + API |
| F2  | `src/persona/lora_router.c`                       | **new** | 380  | Router forward/backward, save/load, channel-id mapping, message-class classifier |
| F3  | `tests/test_lora_router.c`                        | **new** | 220  | Unit + property tests (see §6) |
| F4  | `include/human/persona.h`                         | modify  | +12  | Add `expert_id` + `expert_weight_floor` to `hu_persona_overlay_t` (already-cited fields in brief) |
| F5  | `src/persona/persona.c`                           | modify  | +30  | Parse `expert_id` / `expert_weight_floor` from overlay JSON; zero-init in `hu_persona_overlay_load_defaults` |
| F6  | `include/human/provider.h`                        | modify  | +90  | Add `HU_MOLORA_MAX_ACTIVE`, `HU_MOLORA_MAX_SLOTS`, `hu_lora_mixture_adapter_t`, 3 vtable methods, 3 helper prototypes |
| F7  | `src/providers/helpers.c`                         | modify  | +60  | Implement `hu_provider_load_adapter_mixture` / `_set_adapter_mixture` / `_active_mixture` wrappers (NOT_SUPPORTED on NULL vtable) |
| F8  | `src/providers/llamacpp.c`                        | modify  | +140 | Implement the three mixture methods on top of `llama_adapter_lora_init` (already used for single) + `llama_set_adapters_lora(ctx, A[], n, w[])` (modern API natively takes an array). |
| F9  | `tests/test_provider_all.c`                       | modify  | +60  | Extend NOT_SUPPORTED dispatcher pack to cover the 3 new helpers across all cloud providers |
| F10 | `tests/test_llamacpp_provider.c`                  | modify  | +90  | Mixture load/set/active happy + null-arg + slot-0-missing path |
| F11 | `include/human/persona/molora.h`                  | **new** | 95   | Manifest types + `hu_molora_load_manifest` + `hu_molora_apply_to_provider` |
| F12 | `src/persona/molora.c`                            | **new** | 260  | Manifest parser/serializer, expert resolution against `hu_provider_load_adapter_mixture`, hot-path `hu_molora_route_and_apply(agent, ctx)` |
| F13 | `tests/test_molora_manifest.c`                    | **new** | 180  | Manifest parse/serialize + 4-expert load happy path against the llamacpp stub |
| F14 | `include/human/config_types.h`                    | modify  | +20  | Add `hu_molora_config_t`; embed pointer in agent config |
| F15 | `src/config_parse.c` + `src/config_serialize.c` + `src/config_merge.c` | modify | +80 | `molora` block parser, serializer, defaults |
| F16 | `tests/test_config_parse.c`                       | modify  | +60  | `test_config_parses_molora_block` + disabled-by-default test |
| F17 | `include/human/agent/scheduler.h`                 | modify  | +3   | `HU_JOB_MOLORA_ROUTER_TRAIN` enum entry |
| F18 | `src/persona/molora_runner.c`                     | **new** | 220  | W14 idle runner: pull labelled samples from history → call `hu_lora_router_train` → save → bump scheduler counter |
| F19 | `tests/test_molora_runner.c`                      | **new** | 160  | Runner happy path + budget-respected + min-samples gate |
| F20 | `src/agent/agent_turn.c`                          | modify  | +30  | Compute `hu_lora_router_ctx_t` per turn; call `hu_molora_route_and_apply` before the provider call (no-op when `molora.enabled == false`) |
| F21 | `src/agent/agent_stream.c`                        | modify  | +30  | Same as F20 for the streaming path |
| F22 | `src/ml/cli.c`                                    | modify  | +120 | `human ml molora` subcommands: `train-router`, `manifest add-expert`, `manifest validate`, `dry-run-route` (offline replay) |
| F23 | `include/human/ml/cli.h`                          | modify  | +12  | CLI arg structs for the new subcommands |
| F24 | `tests/test_ml.c`                                 | modify  | +90  | `human ml molora dry-run-route` snapshot tests using fixtures |
| F25 | `tests/fixtures/molora_manifest_4_experts.json`    | **new** | 30   | Reference manifest used by tests |
| F26 | `tests/fixtures/molora_router_uniform.bin`        | **new** | 4.3KB | Reference router with uniform-softmax weights |
| F27 | `scripts/check-molora-ab.sh`                      | **new** | 80   | Offline gate: run `lora-runner` once per slot, score with `lora-ab`, fail when N-expert delta is below `MOLORA_FLOOR_DELTA=0.05` vs single-expert baseline |
| F28 | `scripts/verify-all.sh`                           | modify  | +4   | Call `check-molora-ab.sh` next to `check-lora-ab.sh` (skip when manifest absent) |
| F29 | `ui/src/components/hu-fidelity-tile.ts` (extension) | modify | +40 | Add a fourth lane "router accuracy" (% of turns where router's top-1 == argmax-fidelity expert on a held-out replay) when the gateway exposes it |
| F30 | `src/gateway/cp_admin.c`                          | modify  | +80   | New gateway method `metrics.molora` → `{router_accuracy, top1_slot_distribution, mean_active_slots}` |

**Totals.** ~12 new files, ~17 modified files, ~2.0 KLOC added (≈1.1 KLOC C source + 0.6 KLOC tests + 0.3 KLOC scripts/fixtures/UI), ~250 LOC public API surface.

# 3 · Data flow

## 3.1 Startup (daemon boot)

```
daemon.c
  └─ hu_w14_scheduler_open
  └─ hu_persona_load  (existing path; overlays now carry expert_id / expert_weight_floor)
  └─ if (cfg.molora.enabled)
       └─ hu_molora_load_manifest        (F12)            → hu_molora_t * (slot table)
       └─ hu_lora_router_open(cfg.router_path)            → hu_lora_router_t *
       └─ for each expert in manifest:
             hu_lora_mixture_adapter_t a = {slot, path, id};
         (collect into array)
       └─ hu_provider_load_adapter_mixture(provider, alloc, adapters, n)   (F7 + F8)
            llamacpp: for each: llama_adapter_lora_init() into a resident slot table.
                      no `llama_set_adapters_lora` yet — that happens per-turn.
       └─ register HU_JOB_MOLORA_ROUTER_TRAIN runner with the scheduler (F18)
```

Failure modes:
- Manifest missing → log `info`, leave provider with no resident adapters, route through the existing single-adapter path. Daemon never crashes here.
- Router missing → `hu_lora_router_open_default` initializes a uniform-softmax router (every slot equally likely) so the daemon ships with a working — if dumb — mixture. The W14 runner trains it within the first idle window.
- `provider->vtable->load_adapter_mixture == NULL` → log `info`, fall back to the existing single-adapter auto-load path (W13). This is what every cloud provider sees today.

## 3.2 Per-turn hot path (the only place this matters for latency)

```
agent_turn.c                                                 [≤ 50 µs added]
  ├─ derive hu_lora_router_ctx_t:
  │     channel_id   = hu_lora_router_channel_id(channel_name, len)         //  ≤ 200 ns
  │     message_class = hu_lora_router_classify_message(msg, len)           //  ≤ 2 µs (FNV-1a + 6 keyword checks)
  │     macro_mode    = hu_persona_macro_mode_for_context(agent, contact)   //  ≤ 1 µs (already-computed in mood.c)
  │     overlay       = hu_persona_find_overlay(persona, channel, len)      //  already in turn, no extra cost
  │     ctx.preferred_slot  = overlay ? overlay->expert_id : 0
  │     ctx.preferred_weight_floor = overlay ? overlay->expert_weight_floor : 0.0f
  ├─ hu_lora_router_select(router, &ctx, &mixture)                          //  ≤ 5 µs (1 matmul + softmax + top-k)
  ├─ if (mixture.n > 0):
  │     hu_provider_set_adapter_mixture(provider, mixture.slots, mixture.weights, mixture.n)   //  ≤ 30 µs (llamacpp wraps llama_set_adapters_lora — does NOT touch GPU memory; just updates an internal pointer table.)
  └─ existing chat_with_system / stream_chat                                //  unchanged
```

The hot path adds **≤ 50 µs** per turn. Provider chat-with-system today is bounded only by network/decode latency (cloud) or token decode (local), neither of which is affected. We document this as N≤0.1 % of any tier's TTFT budget.

## 3.3 Idle path (router training, W14 job)

```
hu_scheduler_tick  (1 Hz)
  └─ HU_JOB_MOLORA_ROUTER_TRAIN  (interval_sec = 6 * 3600, requires_idle, requires_ac_power)
        molora_router_train_runner   (F18)
          ├─ collect labelled samples from memory:
          │     for each (channel_id, message_class) bucket in the last N turns:
          │       run hu_communication_style_fidelity_score against the resident expert outputs
          │       (recorded in agent_turn from the previous LoRA-AB writeback)
          │       sample.target_slot = argmax over experts
          ├─ if (n_samples < cfg.train_min_samples)  return HU_OK   (silent skip — telemetry counter++)
          ├─ hu_lora_router_train(router, samples, n, &cfg, budget_ms)
          ├─ hu_lora_router_save(router, tmp_path)
          └─ atomic-rename to cfg.router_path                       (mirror hu_personal_model_save atomicity)
```

Crash safety follows the exact pattern landed for `hu_personal_model_save`
(commit 028f4544 era): `tmp + fwrite + fflush + fsync + rename`. Tested in
`tests/test_lora_router.c::test_router_save_preserves_prior_state_when_tmp_blocked` (mirrors
`tests/test_personal_model_atomic_save.c`).

# 4 · Coexistence with initiatives #04 (MLX Qwen3) and #05 (TTT)

The brief is explicit: "Must coexist cleanly with initiative #04 (MLX Qwen3
provider) and #05 (verifier-driven TTT). Note the API conflicts in the design
doc." Here they are, with concrete resolution:

| Conflict | Where | Resolution |
|----------|-------|-----------|
| **C1**: Both #02 and #04 modify the provider vtable. | `include/human/provider.h`. | #04 adds ONLY `load_adapter` / `unload_adapter` / `active_adapter` (already declared today). #02 adds the **separate** `load_adapter_mixture` / `set_adapter_mixture` / `active_mixture` triple. No symbol collision. #04 lands its slice first; #02 lands the mixture hook on top, no rebase needed. |
| **C2**: #02 and #05 both want to swap LoRA at chat time. | `src/agent/agent_turn.c`. | **Slot 7 is reserved for #05.** TTT writes its tiny per-conversation update into slot 7's expert; the router's softmax has slot 7 in its output dim but the on-disk manifest leaves it at `kind:ttt_scratch, path:null` so under default operation slot 7 is unused (uniform router still has weight there, but the mixture path filters out slots with `path == NULL`). When #05 lands, TTT calls `hu_molora_swap_ttt_slot(molora, expert_path)` (new public function in `src/persona/molora.c`) which atomically rebinds slot 7 and bumps a generation counter the router observes. The router doesn't retrain — it's structurally agnostic to which file backs slot 7. |
| **C3**: #02 router and #04 model_router are different things — naming risk. | Cross-file. | `hu_model_router_*` (existing, in `include/human/agent/model_router.h`) stays: it picks *cloud models*. `hu_lora_router_*` (new, in `include/human/persona/lora_router.h`) picks *on-device experts*. The two never compose: cloud models always have `load_adapter_mixture == NULL`. Documented in F1 header doc-comment and in the new `docs/CONCEPT_INDEX.md` row. |
| **C4**: #02 manifest vs #04 single `personalization.lora_adapter_path`. | `include/human/config_types.h`. | Both blocks remain. When `molora.enabled == true` AND the active provider supports `load_adapter_mixture`, the daemon uses the mixture path and **ignores** `personalization.lora_adapter_path` (with one `info`-level log line at startup so the user notices). Otherwise the single-adapter path takes over. Tested in F16. |
| **C5**: #06 SimPO/ORPO/GRPO-2 trainers feed the expert pool. | Producer side. | No conflict, but the manifest gains a free-form `"trainer": "simpo"` field on each expert entry so #06's eval gates can A/B trainers within a slot. Schema version stays at 1 (additive fields are forward-compatible). |
| **C6**: #07 ThinkPRM verifier uses the same fidelity score the router trains on. | Label side. | Net positive: when #07 lands, the router's training labels improve in quality (PRM scores instead of heuristic fidelity). Router code is unchanged — only the W14 runner's label-extraction step swaps in the better scorer. |
| **C7**: #09 memory trust tiers. | Bank construction. | Banks used to train the experts pass through the same `hu_pii_redact` / `hu_quality_check` / trust-tier gate. The MoLoRA pipeline does NOT introduce a new path into memory — it reuses `hu_persona_banks_extract_from_history`. |

**Net effect.** This initiative widens the adapter-loading API in a strictly additive way and reserves two well-known slots (0 and 7) so #04 and #05 can land independently in either order.

# 5 · Component implementations (detailed)

## 5.1 Router forward pass (F2, the hot path)

Pseudocode (≈40 LOC in C with the param layout pinned):

```c
int hu_lora_router_select(const hu_lora_router_t *r,
                          const hu_lora_router_ctx_t *ctx,
                          hu_lora_mixture_t *out) {
    if (!ctx || !out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* Degenerate path: missing router → slot-0 baseline at full weight. */
    if (!r) {
        out->slots[0]   = 0;
        out->weights[0] = 1.0f;
        out->n          = 1;
        return HU_OK;
    }

    /* 1 · Build the 24-dim one-hot input on the stack. */
    float x[HU_LORA_ROUTER_INPUT_DIM] = {0};
    if (ctx->channel_id    < 8) x[ctx->channel_id]      = 1.0f;
    if (ctx->message_class < 8) x[8  + ctx->message_class] = 1.0f;
    if (ctx->macro_mode    < 8) x[16 + ctx->macro_mode]    = 1.0f;

    /* 2 · Hidden layer: h = ReLU(W1 x + b1). 24 * 32 = 768 FMA. */
    float h[HU_LORA_ROUTER_HIDDEN_DIM];
    for (int j = 0; j < 32; j++) {
        float s = r->b1[j];
        for (int i = 0; i < 24; i++) s += r->W1[j * 24 + i] * x[i];
        h[j] = s > 0.0f ? s : 0.0f;
    }

    /* 3 · Output layer: y = W2 h + b2. 32 * 8 = 256 FMA. */
    float y[HU_LORA_ROUTER_OUTPUT_DIM];
    for (int j = 0; j < 8; j++) {
        float s = r->b2[j];
        for (int i = 0; i < 32; i++) s += r->W2[j * 32 + i] * h[i];
        y[j] = s;
    }

    /* 4 · Apply overlay floor on the preferred non-zero slot
     *     (slot 0 is the macro-mode floor, applied separately below). */
    if (ctx->preferred_slot >= 1 && ctx->preferred_slot < 8 &&
        ctx->preferred_weight_floor > 0.0f) {
        const float boost = ctx->preferred_weight_floor;  /* logit-space additive boost */
        y[ctx->preferred_slot] += boost;
    }

    /* 5 · Softmax in-place (numerically stable). */
    softmax_inplace(y, 8);

    /* 6 · Floor slot 0 at config.macro_mode_floor before top-k. */
    /* (router has the floor compiled in at open() time so this is a single
     *  fmaxf — no global state lookup in the hot path.) */
    y[0] = y[0] < r->macro_mode_floor ? r->macro_mode_floor : y[0];
    renormalize(y, 8);

    /* 7 · Top-HU_MOLORA_MAX_ACTIVE by weight, skipping slots whose
     *     manifest entry has `path == NULL` (init-#05 reserved). */
    return top_k_filtered(y, r->slot_mask, HU_MOLORA_MAX_ACTIVE, out);
}
```

Approx 1,024 FMA + one softmax + one top-k of 8 entries.

## 5.2 Router training (F2, idle-only)

`hu_lora_router_train` is the only non-trivial training path in this
initiative. Because the model is so small (1,064 params), we deliberately
use plain SGD with a fixed learning rate and no momentum — exactly the
pattern the W13 reference `hu_ml_optimizer_t` already supports, but cheaper
to keep self-contained (no allocator handshake against the ML optimizer
vtable, no `hu_lora_register_params` round-trip).

Sketch:

```c
for (epoch = 0; epoch < cfg->epochs && time_remaining > 0; epoch++) {
    shuffle(samples, n);
    for (each minibatch of cfg->batch_size) {
        forward(r, sample.ctx, &y);
        ce_gradient(y, sample.target_slot, &grad_y);          // y - one_hot
        backward(r, sample.ctx, grad_y);                       // 2 matmuls; in-place into r->grad_*
        sgd_step(r, cfg->lr * sample.weight);                  // p -= lr * grad
        zero_grads(r);
    }
}
```

Budget enforcement: trainer checks `now_ms() > budget_deadline` between
minibatches; on timeout, exits with `HU_OK` and the partial weights are
still saved on the runner's success path (training is anytime-valid —
worse than full-epoch, never worse than uniform).

## 5.3 llamacpp mixture implementation (F8)

The modern API (b3000+) is *already* mixture-shaped:

```c
int llama_set_adapters_lora(struct llama_context * ctx,
                            struct llama_adapter_lora ** adapters,
                            int n,
                            float * scales);
```

We just hold a resident pool of `llama_adapter_lora *` indexed by slot
and pass the active subset on each `set_adapter_mixture` call.

```c
typedef struct llamacpp_mixture_slot {
    char *adapter_id;
    char *adapter_path;
    struct llama_adapter_lora *adapter;   /* lazily inited on first load_adapter_mixture */
} llamacpp_mixture_slot_t;

typedef struct llamacpp_ctx {
    /* ... existing fields ... */
    llamacpp_mixture_slot_t mixture[HU_MOLORA_MAX_SLOTS];  /* +~512 B per provider instance */
    uint8_t active_slots[HU_MOLORA_MAX_ACTIVE];
    float   active_weights[HU_MOLORA_MAX_ACTIVE];
    size_t  active_n;
} llamacpp_ctx_t;

static hu_error_t llamacpp_load_adapter_mixture(void *ctx, hu_allocator_t *alloc,
                                                 const hu_lora_mixture_adapter_t *adapters,
                                                 size_t adapter_count) {
    /* Validate: slot 0 must be present (the macro-mode baseline). */
    /* Validate: each slot in [0..HU_MOLORA_MAX_SLOTS). */
    /* Free prior resident adapters in the mixture pool. */
    /* For each adapter: llama_adapter_lora_init(c->model, path); store in c->mixture[slot]. */
    /* Note: we do NOT call llama_set_adapters_lora here — that's set_adapter_mixture's job. */
}

static hu_error_t llamacpp_set_adapter_mixture(void *ctx,
                                                const uint8_t *slots, const float *weights,
                                                size_t n) {
    /* Gather pointers from c->mixture[slots[i]].adapter into a stack array. */
    /* n == 0 → llama_set_adapters_lora(ctx, NULL, 0, NULL). */
    /* Otherwise: llama_set_adapters_lora(c->ctx, A, n, scales). */
}
```

The legacy single-adapter `load_adapter` still works and remains the only
path the W13 daemon auto-load hits when `molora.enabled == false`. When
mixture is on, `load_adapter` and `load_adapter_mixture` coexist on the
same provider instance — they share `c->ctx` but write to disjoint slot
pools (`active_adapter` for single, `mixture[]` for the array). Documented
inline in F8.

# 6 · Test plan (gate D3)

## 6.1 Deterministic unit tests (`tests/test_lora_router.c`, ≥ 14)

| Test | What it pins |
|------|--------------|
| `test_router_open_default_returns_uniform` | Open with NULL path → uniform softmax → every slot weight ≈ 0.125 → top-3 includes slot 0 |
| `test_router_select_null_ctx_returns_invalid_argument` | NULL ctx, NULL out → `HU_ERR_INVALID_ARGUMENT` |
| `test_router_select_null_router_returns_slot_0_only` | NULL router → out = `{slots[0]=0, weights[0]=1, n=1}` |
| `test_router_select_respects_macro_mode_floor` | Forge a router where slot 1 dominates; assert slot 0 still appears with weight ≥ `macro_mode_floor` |
| `test_router_select_filters_null_manifest_slots` | Mask slot 7 (TTT scratch); router output that includes 7 → mixture drops 7 silently |
| `test_router_select_overlay_preferred_floor_boosts_slot` | Forge ctx with `preferred_slot=2, preferred_weight_floor=0.7`; assert slot 2 ends up in top-1 even when raw router scored it 3rd |
| `test_router_select_overlay_slot_0_ignored` | Forge ctx with `preferred_slot=0` (illegal); router treats it as `0 → no preference`, doesn't double-floor |
| `test_router_train_improves_top1_accuracy_on_synthetic_4_channel_set` | Generate 1000 synthetic samples where channel ⇒ unique expert; train; assert top-1 accuracy ≥ 0.95 on a held-out 200-sample set |
| `test_router_train_budget_respected_under_tight_deadline` | Pass `budget_ms = 5`; trainer returns within 10 ms wall; weights are well-defined (no NaN); accuracy improves but doesn't hit ceiling |
| `test_router_train_min_samples_gate` | n=10, cfg.min_samples=256 → runner returns `HU_OK` without mutating router |
| `test_router_save_preserves_prior_state_when_tmp_blocked` | Mirror `personal_model_save_atomic` test: pre-block `<path>.tmp` with a directory → save fails → original file intact byte-for-byte |
| `test_router_load_rejects_bad_magic` | Truncate header → `HU_ERR_IO`, no allocations leaked (ASan-clean) |
| `test_router_load_rejects_truncated_weights` | Truncate file mid-weight-array → `HU_ERR_IO`, no allocations leaked |
| `test_classify_message_keyword_set` | "help i'm in crisis" → CRISIS; "thanks" → ACK; "what's the weather?" → QUESTION; everything else → CHITCHAT/OTHER |

## 6.2 Provider dispatcher tests (`tests/test_provider_all.c`, +6)

Extend the existing NOT_SUPPORTED guard pack:

- For each cloud provider (openai, anthropic, gemini, ollama, openrouter): `hu_provider_load_adapter_mixture` returns `HU_ERR_NOT_SUPPORTED`, `hu_provider_set_adapter_mixture` returns `HU_ERR_NOT_SUPPORTED`, `hu_provider_active_mixture` fills `*out_n = 0`. No deref of provider ctx, no allocator touch.
- `test_load_adapter_mixture_rejects_null_args` — NULL provider, NULL adapters, count=0 → `HU_ERR_INVALID_ARGUMENT`.

## 6.3 llamacpp mixture tests (`tests/test_llamacpp_provider.c`, +6)

Stay in the `HU_ENABLE_LLAMACPP=OFF` default for the slim suite (these are
`HU_ERR_NOT_SUPPORTED` checks). For the `llamacpp-on` matrix entry, three
linked-build tests gated on `HU_LLAMACPP_LINKED`:

- `test_llamacpp_mixture_load_4_slots_no_set_returns_ok` — 4 adapters init via stub model; `set_adapter_mixture(NULL, 0)` no-ops cleanly.
- `test_llamacpp_mixture_set_then_get_round_trips` — load 4 → set {1, 2} with weights {0.6, 0.4} → `active_mixture` returns the same.
- `test_llamacpp_mixture_load_rejects_missing_slot_0` — manifest without slot 0 → `HU_ERR_INVALID_ARGUMENT` (with msg "slot 0 required").

## 6.4 Manifest + integration tests (`tests/test_molora_manifest.c`, ≥ 7)

- Parse the 4-expert reference manifest; assert slot table fully populated.
- Serialize → re-parse → byte-equal manifest.
- Missing `experts` array → `HU_ERR_INVALID_ARGUMENT`.
- `hu_molora_apply_to_provider` against a `HU_ERR_NOT_SUPPORTED` provider → no crash, returns `HU_ERR_NOT_SUPPORTED`, persona system stays usable.
- `hu_molora_apply_to_provider` happy path against the linked llamacpp stub (4 slots, slot 7 NULL).
- `hu_molora_swap_ttt_slot(slot=7, path)` (reserved for #05) — non-NULL path writes slot, NULL path clears, bumps `mixture_generation` so the router-cached mask updates next call.

## 6.5 Runner test (`tests/test_molora_runner.c`, ≥ 5)

- Below-min-samples → `HU_OK`, router unchanged, counter += 1.
- 1000-sample synthetic run → router accuracy ≥ 0.9 on held-out → counter `success += 1`.
- Tight budget (5 ms) → returns under 30 ms wall, no partial-state corruption.
- Save-failure path (read-only `experts_dir`) → returns `HU_ERR_IO`, prior router file intact.
- Trainer-internal NaN injection (artificial) → caught by `isnan(w[0])` post-step guard, training aborts, prior router intact.

## 6.6 Offline E2E gate (`scripts/check-molora-ab.sh`)

The shipping gate: for each of `{telegram, imessage, slack}` (Tier-1 channels in CLAUDE.md M6), run the existing `lora-runner` once with the single-adapter baseline and once with the 4-expert mixture, score both with `lora-ab`. Fail when:

- Mean of per-channel deltas < `MOLORA_FLOOR_DELTA` (default 0.05).
- *Any* channel regresses by more than `MOLORA_PER_CHANNEL_FLOOR` (default 0.02).

The script writes a `~/.human/last_molora_ab.json` document the new
gateway method (F30) reads to populate the dashboard tile.

## 6.7 Fuzz harness (deferred until linked build)

When `HU_ENABLE_LLAMACPP=ON`, add `fuzz/fuzz_lora_router.c` (LibFuzzer):
random bytes → `hu_lora_router_open` → on success, random ctx → `_select`
→ assert: no UB, mixture invariant (sum(weights) ∈ [0.5, 1.0001] when n>0;
slot 0 always present when macro_mode_floor > 0). Aborts on any
`__asan_*` report.

# 7 · Risk register (gate D4)

| # | Risk | Likelihood × Impact | Mitigation |
|---|------|---------------------|-----------|
| **R1** | **Router silently drifts** as channel mix changes (e.g. new channel, training data shifts). | Med × High. | The W14 runner refuses to train when `n_samples < cfg.train_min_samples`, AND a new metric `metrics.molora.router_accuracy` is exposed via gateway (F30). Drift below `MOLORA_FLOOR_DELTA` for >7 days triggers an `info`-level log and a dashboard tile (`hu-fidelity-tile.ts` already accepts a new lane). Operators can re-train manually via `human ml molora train-router`. |
| **R2** | **`load_adapter_mixture` collides with #04 / #05** if they're implemented before this. | Low × Med. | All three vtable methods are new optional pointers next to the W13 triple; cloud providers leave them NULL; #04 doesn't need them; #05 uses slot 7 only. The `tests/test_provider_all.c` NOT_SUPPORTED pack pins the safety contract. |
| **R3** | **ASan / leak risk** on the lazy `llama_adapter_lora_init` pool — adapters loaded but never reused on deinit. | Med × Med. | `llamacpp_deinit` walks `mixture[]` and frees each non-NULL `adapter` with `llama_adapter_lora_free`, *then* the legacy single-adapter cleanup. Three explicit ASan tests in the linked build verify (load_mixture → never set → deinit → 0 leaks; load_mixture → set → unload via `unload_adapter` on each id → deinit → 0 leaks; load_mixture twice with overlapping slots → 0 leaks across replacement). |
| **R4** | **Binary-size regression** if the router struct or message-class classifier balloon. | Low × Med. | Hard upper bound enforced in `tests/test_lora_router.c::test_router_resident_size_is_under_8kb` (asserts `sizeof(hu_lora_router_t) + HU_LORA_ROUTER_PARAM_COUNT * sizeof(float) <= 8192`). CI's binary-size gate runs `size build-release/human` and flags >2 KB delta. |
| **R5** | **Persona-continuity loss** when a strongly-tilted router picks a single-channel expert and the agent abandons macro-mode tone. | Med × High. | `cfg.macro_mode_floor` defaults to 0.2 and is baked into the router struct at `open()` time. The router's softmax is post-floored on slot 0 before top-k, so the macro-mode adapter cannot fall out of the active mixture as long as the floor is positive. Pinned by `test_router_select_respects_macro_mode_floor`. The offline gate (§6.6) also fails on >2 % regression in any channel, which catches macro drift end-to-end. |
| **R6** | **Memory-poisoning vector** via a malicious persona JSON pinning a hostile expert id with weight floor 1.0 — effectively forcing the agent into a different style on every turn. | Low × High (intersects #09). | `expert_weight_floor` clamps at parse time to [0.0, 0.8] (the upper bound leaves room for the macro floor). Only the persona owner can edit the persona file — same trust model as today's persona JSON. #09's memory trust tiers do not touch persona files; this is an additional security note we coordinate with the #09 design doc. |
| **R7** | **Quality regression on small expert counts** (1–2 experts only) where the mixture overhead pays no benefit. | Low × Low. | When `manifest.experts_count < 2` the mixture path early-returns and we fall through to the single-adapter path. Telemetry counter `molora_skip_single_adapter` lets us measure how often this fires. |
| **R8** | **Tests touch real `~/.human/`** when the runner trains on history. | Low × Med. | Same pattern as `scripts/check-lora-baseline.sh`: tests set `HU_MOLORA_EXPERTS_DIR` env var to a private tmpdir; runner respects it. Mirrors the `HU_PERSONA_DIR` staging used by other persona tests. |

# 8 · Binary-budget delta (gate D6)

| Item | Static | RSS at runtime | Off-binary |
|------|-------|---------------|-----------|
| Router code (`lora_router.c` after `-Os -flto`) | ~3.0 KB | — | — |
| Mixture vtable wrappers (`helpers.c` delta) | ~0.6 KB | — | — |
| llamacpp mixture path (F8 delta) | ~1.2 KB | — | — |
| `hu_persona_overlay_t` 2 new fields | 0 KB | 5 B × overlay count (~ 0.04 KB on a 16-overlay persona) | — |
| Manifest types + helpers (`molora.c`, link-time) | ~1.5 KB | — | — |
| Router weights (`router.bin`) | 0 KB | **4.3 KB** | 4.3 KB |
| Per-expert metadata (manifest, slot table) | 0 KB | ~0.6 KB | < 1 KB |
| Experts themselves | 0 KB | 0 KB (mmap or lazy heap) | 4–16 MB × N |
| **Total** | **≤ 8 KB** in MinSizeRel+LTO | **≤ 8 KB** added RSS (router + slot table) | Off-binary, user-controlled |

**Hard ceiling: 8 KB binary, 8 KB RSS.** Enforced by the unit test cited
in R4 + the existing `benchmark.yml` binary-size workflow.

# 9 · Defer / descope condition (gate D7)

**We park this initiative if, after running `scripts/check-molora-ab.sh`
on a representative 4-expert manifest (telegram + imessage + slack +
macro-default) trained on the user's history, the mean per-channel
fidelity delta vs. the single-LoRA baseline is below `MOLORA_FLOOR_DELTA
= 0.05` on the N3 Tier-1 channel naturalness eval from the 6-mo
roadmap.** Concretely, that means: with four experts we do not move the
mean of `hu_communication_style_fidelity_score` across the Tier-1
channels by more than 5 percentage points relative to the *current*
single-LoRA personalization path. In that world the extra binary cost,
the W14 idle-CPU cost of training the router, and the operational
complexity of an N-file manifest are not paying for themselves; we
revert the manifest/router/scheduler glue, keep the additive vtable
methods (they're stable surface, not behavior), and re-evaluate when
either (a) the underlying expert quality lifts (e.g. #06 SimPO/ORPO
trainers land and raise per-expert fidelity by ≥ 0.05), or (b) we move
to token-level routing (LD-MoLE / DynMoLE) where the per-turn
sparse-select limit is the bottleneck instead of the per-expert
fidelity. We do *not* park on a single regressed channel; per-channel
floor (`MOLORA_PER_CHANNEL_FLOOR = 0.02`) catches that earlier.

# 10 · Build sequence (proof-of-feasibility, in order)

Each phase ends with a green test suite + an explicit honest-status
update in `2026-05-10-m3-frontier-model-bridge.md`'s tracking table.

- [ ] **Phase A — vtable + dispatcher** (≤ 1 day): F6, F7, F9. Land the
      mixture triple + helpers + NOT_SUPPORTED dispatcher pack. No
      behavior change. 9,800+ tests still pass.
- [ ] **Phase B — router core** (≤ 2 days): F1, F2, F3. Router forward
      + train + save/load + atomicity test. Unit tests against
      synthetic samples. No agent integration yet.
- [ ] **Phase C — manifest + overlay** (≤ 1 day): F4, F5, F11, F12,
      F13, F14, F15, F16, F25, F26. Manifest parser, overlay JSON
      fields, fixture manifest, config block. Tests against the
      `HU_ERR_NOT_SUPPORTED` dispatcher (still no real provider work).
- [ ] **Phase D — agent integration** (≤ 1 day): F20, F21, F17. Hot
      path computes ctx → router → mixture → no-op against
      NOT_SUPPORTED provider (existing behavior preserved). Adds the
      scheduler enum entry. No new visible behavior.
- [ ] **Phase E — llamacpp mixture** (≤ 2 days): F8, F10. Linked-build
      tests in the `llamacpp-on` matrix entry. Per the R3 mitigation:
      ASan-clean across load/replace/deinit. Real mixture now visible
      under `provider: llamacpp`.
- [ ] **Phase F — CLI + W14 runner** (≤ 2 days): F18, F19, F22, F23,
      F24, F27, F28. `human ml molora train-router` + W14 idle job.
      Runner respects min-samples and budgets. Offline gate wired.
- [ ] **Phase G — observability + dashboard** (≤ 1 day): F29, F30.
      `metrics.molora` gateway method + fidelity-tile fourth lane.
- [ ] **Phase H — proof gate (D7)**: run §6.6 on a real 4-expert
      manifest against the user's history. If the delta beats
      `MOLORA_FLOOR_DELTA`, flip status to `design done` → `sprint
      open` in the master coordinator table. Otherwise park per §9.

Total estimate: **≈ 10 working days of one focused engineer**, in line
with the master program's "design + proof of feasibility" budget.

# 11 · Cross-references (gate D1, D5)

## 11.1 Vtable surfaces touched

| Vtable / type | Surface | Action |
|---------------|--------|--------|
| `hu_provider_t` / `hu_provider_vtable_t` | `include/human/provider.h` | **Add 3 optional fn ptrs**: `load_adapter_mixture`, `set_adapter_mixture`, `active_mixture`. No existing field changed. |
| `hu_persona_overlay_t` | `include/human/persona.h` | **Add 2 fields**: `expert_id` (uint8_t), `expert_weight_floor` (float). Zero-init backward-compatible. |
| `hu_job_kind_t` | `include/human/agent/scheduler.h` | **Add 1 enum entry**: `HU_JOB_MOLORA_ROUTER_TRAIN`. |
| `hu_personalization_config_t` / new `hu_molora_config_t` | `include/human/config_types.h` | **Add sibling block** `hu_molora_config_t`; legacy block unchanged. |
| `hu_lora_router_*`, `hu_molora_*` | `include/human/persona/lora_router.h`, `include/human/persona/molora.h` | **New** public surface — all symbols `hu_<module>_<action>` per `docs/standards/engineering/naming.md`. |

## 11.2 Naming compliance

All new symbols comply with `docs/standards/engineering/naming.md`:

- Public functions: `hu_lora_router_*`, `hu_molora_*`, `hu_provider_load_adapter_mixture` — `hu_<module>_<action>`.
- Types: `hu_lora_router_t`, `hu_lora_mixture_t`, `hu_molora_config_t`, `hu_lora_router_ctx_t` — `hu_<name>_t`.
- Constants: `HU_MOLORA_MAX_ACTIVE`, `HU_LORA_ROUTER_PARAM_COUNT`, `HU_LORA_ROUTER_MAGIC` — `HU_SCREAMING_SNAKE`.
- Tests: `test_router_open_default_returns_uniform` etc. — `subject_expected_behavior`.
- Enums: `HU_MSG_CLASS_*`, `HU_PERSONA_MODE_*`, `HU_JOB_MOLORA_ROUTER_TRAIN` — consistent with the existing `hu_cognitive_tier_t` / `hu_job_kind_t` style.

## 11.3 arXiv references (D5)

| Ref | Citation | Used for |
|-----|---------|---------|
| **MoLoRA / MoLE** | Wu, Huang, Wei. *Mixture of LoRA Experts*. arXiv:2404.13628 (April 2024; ICLR 2024). | Foundational shape: N independent LoRAs composed via a learnable gate at layer level. We adopt the *static-per-turn* slice (cheaper than MoLE's hierarchical gating) and reserve the hierarchical-gate option for a future initiative. |
| **MixLoRA** | Li et al. *MixLoRA: Enhancing Large Language Models Fine-Tuning with LoRA-based Mixture of Experts*. arXiv:2404.15159 (April 2024). | Adjacent prior art for the experts-as-MoE framing on top of an open base. Mostly cited as the alternative we explicitly did **not** pick (token-level routing requires base modifications we won't make in this slice). |
| **X-LoRA** | Buehler & Buehler. *X-LoRA: Mixture of Low-Rank Adapter Experts*. arXiv:2402.07148 (February 2024). | Reference for "deep layer-wise, token-level" routing; we cite it as the existence proof that the gate can be a tiny MLP separate from the base model — the architectural permission slip for our 4.3-KB router. |
| **DynMoLE** | Wu, Zhang, et al. *DynMoLE: Boosting Mixture of LoRA Experts Fine-Tuning with a Hybrid Routing Mechanism*. arXiv:2504.00661 (April 2025). | Tsallis-entropy hybrid routing — better dynamic top-k than fixed-k. **Deferred**: our static top-3 (HU_MOLORA_MAX_ACTIVE) is the safer first slice; DynMoLE is the natural follow-on initiative once we have the offline eval gate live. |
| **LD-MoLE** | Shen et al. *LD-MoLE: Learnable Dynamic Routing for Mixture of LoRA Experts*. arXiv:2509.25684 (September 2025; ICLR 2026). | Differentiable Sparsegen routing, token-level + layer-level adaptive sparsity on Qwen3-1.7B / Llama-3.2-3B. **Deferred**: same future-initiative slot as DynMoLE; LD-MoLE is the strictly better follow-on but requires touching the base model's forward pass (out of scope for the `hu_provider_t` boundary). |
| **SAMoRA** | Wang et al. *SAMoRA: Semantic-Aware Mixture of LoRA Experts for Task-Adaptive Learning*. arXiv:2604.19048 (April 2026; ACL 2026 Findings). | Semantic-aware routing + task-adaptive scaling. The semantic-router idea informs the message-class one-hot in our feature vector (we approximate SAMoRA's text-embedding semantic input with a 6-class keyword classifier; cheap, no extra embedding cost). |

## 11.4 Standards adherence

- **`docs/standards/engineering/principles.md`** — KISS (one new optional vtable triple, not a wide overload), YAGNI (no generic `hu_request_ctx_t`; the router context type stays in `lora_router.h`), Fail Fast (explicit NOT_SUPPORTED on every cloud provider), Determinism (no real network or hardware in tests; `HU_IS_TEST` guards in the runner).
- **`docs/standards/engineering/anti-patterns.md`** — no vtable pointers to temporaries (resident pool is heap-owned by the provider; freed in deinit); explicit `free()` on every `alloc->alloc` (every test exercised under ASan).
- **`docs/standards/security/threat-model.md`** — R6 in §7 documents the persona-JSON-as-attack-surface case and pins the clamp; coordinates with init #09 memory trust tiers without overlapping its scope.
- **`docs/standards/ai/evaluation.md`** — the N3 channel naturalness eval is the gating metric (§9). No new metric definition introduced; reuses `hu_communication_style_fidelity_score` and `lora-ab`'s delta.

# 12 · Open questions (intentionally surfaced for review)

1. **Router precision.** Stay FP32 (4.3 KB) or quantize the router itself to INT8 (≤ 1.1 KB)? Forward latency wouldn't change at this size, but the on-disk footprint shrinks 4×. **Defaulting to FP32 for the first slice**; revisit if R4 trips.
2. **Slot capacity.** `HU_MOLORA_MAX_SLOTS = 8`. Is 8 enough across (Tier-1 channels = 4) + (macro modes = 4)? Probably; if not, the enum width is the only API change needed (manifest schema_version bumps to 2). Cheap escape hatch.
3. **Where does slot 0 come from when the user has never run `lora-persona`?** Fall back to a zero-rank pass-through adapter so the mixture call is well-defined? **Current answer**: when `experts[0].path` is missing, the daemon skips `load_adapter_mixture` entirely and uses the single-adapter path. Documented in F12. We do NOT ship a stub adapter — keeps the bytes off-disk and the contract honest.

# 13 · Verbatim deliverable summary (D0 conformance check)

- [x] **D0** — file at `docs/plans/2026-05-11-init-02-molora-channels.md`, YAML frontmatter present, related links resolve.
- [x] **D1** — vtable additions enumerated in §11.1; all new symbols named per `docs/standards/engineering/naming.md` (§11.2).
- [x] **D2** — every file to create/modify in §2.6 with LOC estimates.
- [x] **D3** — test plan in §6 with deterministic unit, integration, fuzz, and offline E2E gate.
- [x] **D4** — top 8 risks with mitigations in §7.
- [x] **D5** — six arXiv references with IDs in §11.3.
- [x] **D6** — explicit binary-budget table in §8, hard ceiling 8 KB, enforced by unit test + CI workflow.
- [x] **D7** — explicit defer/descope condition in §9 keyed to the N3 channel-naturalness eval.

— end of init-02 design.
