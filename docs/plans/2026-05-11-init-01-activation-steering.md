---
title: "Init-01 — Activation steering / SAE persona control on cloud + on-device providers"
created: 2026-05-11
status: deferred
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-10-m3-frontier-model-bridge.md
  - 2026-05-11-rl-loop-phase-1-llamacpp.md
  - 2026-05-10-master-follow-through-program.md
  - ../../CLAUDE.md
  - ../../AGENTS.md
  - ../standards/engineering/principles.md
  - ../standards/ai/prompt-engineering.md
  - ../../include/human/provider.h
  - ../../include/human/persona.h
  - ../../include/human/memory/personal_model.h
  - ../../src/agent/frontier_prompt.c
last_audit: 2026-05-25
---

# Init-01 — Activation steering / SAE persona control on cloud + on-device providers

**Initiative slug.** `activation-steering`. Initiative #01 of the 14-track SOTA-2026 program (`2026-05-11-sota-2026-massive-team-program.md`).

## One-line

Even when h-uman calls a cloud frontier model, we steer outputs toward the user's persona by **(a) activation-steering the on-device draft / local model** through a small abstract steering vector derived from `hu_persona_t` + `hu_personal_model_t` style EWMA, and **(b) prompt-side adversarial weighting** of system-prompt directive lines whose tokens map to SAE features tied to persona traits (warmth, formality, humor density, hedging). Cloud providers run the prompt-side fallback only; on-device providers (`llamacpp`, `embedded`, future `mlx_qwen3`) actually apply the residual-stream addition.

## Why now

| Pillar | Evidence |
|---|---|
| SAEs reliably isolate trait features at 1B–4B scale | Cunningham et al. 2023 (arXiv:2309.08600), Templeton et al. 2024 ("Scaling Monosemanticity", Anthropic) |
| Persona steering via residual-stream addition is empirically robust | Turner et al. 2023 (arXiv:2308.10248), Panickssery et al. 2023 (arXiv:2312.06681, CAA) |
| Facet-level persona control is now SOTA at 30+ Big-Five facets | "Facet-Level Persona Control" 2026 (arXiv:2602.19157) |
| Behavioral steering on production-scale MoE is reproducible | "Behavioral Steering in a 35B MoE LM via SAE-Decoded Probe Vectors" 2026 (arXiv:2603.16335) |
| Dynamic SAE steering for preference alignment ships at inference time | "DSPA" 2026 (arXiv:2603.21461) |

This initiative is **strictly additive** to existing surfaces. No `hu_provider_t` caller pays a price if the new optional method is `NULL`.

## Proof bar (mapped to master coordinator D0–D7)

| Gate | Requirement | This doc |
|---|---|---|
| D0 | File exists with YAML frontmatter; links resolve | ✅ this file |
| D1 | Maps to `include/human/*.h` vtable additions; names per naming standard | §1 C-API surface |
| D2 | Lists every file to create/modify with LoC estimate | §3 file map |
| D3 | ≥1 unit test (deterministic), ≥1 integration test, optional fuzzer | §5 test plan |
| D4 | Top 3 risks with mitigations | §6 risk register |
| D5 | ≥2 arXiv refs with IDs | §8 references (5 refs) |
| D6 | Binary-budget delta in MinSizeRel + RSS | §7 binary budget |
| D7 | Defer / descope condition | §9 |

## 1) C-API surface

All additions are **optional** (vtable method may be `NULL`) and follow `hu_<module>_<action>` per `docs/standards/engineering/naming.md`.

### 1.1 New `hu_provider_vtable_t` optional method

Added to the existing optional triple block at the bottom of `hu_provider_vtable_t` in `include/human/provider.h`:

```c
/* Init-01 — activation steering. Optional. Cloud providers leave NULL;
 * on-device providers (llamacpp, embedded, future mlx_qwen3) implement.
 *
 * `vec` is a small abstract trait-coefficient vector with `dim` floats.
 * The persona/personal-model layer produces it via
 * `hu_persona_steering_vector(...)`. The provider is responsible for
 * mapping each coefficient to its model's residual stream (typically
 * via a precomputed SAE-decoder projection or CAA-style difference
 * vector at a configured layer index).
 *
 * Calling with vec==NULL or dim==0 disables steering (resets to base).
 * Returning HU_ERR_NOT_SUPPORTED is legal at any time — callers must
 * fall through to the prompt-side directive path. Implementations that
 * load steering lazily may return HU_ERR_NOT_FOUND on first call before
 * the SAE table is materialized; the caller treats that identically to
 * NOT_SUPPORTED.
 *
 * The contract is intentionally narrow:
 *   - `vec` is owned by the caller; the provider must copy any state it
 *     wants to retain across calls (same rule as load_adapter).
 *   - `dim` is bounded by HU_STEERING_VEC_MAX_DIM. Providers MUST
 *     reject larger dims with HU_ERR_INVALID_ARGUMENT — keeps the
 *     binary-budget delta knowable.
 *   - Steering vectors apply to subsequent chat()/stream_chat() calls
 *     on the same provider context until `apply_steering(NULL, 0)`
 *     resets, or a new vector overwrites. */
hu_error_t (*apply_steering)(void *ctx, const float *vec, size_t dim);
```

**Return type note.** The master coordinator's prose used `int (*apply_steering)(...)` as a starting signature. The codebase convention is uniform: every other vtable method returns `hu_error_t` (see existing `load_adapter`, `chat`, `stream_chat`). Returning `hu_error_t` keeps observability hooks consistent (`hu_error_classify`) and avoids one-off `int` ↔ `hu_error_t` plumbing. This is the only deviation from the master prose; it strengthens KISS without changing the additive-vtable contract.

### 1.2 New helper, parallel to `hu_provider_load_adapter`

```c
/* Mirrors hu_provider_load_adapter's safe-no-op pattern. Returns
 * HU_ERR_NOT_SUPPORTED when the vtable leaves apply_steering NULL.
 * Callers (agent_turn.c) treat NOT_SUPPORTED as a signal to escalate
 * the prompt-side directive weight; HU_OK means residual-stream
 * steering took the load. */
hu_error_t hu_provider_apply_steering(hu_provider_t *provider, const float *vec, size_t dim);
```

### 1.3 New persona-layer projection

New header `include/human/persona/steering.h` (added because `persona.h` is already at ~740 LoC and frontends pull it heavily — KISS says give the new surface its own header):

```c
/* Abstract trait-coefficient dim. Persona+style produce a fixed-length
 * vector of normalized [-1.0, +1.0] coefficients. Order is stable across
 * builds (binary-format friendly) and matches HU_STEERING_AXIS_*. */
#define HU_STEERING_VEC_DIM        32
#define HU_STEERING_VEC_MAX_DIM    64   /* hard cap for vtable callers */

/* Stable axis indices into the steering vector. Append-only enum so
 * older on-device providers reading newer vectors safely ignore tail
 * slots they don't have SAE decoders for. */
typedef enum hu_steering_axis {
    HU_STEERING_AXIS_WARMTH = 0,
    HU_STEERING_AXIS_FORMALITY,
    HU_STEERING_AXIS_HUMOR_DENSITY,
    HU_STEERING_AXIS_HEDGING,
    HU_STEERING_AXIS_VERBOSITY,
    HU_STEERING_AXIS_EMOJI_AFFINITY,
    HU_STEERING_AXIS_LOWERCASE_AFFINITY,
    HU_STEERING_AXIS_ABBREVIATION_AFFINITY,
    /* slots 8..31 reserved for future Big-Five facets / SAE-discovered
     * features (arXiv:2602.19157 maps 30 facets cleanly into 32). */
    HU_STEERING_AXIS__COUNT
} hu_steering_axis_t;

/* Project persona + personal-model state into a steering vector.
 *
 * Writes exactly `dim` floats into `out` (must be >= HU_STEERING_VEC_DIM).
 * Unknown axes are zeroed. Coefficients are bounded to [-1, +1].
 *
 * Derivation rules (deterministic — important for testability):
 *   - warmth        ← persona->emotional_range + personal_model->style.humor_receptivity*0.3
 *                     (warmth is the dominant trait; bump on observed humor receptivity)
 *   - formality     ← overlay->formality (mapped) + style.formality
 *   - humor_density ← persona->humor.frequency (mapped) + style.humor_receptivity
 *   - hedging       ← persona->conflict_style.confrontation_comfort (inverted)
 *   - verbosity     ← style.verbosity (with avg_message_length sanity gate)
 *   - emoji_affinity        ← style.emoji_frequency
 *   - lowercase_affinity    ← style.lowercase_ratio
 *   - abbreviation_affinity ← style.abbreviation_ratio
 *
 * Freshness-aware: axes 4..7 are multiplied by
 * hu_personal_communication_style_freshness(now), so a year-old style
 * fingerprint doesn't push the steering vector hard on a fresh
 * conversation. Persona-derived axes 0..3 are NOT freshness-gated —
 * the persona file is the user's stated intent and doesn't decay.
 *
 * NULL `p` is allowed (yields all-zero axes 0..3); NULL `m` is allowed
 * (yields all-zero style axes 4..7). NULL `out` is HU_ERR_INVALID_ARGUMENT.
 * Pure CPU; no allocation; no I/O. Safe under HU_IS_TEST. */
hu_error_t hu_persona_steering_vector(const hu_persona_t *p,
                                      const hu_personal_model_t *m,
                                      float *out, size_t dim);

/* Inverse helper for the prompt-side fallback path: given a steering
 * vector, return a freshly-allocated NUL-terminated directive snippet
 * to inject into the frontier system prompt (e.g., "Lean ~15% warmer
 * than your default register; lowercase; ~1 short sentence; never
 * formal-Latinate vocabulary."). Caller owns the buffer; free via
 * `alloc->free(... , out, *out_len + 1)`. Returns HU_OK with out=NULL,
 * out_len=0 when no axis crosses the directive-emission threshold
 * (currently |coef| >= 0.15 — keeps the directive quiet on
 * weak/uncalibrated signals). */
hu_error_t hu_persona_steering_directive(hu_allocator_t *alloc,
                                         const float *vec, size_t dim,
                                         char **out, size_t *out_len);
```

### 1.4 Frontier-prompt bundle extension

`include/human/agent/frontier_prompt.h` gains one new field on `hu_frontier_prompt_bundle_t` (additive):

```c
char *steering_dir;       /* prompt-side fallback directive, may be NULL */
size_t steering_dir_len;
```

`hu_frontier_prompt_build` is extended to populate this field iff `hu_provider_apply_steering` returned `HU_ERR_NOT_SUPPORTED` for the active provider, OR the active provider is not yet steering-capable. The bundle's `_free` path already handles NULL fields — only the free call has to be added.

### 1.5 Configuration surface

One config knob (`include/human/config.h` / `src/config.c`):

```jsonc
{
  "personalization": {
    "steering": {
      "enabled": true,                    // default true; gates the whole feature
      "force_prompt_side": false,         // skip apply_steering even when supported (A/B)
      "directive_threshold": 0.15,        // |coef| floor for prompt-side emission
      "sae_table_path": null              // optional override for on-device SAE table
    }
  }
}
```

Defaults are chosen so the feature is on, but with the directive threshold high enough that an uncalibrated personal model emits no directive and no on-device steering — the system prompt is byte-identical to today's output until the personal model is calibrated.

## 2) Component design

```
┌────────────────────────────────────────────────────────────────────┐
│                       agent_turn.c (orchestration)                 │
│  hu_persona_steering_vector(persona, personal_model, vec, DIM)     │
│      │                                                              │
│      ├── hu_provider_apply_steering(provider, vec, DIM)            │
│      │       │                                                      │
│      │       ├── llamacpp.apply_steering   → maps vec via SAE       │
│      │       │   table to residual-stream addition at layer L       │
│      │       │   (Panickssery 2023 CAA pattern; arXiv:2312.06681)   │
│      │       │                                                      │
│      │       ├── embedded.apply_steering  → forwards vec to         │
│      │       │   llama-cli via JSON flag (HU_GATEWAY_POSIX only;    │
│      │       │   not yet implemented when llama-cli lacks the      │
│      │       │   --steering flag — returns NOT_SUPPORTED)           │
│      │       │                                                      │
│      │       └── mlx_qwen3.apply_steering  → (init-04 owns this)   │
│      │       │                                                      │
│      │       └── openai / anthropic / gemini / ollama / ...        │
│      │              all leave apply_steering = NULL                 │
│      │                                                              │
│      ├── if NOT_SUPPORTED: hu_persona_steering_directive(vec)       │
│      │       │                                                      │
│      │       └── injected into hu_frontier_prompt_bundle_t          │
│      │           .steering_dir → appended to system prompt          │
│      │           between the "humanness" block and the              │
│      │           "personal model" block (frontier_prompt.c)         │
│      │                                                              │
│      └── chat() / stream_chat() runs as today                       │
└────────────────────────────────────────────────────────────────────┘
```

Key boundary: the persona/personal-model layer does **not** know the on-device model's hidden dimension. It produces a `HU_STEERING_VEC_DIM`-wide abstract vector. The on-device provider owns the projection `R^32 → R^{d_model}` via a per-model SAE-decoder lookup table. Cloud providers never see the vector at all (they get the prompt-side directive).

## 3) Files to create / modify (LoC estimates)

### Create

| Path | LoC | Purpose |
|---|---|---|
| `include/human/persona/steering.h` | ~90 | Public header for `hu_persona_steering_vector`, `_directive`, axis enum, dim macros |
| `src/persona/steering.c` | ~280 | Projection + directive builder; deterministic, no I/O, no allocation in hot path (directive uses `hu_alloc`) |
| `tests/test_persona_steering.c` | ~320 | Unit tests for projection (NULL handling, axis order, freshness, clamping) + directive renderer (threshold gate, empty-case, axis-priority ordering) |
| `tests/test_provider_steering_dispatch.c` | ~180 | Vtable dispatch: cloud providers return NOT_SUPPORTED; llamacpp stub records vec; HU_IS_TEST mocks; ensures `apply_steering(NULL, 0)` reset is honored |
| `tests/fixtures/steering_axes.json` | ~60 | Golden vectors for warmth/formal/casual/humorous personas (asserted exactly) |
| `docs/standards/ai/activation-steering.md` | ~120 | Standard: when to steer, what the axes mean, fidelity-eval methodology |

### Modify

| Path | Net LoC | Change |
|---|---|---|
| `include/human/provider.h` | +30 | New optional `apply_steering` vtable method + `HU_STEERING_VEC_MAX_DIM` macro + `hu_provider_apply_steering` helper declaration |
| `src/providers/helpers.c` | +25 | `hu_provider_apply_steering` helper (mirrors `hu_provider_load_adapter` pattern: NULL-method → HU_ERR_NOT_SUPPORTED) |
| `src/providers/llamacpp.c` | +180 | `apply_steering` impl: keep `float steering_vec[HU_STEERING_VEC_DIM]` in ctx, lazy-load SAE decoder table from `sae_table_path`, project to `d_model` and call llama.cpp's per-layer activation hook. Behind `HU_LLAMACPP_LINKED`; otherwise return NOT_SUPPORTED |
| `src/providers/embedded.c` | +60 | `apply_steering` stub returning NOT_SUPPORTED today (llama-cli has no steering flag); placeholder so the vtable slot is wired and a future llama-cli build with `--control-vector` flips it on |
| `src/providers/factory.c` | +12 | Register `apply_steering` in vtables for `llamacpp`, `embedded`; cloud factories untouched (NULL = no-op) |
| `src/agent/frontier_prompt.c` | +90 | New `build_steering` block; integrates between `build_humanness_ctx` and existing personal-model wiring. Always calls `hu_persona_steering_vector`; calls `hu_provider_apply_steering` once per turn; only emits directive when provider returned NOT_SUPPORTED |
| `include/human/agent/frontier_prompt.h` | +4 | New bundle field |
| `src/agent/agent_turn.c` | +35 | Thread `steering_dir` into the system-prompt composition site (next to existing `humanness_context`); already-existing free path picks up the new bundle field |
| `include/human/config.h` | +15 | New `hu_steering_config_t` substruct on `hu_personalization_config_t` |
| `src/config.c` | +45 | Parse + validate the new JSON subtree; defaults match §1.5 |
| `tests/test_config_parse.c` | +40 | Round-trip tests for the new keys; defaults assertion |
| `tests/test_frontier_prompt.c` (existing) | +60 | New cases: NOT_SUPPORTED → directive injected; HU_OK → no directive; force_prompt_side=true → directive even when supported |
| `CMakeLists.txt` | +6 | Add `src/persona/steering.c` and the two new test files to the source/test lists |

### Optional (Sprint+1 if budget allows)

| Path | LoC | Purpose |
|---|---|---|
| `fuzz/fuzz_steering_directive.c` | ~120 | libFuzzer harness on `hu_persona_steering_directive` (NULL vecs, out-of-range coefs, dim mismatches) |
| `tools/sae-tables/qwen3-4b-sae.bin` | binary | Per-model SAE decoder table; NOT vendored (loaded from `~/.human/sae/`); manifest in `docs/sae-tables.md` |

**Total new C LoC**: ~1,030 (production ~600, tests ~370 — healthy ratio per `docs/standards/quality/governance.md`).

## 4) Data flow

```
1. agent_turn.c receives a user message.
2. hu_frontier_prompt_build() is invoked (existing call).
3. build_steering() runs:
   a. Calls hu_persona_steering_vector(persona, personal_model, vec, 32).
   b. If config.steering.enabled == false → skip.
   c. Calls hu_provider_apply_steering(active_provider, vec, 32):
      - llamacpp: copies vec into ctx; on next chat() decode, applies
        residual-stream addition at configured layer index L using the
        SAE-decoder projection (loaded lazily on first non-zero call).
      - embedded / cloud: returns HU_ERR_NOT_SUPPORTED.
   d. If NOT_SUPPORTED OR config.steering.force_prompt_side:
      hu_persona_steering_directive(vec) → bundle.steering_dir.
4. agent_turn.c composes the system prompt, splicing bundle.steering_dir
   between the humanness directive block and the personal-model directive
   block (same pattern as existing imperfect_dir, residue_dir).
5. provider chat()/stream_chat() runs against the now-steered model.
6. On turn end, bundle is freed (existing path picks up the new field).
```

The function `hu_persona_steering_vector` is called **once per turn**, not once per token. The vector lives in the provider's `ctx` across the chat() boundary and is reset by `apply_steering(NULL, 0)` only when the persona changes (a rare event).

## 5) Test plan

Tests follow `subject_expected_behavior` naming (per `docs/standards/engineering/naming.md`).

### Unit (deterministic, no I/O, no network)

| Test name | Suite | Asserts |
|---|---|---|
| `test_persona_steering_vector_zero_persona_yields_zero` | `persona_steering` | NULL persona + NULL model → all-zero vector, HU_OK |
| `test_persona_steering_vector_axis_order_is_stable` | `persona_steering` | A canned persona produces bit-identical vector across runs; gold-file compare against `tests/fixtures/steering_axes.json` |
| `test_persona_steering_vector_clamps_to_unit_interval` | `persona_steering` | Even with extreme inputs, every coef ∈ [-1, +1] |
| `test_persona_steering_vector_freshness_decays_style_axes` | `persona_steering` | After 360 days of `last_observed_at`, axes 4..7 are ≥50% closer to 0 than at observation time; persona-derived 0..3 untouched |
| `test_persona_steering_vector_rejects_small_dim` | `persona_steering` | `dim < HU_STEERING_VEC_DIM` returns HU_ERR_INVALID_ARGUMENT |
| `test_persona_steering_directive_threshold_gates_emission` | `persona_steering` | All-zero vec → out=NULL, out_len=0, HU_OK; vec with `warmth=0.5` → non-NULL directive containing "warmth" axis language |
| `test_persona_steering_directive_orders_axes_by_magnitude` | `persona_steering` | Strongest axis appears first in the directive (readability) |
| `test_provider_apply_steering_null_vtable_returns_not_supported` | `provider_steering_dispatch` | OpenAI/Anthropic/Gemini provider ctx → NOT_SUPPORTED |
| `test_provider_apply_steering_llamacpp_disabled_returns_not_supported` | `provider_steering_dispatch` | When `HU_ENABLE_LLAMACPP=OFF`, llamacpp returns NOT_SUPPORTED |
| `test_provider_apply_steering_reset_clears_vec` | `provider_steering_dispatch` | Stub provider receives non-zero vec, then `apply_steering(NULL, 0)`; ctx state shows zero vec |
| `test_provider_apply_steering_rejects_oversize_dim` | `provider_steering_dispatch` | `dim > HU_STEERING_VEC_MAX_DIM` → HU_ERR_INVALID_ARGUMENT |
| `test_frontier_prompt_emits_steering_dir_when_provider_not_supported` | `frontier_prompt` | After provider returns NOT_SUPPORTED, bundle.steering_dir is non-NULL |
| `test_frontier_prompt_no_steering_dir_when_provider_supported` | `frontier_prompt` | After provider returns HU_OK, bundle.steering_dir is NULL |
| `test_frontier_prompt_force_prompt_side_emits_dir_anyway` | `frontier_prompt` | `force_prompt_side=true` → directive emitted even when apply_steering returned HU_OK |
| `test_config_parse_steering_defaults` | `config_parse` | Missing `personalization.steering` block → defaults from §1.5 |
| `test_config_parse_steering_round_trip` | `config_parse` | All four keys round-trip cleanly through parse + serialize |

### Integration (deterministic, HU_IS_TEST guards)

| Test name | Suite | Path |
|---|---|---|
| `test_agent_turn_steering_dispatch_end_to_end` | `agent_turn` | Boots an agent with a mocked provider that records every `apply_steering` call; runs one turn; asserts (a) projection ran, (b) vector hit the provider, (c) reset on persona-swap |
| `test_agent_turn_steering_cloud_falls_back_to_prompt_side` | `agent_turn` | Boots an agent with the cloud-style mock provider (NULL `apply_steering`); asserts system prompt contains the directive substring; asserts no SAE-table I/O occurred |

Both integration tests run under `HU_IS_TEST=1` so no real provider call, no network, no llama.cpp linkage required.

### Optional fuzzer (parked behind D7; not required for D3 pass)

`fuzz/fuzz_steering_directive.c` — libFuzzer over `hu_persona_steering_directive` argument space. Catches: stack-buffer overruns on long persona-name interpolation, double-free on threshold-exact zero vec, integer overflow on `dim`.

### What we deliberately do NOT test

- Real on-device SAE-decoder math (lives behind `HU_LLAMACPP_LINKED`; covered by `init-04 mlx-qwen3-provider` and the Phase-1 llama.cpp smoke gates, not here).
- Cross-model SAE generalization (init-04 owns).
- End-to-end persona fidelity uplift in production (Track D D2.2 fidelity scorer in `src/ml/fidelity.c` is the right surface; this initiative ships the steering vector, not the eval loop).

## 6) Risk register

| # | Risk | Likelihood | Severity | Mitigation |
|---|---|---|---|---|
| 1 | **SAE-decoder table availability**: per-model SAE features for warmth/formality/humor may not exist in publishable form for Qwen3-4B / Gemma-3 when Sprint+2 lands. Without them, the on-device path is dead and the initiative collapses to prompt-side directives only. | **High** (this is THE driver of the D7 defer condition) | High | (a) Ship the abstract-vector contract first — the same `hu_persona_steering_vector` output feeds both paths. (b) Bridge with **CAA-style** difference vectors (arXiv:2312.06681) trained on h-uman's persona-banks instead of waiting for academic SAEs. (c) If neither lands by Sprint+2, defer per §9 and ship prompt-side alone. |
| 2 | **Binary-budget breach on linking**: the on-device SAE decoder table for a single 4B model is ~16 KB at FP16 per layer × N layers — if naively vendored, the static asset blows the 20 KB ceiling (D6). | Medium | Medium | (a) Treat the SAE table as a **runtime resource**, not a compiled-in blob. Loaded from `~/.human/sae/<model>.bin` on first non-zero steering call. (b) Default `personalization.steering.enabled = true` with `directive_threshold = 0.15` — but only the projection code (~280 LoC + ~2 KB rodata) lands in the binary unconditionally. (c) Hard-cap `HU_STEERING_VEC_MAX_DIM = 64`; reject larger requests at the vtable boundary. |
| 3 | **Persona drift / regression**: a poorly-calibrated steering vector could push the cloud model's outputs **away** from persona faithfulness (e.g., humor=+0.9 when the user is in a serious mood). The `imperfect_delivery` and `affect_mirror_ceiling` mechanisms already exist for this — but they were never designed to compose with residual-stream addition. | Medium | High (user-facing quality regression) | (a) **Freshness gating** on style axes (per `hu_personal_communication_style_freshness`) — uncalibrated style produces a near-zero vector. (b) **Threshold gate** (`directive_threshold` default 0.15) — weak signals never reach the system prompt. (c) **Track D D2.2 fidelity scorer** (`hu_communication_style_fidelity_score` in `src/ml/fidelity.c`) regression-gates every release: pre-steering vs post-steering mean fidelity must not regress. Failure flips `steering.enabled=false` via release-gate config patch. (d) `affect_mirror_ceiling` applies BEFORE projection — humor axis is clamped by `hu_affect_mirror_apply` in `hu_persona_steering_vector`, not afterward. |

Honorable mentions (tracked but not in the top-3):

- **Provider-context lifetime**: `apply_steering` mutates ctx state across chat() boundaries. Misuse pattern is "call apply_steering on a temporary ctx that the next chat() doesn't see." Same `void *ctx` ownership rule as `load_adapter`; documented in the vtable comment.
- **ASan on Metal**: when llamacpp + `HU_LLAMACPP_METAL=ON`, the SAE-decoder math runs on GPU. ASan can't see Metal allocations. Mitigated by keeping the projection layer (`R^32 → R^{d_model}`) on the CPU side; only the residual-stream addition crosses to Metal.

## 7) Binary-budget delta

Target: MinSizeRel + LTO build (`cmake --preset release`). Baseline today: ~1750 KB.

| Item | Delta | Notes |
|---|---|---|
| `src/persona/steering.c` text | ~1.6 KB | Pure CPU, no SIMD; LTO inlines into agent_turn.c hot path |
| `hu_persona_steering_vector` rodata (axis names, thresholds) | ~0.4 KB | Stable strings for the directive renderer |
| `hu_provider_apply_steering` helper | ~0.1 KB | Mirrors `hu_provider_load_adapter` (~0.1 KB observed in size build) |
| `frontier_prompt.c` build_steering block | ~0.5 KB | One branch + one allocator call per turn |
| Provider `apply_steering` stubs (embedded.c, cloud factories' NULL slots) | ~0.2 KB | NULL slots are free; the embedded stub is ~30 instructions |
| Config-parse additions | ~0.3 KB | JSON keys are short |
| `llamacpp.c` SAE-decoder math (behind `HU_LLAMACPP_LINKED`) | ~3.0 KB | **Not in the default release binary**. Only present when `HU_ENABLE_LLAMACPP=ON`, which is opt-in. |
| Per-persona steering-vec storage in provider ctx | 32 × 4 B = 128 B | Far under the 2 KB-per-persona ceiling |
| SAE feature table (rodata strings for prompt-side fallback) | ~3.1 KB | Axis labels + per-axis directive templates; constexpr; LTO-cold |
| **Total default release delta** | **≈ 5.6 KB** | Well under the 20 KB ceiling |
| **Total with `HU_ENABLE_LLAMACPP=ON`** | **≈ 8.6 KB** | Still well under |

Runtime RSS impact: +128 B per active provider context (steering vec); +~2 KB transient per turn for the directive string (freed at bundle teardown). Steady-state RSS delta: **negligible** (<5 KB). The 4 GB SAE-decoder table for a 4B model is loaded only on first non-zero `apply_steering` call AND only when `HU_ENABLE_LLAMACPP=ON` — that's a per-model file footprint, not a process footprint, and lives outside the binary anyway (loaded from `~/.human/sae/`).

**Ceiling.** This initiative MUST stay under 20 KB binary delta and 5 KB steady-state RSS delta. Crossing those triggers an automatic defer per §9.

## 8) References

| ID / DOI | Paper | Relevance |
|---|---|---|
| arXiv:2308.10248 | Turner, M., Thiergart, L., Udell, D., Leech, G., et al. (2023) "Steering Language Models with Activation Engineering" | Founding result that residual-stream addition reliably steers behavior at small scale. Directly informs `apply_steering` semantics. |
| arXiv:2312.06681 | Panickssery, N., Gabrieli, N., Schulz, J., Tong, M., Hubinger, E., Turner, A. (2023) "Steering Llama 2 via Contrastive Activation Addition" | CAA — the practical recipe for producing the steering vectors h-uman would use as a fallback when SAE features aren't available. Open-source reference impl at `github.com/nrimsky/CAA`. |
| arXiv:2309.08600 | Cunningham, H., Ewart, A., Riggs, L., Huben, R., Sharkey, L. (2023) "Sparse Autoencoders Find Highly Interpretable Features in Language Models" | Establishes that SAEs reliably extract human-interpretable features (incl. style/persona traits) from LM residual streams. Underpins the SAE-feature-table design. |
| arXiv:2602.19157 | (2026) "Facet-Level Persona Control by Trait-Activated Routing with Contrastive SAE for Role-Playing LLMs" | SOTA for fine-grained persona-axis control via contrastive SAEs; informs the 32-axis dim choice and the warmth/formality/humor/hedging axis selection. |
| arXiv:2603.16335 | (2026) "Behavioral Steering in a 35B MoE Language Model via SAE-Decoded Probe Vectors" | Important honesty signal: at scale, multiple "trait" vectors may collapse onto a single dominant axis. Motivates the conservative directive-threshold + freshness-gating defaults. |
| Templeton, A. et al. (2024) "Scaling Monosemanticity" — `transformer-circuits.pub/2024/scaling-monosemanticity` | Anthropic | Canonical proof that SAEs scale to frontier-class models (Claude 3 Sonnet) and isolate features like "warmth", "anger", "obsequiousness". |
| arXiv:2603.21461 | (2026) "DSPA: Dynamic SAE Steering for Data-Efficient Preference Alignment" | Inference-time SAE steering at minimal data cost; relevant to a future "personalize-during-conversation" extension but explicitly **out of scope** for init-01. |

## 9) Defer / descope condition

The activation-side path is **deferred** to a future sprint if, by the end of **Sprint SOTA-2026-01 + 2 sprints** (i.e., the two implementation sprints following design adoption), any of the following is true:

1. **No usable SAE decoder table** exists for any model h-uman ships on-device (Qwen3-4B / Gemma-3-4B / similar). "Usable" means: at least four of the eight named axes (warmth, formality, humor, hedging, verbosity, emoji, lowercase, abbreviation) are isolable with d-prime ≥ 0.5 on h-uman's own A/B persona-fidelity eval (Track D D2.2, `hu_communication_style_fidelity_score`).
2. **The CAA bridge can't be trained on h-uman's persona banks** at the volume currently available (`hu_persona_banks_extract_from_history` typically yields 50–500 pairs per channel — if that's < the minimum to fit a discriminating direction, the bridge fails).
3. **Binary-budget breach**: the activation-side path crosses the 20 KB binary delta or 5 KB steady-state RSS delta ceiling (D6) and can't be brought back under without breaking the additive-vtable contract.
4. **Fidelity-scorer regression**: the post-steering mean of `hu_communication_style_fidelity_score` over the eval set drops by > 0.02 vs the prompt-side-only baseline. This means activation steering is actively hurting persona fidelity instead of helping; release-gate flips `steering.enabled=false`.

When **deferred**, the prompt-side fallback ships **standalone**:

- `hu_persona_steering_vector` ships (the projection is useful even when only the directive is consumed).
- `hu_persona_steering_directive` ships and is wired into `frontier_prompt.c`.
- The `apply_steering` vtable slot is added but every provider leaves it `NULL` for one more sprint cycle.
- The defer is logged in this doc's frontmatter (`status: design done, on-device path deferred to Sprint NN`), and the master coordinator's status table is updated per its rules.

When **descoped entirely** (worst case — neither path is useful), the directive emission threshold is set to 1.0 (effectively off), the vtable slot is removed in the next API churn window, and the `hu_persona_steering_vector` projection is kept private (still used as a feature vector for the Track D fidelity scorer but no longer wired into prompts).

The non-negotiable: this initiative **does not break** any existing `hu_provider_t` caller. If the only safe outcome is "ship nothing," the additive-vtable contract guarantees we ship nothing harmful — every modified file's diff collapses to a NULL slot.

## 10) Open questions (escalated to sprint planning)

1. **SAE decoder table sourcing**: do we train our own on h-uman's persona banks (privacy story stays clean, but ~weeks of compute and the bridge depends on a working M3 Bridge A), use Anthropic's published Claude-Sonnet features as a starting heuristic (privacy story is fine since we never call the SAE at runtime against user data — the decoder is static rodata-ish), or wait for a Qwen3-4B / Gemma-3 SAE release? **This is the single biggest open question driving the defer condition.**
2. Which layer index does CAA target on llama.cpp's API for Qwen3-4B? llama.cpp `b9055+` exposes per-layer hooks, but the "right" layer for trait steering is empirically usually `L = ⌊2/3 · n_layers⌋`. Owner: init-04 (mlx-qwen3-provider) — this initiative provides the abstract contract; init-04 owns the per-model tuning.
3. Composition with `init-02 molora-channels`: a per-channel LoRA expert AND a steering vector are both modulating the same residual stream. Which wins on conflict? Proposed: steering vector applies AFTER LoRA merge (LoRA changes the weights; steering changes the activations). Validate in init-02's design doc.

---

**End of init-01 design doc.** Status: `design done`, ready for sprint adoption.
