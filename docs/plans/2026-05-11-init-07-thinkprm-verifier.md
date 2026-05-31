---
title: "Initiative 07 — ThinkPRM trained verifier panel"
created: 2026-05-11
status: deferred
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-11-full-sota-rl-improvement-loop-design.md
  - 2026-05-10-m3-frontier-model-bridge.md
  - 2026-05-10-master-follow-through-program.md
  - ../../CLAUDE.md
  - ../../AGENTS.md
  - ../standards/ai/evaluation.md
  - ../standards/ai/hallucination-prevention.md
  - ../standards/security/threat-model.md
  - ../../include/human/agent/process_reward.h
  - ../../include/human/agent/reflection.h
  - ../../include/human/agent/response_verifier.h
  - ../../include/human/ml/fidelity.h
  - ../../include/human/ml/dpo.h
last_audit: 2026-05-25
---

# Initiative 07 — ThinkPRM trained verifier panel

> Replace the prompt-based critic chain on the response path with a small
> trained Process Reward Model (PRM) calibrated on h-uman's own DPO data,
> running on-device behind `HU_VERIFIER_TRAINED=1`. The PRM is a new
> `hu_reward_model_t` vtable backend; existing prompt critics remain as
> the default, swappable backend for staged rollout. The C-side delta is
> ≤ 8 KB; the model artifact (~250–400 MB) lives outside the binary.

## TL;DR

Today the agent's "verifier panel" is three independent prompt-driven heuristics
running on every response:

1. `hu_prm_score_chain` / `hu_prm_score_step` (`src/agent/process_reward.c`) —
   keyword/digit/connector heuristic that returns a 0..1 score.
2. `hu_reflection_evaluate_llm` (`src/agent/reflection.c`) — sends the response
   back to the same provider asking "is this answer OK?" with a rubric prompt.
3. `hu_response_verify` (`src/agent/response_verifier.c`) — claim-level
   memory-grounded verifier (W4). **NOT replaced here** — this is grounded in
   the user's memory graph and is structurally different from a process reward.

Items 1 and 2 are the "prompt critic" problem. April 2026 arXiv literature shows
that 0.5B–1.5B trained PRMs (ThinkPRM, Athena-PRM) beat prompt critics on both
calibration and noise-robustness, and that the same architecture can be trained
from already-collected DPO-style preference data plus synthetic rubric-violation
pairs. This initiative ships the design for that swap.

The C-side surface is one new header (`include/human/ml/reward_model.h`),
one factory (`src/ml/reward_model.c`), and three backends (`prompt_critic`,
`heuristic`, `thinkprm`). The training path piggy-backs on the M3 frontier
bridge (init-04 MLX provider / init-06 RL trainers) — no new MLX subprocess
scaffolding; the PRM is just another LoRA-style head on a 0.5B base.

---

## D0 — File / status

This document satisfies the master coordinator's D0 gate:

- File: `docs/plans/2026-05-11-init-07-thinkprm-verifier.md` (this file)
- Frontmatter: present, links resolve to existing docs and headers
- Status: `design done` (flip to `sprint open` when Sprint SOTA-2026-01 adopts)

## D1 — Vtable / API surface

### `hu_reward_model_t` — new public vtable (`include/human/ml/reward_model.h`)

```c
// include/human/ml/reward_model.h — ~80 LOC, declarations only
#ifndef HU_ML_REWARD_MODEL_H
#define HU_ML_REWARD_MODEL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include "human/memory/personal_model.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PRM scoring context — caller-owned, lives for the duration of one call. */
typedef struct hu_reward_score_ctx {
    const char *prompt;            /* user turn (utf-8) */
    size_t prompt_len;
    const char *response;          /* candidate response */
    size_t response_len;
    const char *channel;           /* e.g. "imessage", optional */
    size_t channel_len;
    int64_t now_ms;                /* 0 = use OS clock */
    const hu_persona_t *persona;   /* optional, not owned */
    const hu_personal_model_t *personal_model; /* optional */
} hu_reward_score_ctx_t;

/* Per-step (process) scoring: one score per detected reasoning step in `chain`. */
typedef struct hu_reward_step_score {
    size_t step_offset;            /* byte offset into `chain` */
    size_t step_len;
    float score;                   /* 0..1 */
    float confidence;              /* 0..1 — width of the model's posterior */
} hu_reward_step_score_t;

typedef struct hu_reward_step_result {
    hu_reward_step_score_t *steps; /* allocator-owned, free via _free */
    size_t step_count;
    float aggregate;               /* product^(1/n), per ThinkPRM */
    float calibration_temperature; /* applied during _free, exposed for telemetry */
} hu_reward_step_result_t;

struct hu_reward_model_vtable;

typedef struct hu_reward_model {
    void *ctx;
    const struct hu_reward_model_vtable *vtable;
} hu_reward_model_t;

typedef struct hu_reward_model_vtable {
    const char *(*name)(void *ctx);              /* "prompt", "heuristic", "thinkprm-q0.5b" */
    bool        (*available)(void *ctx);         /* checkpoint loaded? subprocess up? */
    int64_t     (*budget_p50_us)(void *ctx);     /* reported wall-clock budget; <= 30_000 on M3 Max */

    /* Required: turn-level score. Must never block > budget_p50_us * 5. */
    hu_error_t  (*score)(void *ctx, hu_allocator_t *alloc,
                         const hu_reward_score_ctx_t *sctx, float *out);

    /* Required: per-step score over a reasoning chain (\n\n or list-item delimited). */
    hu_error_t  (*score_steps)(void *ctx, hu_allocator_t *alloc,
                               const hu_reward_score_ctx_t *sctx,
                               const char *chain, size_t chain_len,
                               size_t k_steps_cap,
                               hu_reward_step_result_t *out);

    /* Required: free a step result. */
    void        (*step_result_free)(void *ctx, hu_allocator_t *alloc,
                                    hu_reward_step_result_t *result);

    /* Optional. NULL on backends that do not maintain a checkpoint. */
    hu_error_t  (*reload_checkpoint)(void *ctx, const char *path, size_t path_len);
    hu_error_t  (*audit)(void *ctx, hu_allocator_t *alloc,
                         const char *probe_set_path, size_t probe_set_path_len,
                         char **out_report_json, size_t *out_len);

    void        (*deinit)(void *ctx);
} hu_reward_model_vtable_t;

/* Public helpers — `hu_<module>_<action>` per AGENTS.md §6.1. */
hu_error_t hu_reward_model_score(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                 const hu_reward_score_ctx_t *sctx, float *out);
hu_error_t hu_reward_model_score_steps(hu_reward_model_t *rm, hu_allocator_t *alloc,
                                       const hu_reward_score_ctx_t *sctx,
                                       const char *chain, size_t chain_len,
                                       size_t k_steps_cap,
                                       hu_reward_step_result_t *out);
void       hu_reward_model_step_result_free(hu_reward_model_t *rm,
                                            hu_allocator_t *alloc,
                                            hu_reward_step_result_t *result);

/* Factory. `backend` ∈ {"prompt", "heuristic", "thinkprm"}. Resolves from
 * config when `backend == NULL`; honors `HU_VERIFIER_TRAINED` env var. */
hu_error_t hu_reward_model_create(hu_allocator_t *alloc,
                                  const char *backend, size_t backend_len,
                                  const char *checkpoint_path, size_t checkpoint_path_len,
                                  struct hu_provider *fallback_provider,
                                  hu_reward_model_t *out);
void       hu_reward_model_deinit(hu_reward_model_t *rm);

#endif /* HU_ML_REWARD_MODEL_H */
```

Naming follows `docs/standards/engineering/naming.md`: `hu_<module>_<action>`,
`hu_<name>_t`, snake_case fields. Vtable lives at the `ml` boundary because
training is the dominant lifecycle concern; `agent/` consumes it through the
opaque pointer.

### Inward dependency direction

```
agent/agent_turn.c
        │ (consumes opaque hu_reward_model_t*)
        ▼
ml/reward_model.c   ─►  ml/reward_model_prompt.c   ─►  providers/  (LLM judge fallback)
                  ├──►  ml/reward_model_heuristic.c (wraps existing hu_prm)
                  └──►  ml/reward_model_thinkprm.c  ─►  providers/llamacpp.c or mlx (init-04)
```

`ml/` may reach `providers/` (already true for `dpo.c`). `agent/` does **not**
reach `ml/reward_model_*.c` directly — only the vtable. This preserves the
"wall that keeps the inference path lean" from §3.3 of the RL loop design.

## D2 — File list (create / modify, LOC estimate)

### Create (10 source/header files, ~7 KB compiled C-side delta)

| # | Path | LOC | Compiled (MinSizeRel+LTO, est.) |
|---|------|-----|-------------------------------|
| 1 | `include/human/ml/reward_model.h` | ~85 | 0 (declarations) |
| 2 | `src/ml/reward_model.c` (vtable + factory + env-var routing) | ~180 | ~2.0 KB |
| 3 | `src/ml/reward_model_heuristic.c` (wraps existing `hu_prm_score_*`) | ~80 | ~0.6 KB |
| 4 | `src/ml/reward_model_prompt.c` (wraps `hu_reflection_evaluate_llm` + rubric prompt) | ~140 | ~1.4 KB |
| 5 | `src/ml/reward_model_thinkprm.c` (loads PRM checkpoint, dispatches via llamacpp/MLX) | ~220 | ~2.8 KB |
| 6 | `src/ml/cli_prm.c` (`human ml prm-train`, `human ml prm-audit`) | ~280 | ~3.4 KB (gated `HU_ENABLE_ML`) |
| 7 | `scripts/mlx_trainer/prm_train.py` (PRM training entrypoint reusing init-06 MLX subprocess) | ~250 LOC Python | 0 |
| 8 | `tests/test_reward_model_factory.c` | ~120 | test-only |
| 9 | `tests/test_reward_model_heuristic_parity.c` | ~100 | test-only |
| 10 | `tests/test_reward_model_thinkprm_calibration.c` (gated `HU_HAVE_PRM_CKPT=1`) | ~180 | test-only |
| 11 | `tests/test_reward_model_audit_mode.c` | ~140 | test-only |
| 12 | `tests/fixtures/prm/rubric_v1.json` (rubric used for synthetic adversarial pairs) | ~120 | fixture |
| 13 | `tests/fixtures/prm/calibration_v1.jsonl` (held-out persona-fidelity calibration set, 200 turns) | ~600 | fixture |
| 14 | `tests/fixtures/prm/audit_probes_v1.jsonl` (reward-hacking probes — see D4) | ~150 | fixture |

**Default release C-side delta: ≤ 4 KB.** With `HU_ENABLE_ML` (release path that
includes ML CLIs): ≤ 8 KB. The 8 KB budget in the master coordinator is the
upper bound; we hit ~7.4 KB at LTO, with headroom for one more backend.

### Modify (8 source files, line-bounded deltas)

| # | Path | Delta | Purpose |
|---|------|-------|---------|
| 1 | `include/human/agent.h` | +6 lines | Add `hu_reward_model_t reward_model;` to `hu_agent_extensions_t` (next to `prm_config`). Default to `{NULL,NULL}`. |
| 2 | `src/agent/agent_turn.c:5101-5138` | ~25 lines net | Replace the four `hu_prm_score_*` + `hu_reflection_evaluate_llm` calls with `hu_reward_model_score` / `hu_reward_model_score_steps` dispatch. See D2.5 for exact mapping. |
| 3 | `src/agent/agent_turn.c:6216-6280` | ~10 lines | Same swap on the self-improve / consistency paths. |
| 4 | `src/agent/agent.c` (init / deinit) | +12 lines | Create `agent->sota.reward_model` from `config.personalization.reward_model` (default `"prompt"`); honor `HU_VERIFIER_TRAINED` env var. |
| 5 | `src/agent/reflection.c` | +30 lines | Add `hu_reflection_evaluate_via_reward_model()` so the reflection retry path can consume the same backend as the PRM. Old `hu_reflection_evaluate_llm` kept as a deprecated shim that the new prompt backend wraps. |
| 6 | `include/human/ml/cli.h` | +2 lines | Declare `hu_ml_cli_prm_train`, `hu_ml_cli_prm_audit`. |
| 7 | `src/ml/cli.c` | +20 lines | Dispatch `prm-train` and `prm-audit` to `cli_prm.c`. |
| 8 | `src/config/config.c` + `include/human/config.h` | +20 lines | Add `personalization.reward_model.backend` and `personalization.reward_model.checkpoint_path` (defaults: `"prompt"`, `""`). Parse path tilde-expansion via existing helper. |
| 9 | `CLAUDE.md` "M3 row" + "Personalization status" | +6 lines | Document the new `HU_VERIFIER_TRAINED` env var and the calibration gate honestly. |

**Total new code (C only): ~13–14 KB source, ~7.4 KB compiled at LTO.** Within
budget.

### Call-site map — exact replacements

| Call site (file:line) | Today | After (replacement behavior) |
|---|---|---|
| `src/agent/agent_turn.c:5106` `hu_prm_score_chain` | heuristic over reasoning chain when `resp.content_len > 100` | `hu_reward_model_score_steps(rm, alloc, &sctx, resp.content, resp.content_len, 64, &result)` — `result.aggregate` becomes `prm_turn_score`. |
| `src/agent/agent_turn.c:5117` `hu_prm_score_step` | heuristic over the whole response when `> 50` chars | `hu_reward_model_score(rm, alloc, &sctx, &step_score)` — turn-level score. |
| `src/agent/agent_turn.c:5128` `hu_reflection_evaluate_llm` | LLM-as-judge prompt critic, only when heuristic says `ACCEPTABLE` and `use_llm` is on | `hu_reflection_evaluate_via_reward_model(alloc, rm, &sctx, &result)` — when `result < 0.35` flip to `HU_QUALITY_NEEDS_RETRY`; otherwise `HU_QUALITY_GOOD`. |
| `src/agent/agent_turn.c:5133` PRM quality gate | hard 0.3 threshold on heuristic step score | Replaces with `result.aggregate < cfg.retry_threshold` (default 0.35, persona-overridable). |
| `src/agent/agent_turn.c:6216`, `:6269` self-improve heuristic path | `hu_reflection_evaluate` (heuristic) | Unchanged — heuristic is the only synchronous fallback when the reward model is unavailable. The PRM backend writes a turn-level score *alongside* the heuristic for observation. |
| `src/agent/response_verifier.c` | claim-level memory-grounded verifier | **Unchanged.** This verifies factual support against the W7 graph; it is structurally different from a process reward and is the only verifier the user actually sees in SOFT/STRICT mode. |
| `src/agent/agent_stream.c` mirror sites | (none today — streaming path does not run PRM) | Add a deferred score after the streaming flush completes, so streaming and non-streaming converge on the same telemetry. |

The `response_verifier` / `self_rag` path is intentionally **not** replaced. The
PRM scores stylistic + process quality (the things the prompt critic was bad at);
the response verifier checks claim support against the memory graph (the thing
prompt critics are *also* bad at, but for a different reason that's already
addressed by W4/W11). The two coexist.

## D3 — Test plan

Every backend has at least one unit test, one integration test, and a fuzz / red-team harness. Existing CI matrix from `2026-05-11-full-sota-rl-improvement-loop-design.md` §6.5 picks them up by suite name `RM-*`.

| Tier | Test | Suite | Pass criterion |
|---|---|---|---|
| T1 unit | `tests/test_reward_model_factory.c::factory_creates_each_backend` | `RM-unit` | Each of `"prompt"`, `"heuristic"`, `"thinkprm"` constructs and deinits with no ASan leak. |
| T1 unit | `tests/test_reward_model_factory.c::factory_honors_env_var` | `RM-unit` | `setenv("HU_VERIFIER_TRAINED","1")` + checkpoint present → `name()` returns `"thinkprm-*"`; checkpoint missing → falls back to `"prompt"` with a log line. |
| T1 unit | `tests/test_reward_model_heuristic_parity.c::heuristic_matches_legacy_prm` | `RM-unit` | `score_steps` on 50 fixture responses returns identical aggregates to `hu_prm_score_chain` to within 1e-6. Guards against accidental regression of the no-checkpoint path. |
| T2 grad-check | `tests/test_reward_model_thinkprm_calibration.c::trained_prm_beats_prompt_on_ece` (gated `HU_HAVE_PRM_CKPT=1`) | `RM-integration-FULL` | ECE on `calibration_v1.jsonl` for the trained PRM is ≥10 points lower than the prompt critic baseline (deterministic seed, 10-bin reliability diagram). |
| T2 grad-check | `tests/test_reward_model_thinkprm_calibration.c::brier_score_lower_than_prompt` (gated) | `RM-integration-FULL` | Brier score on the same set is < prompt critic's by ≥0.05. Catches "ECE win by miscalibrated centering" failures. |
| T3 integration | `tests/test_reward_model_audit_mode.c::audit_detects_known_hacking_pattern` | `RM-integration` | `audit()` on `audit_probes_v1.jsonl` (10 adversarial probes: short-answer reward hack, sycophancy, hedge-bombing, length-padding, emoji-stuffing) returns flags for ≥8/10. |
| T3 integration | `tests/test_reward_model_audit_mode.c::audit_drift_detector_fires_on_distribution_shift` | `RM-integration` | KL between the historical score distribution and a synthetic shifted distribution (mean shifted by +0.2) exceeds the configured threshold → `audit()` returns `"drift": true`. |
| T4 E2E | `tests/test_agent_turn_uses_reward_model.c::trained_backend_drives_retry` | `RM-E2E` | With `HU_VERIFIER_TRAINED=1` + a stub backend returning 0.1 for a known-bad response, `agent_turn` triggers a retry. With the same stub returning 0.9, no retry. |
| T4 E2E | `tests/test_agent_turn_uses_reward_model.c::prompt_backend_default_unchanged` | `RM-E2E` | Without `HU_VERIFIER_TRAINED`, the call-graph hits the prompt backend and produces a score within ±0.05 of today's `hu_reflection_evaluate_llm` output on the same fixture. Backward-compat guard. |
| T5 fuzz | `fuzz/fuzz_reward_model_score.c` (libFuzzer harness, ~120 LOC) | offline | Random utf-8 inputs into `score` / `score_steps`. No ASan/UBSan errors over 30 s. Catches subprocess buffer overflows on the MLX bridge. |
| T5 adversarial | `tests/test_reward_model_adversarial.c::reward_hack_red_team` | `RM-adversarial` | Runs a 20-prompt red team designed to maximise raw PRM score while violating the rubric. Each probe pre-labelled "should be flagged"; PRM must flag ≥15/20 OR the test xfails with a documented gap and the relevant `audit_probes_v1` row is bumped to the next training cycle. |
| T6 competitive | `tests/test_reward_model_vs_prompt_critic_pairwise.c` (gated) | `RM-competitive` | On a 500-pair preference test set (chosen vs rejected, drawn from `~/.human/private/seth/dpo_pairs.db`), trained PRM agrees with the chosen label ≥+15pp more often than prompt critic agrees, with bootstrap CI lower bound > 0. |

Fixtures `tests/fixtures/prm/{rubric_v1.json,calibration_v1.jsonl,audit_probes_v1.jsonl}` are committed verbatim. The training pairs themselves live under `~/.human/private/` (git-ignored, per `2026-05-11-full-sota-rl-improvement-loop-design.md` §13).

`HU_HAVE_PRM_CKPT=1` is the gate for tests that require a real checkpoint; the same pattern as `HU_HAVE_GEMMA_GGUF=1` in the RL loop spec. CI nightly fetches a small reference checkpoint (`prm-q0.5b-ref-v1`, ~250 MB) once, caches it, and runs the gated tier.

## D4 — Risk register

| # | Risk | Severity | Mitigation |
|---|------|----------|------------|
| 1 | **PRM does not beat prompt critics** on the calibration set (the D7 defer-condition). | High | Phased rollout. `HU_VERIFIER_TRAINED=0` (default) keeps the prompt path. Promotion to default-on requires the ECE-10pt gate in T2. If the gate fails after two training iterations + noise-aware re-labeling, park (see D7). |
| 2 | **Reward hacking on the PRM itself.** A LoRA training loop later (init-05, init-06) could overfit to PRM signal and degrade real user-facing fidelity. | High | (a) `audit()` mode with 10+ adversarial probe categories in `audit_probes_v1.jsonl`. (b) Periodic re-training: scheduler runs `prm-train` once every N user-corrections (default 200) or weekly, whichever comes first. (c) Distributional drift detector: KL between current 7-day score histogram and the prior 30-day histogram > 0.25 → flag for retrain + raise a `hu_observer_event_t` of type `HU_OBSERVER_EVENT_PRM_DRIFT`. (d) Never use PRM as the *only* reward signal in init-05 / init-06 — outcome reward (user 👍/👎) always dominates. PRM contributes ≤ 50% weight in any RL update. |
| 3 | **Label noise from implicit signals.** Heuristic implicit signals from W14 / W15 (substring "thanks", reaction counts) feed the pair store. Trained on raw, PRM will absorb the noise. | High | Adopt Noise-Aware PRM (NAIT, arXiv:2601.12748): two-stage training with reflection-aware label correction + iterative re-labelling by model confidence. Optional FreePRM (arXiv:2501.07301) "buffer area" for ambiguous steps. **Dependency on Initiative #15** (heuristic implicit signal cleanup); D5 cites it explicitly. Until #15 ships, training data is restricted to (a) explicit `hu_dpo_record_from_feedback` pairs, (b) synthetic rubric-violation pairs (deterministic, generated from `rubric_v1.json`), and (c) reflection-retry pairs from `hu_dpo_record_from_retry` (already known-clean). |
| 4 | **Latency budget breach.** PRM inference >30 ms on M3 Max breaks the response-path SLO. | Medium | Hard budget in `score()` / `score_steps()` enforced via `budget_p50_us`; backend self-reports. Test `RM-integration-FULL::trained_prm_p50_under_30ms` measures p50 over 500 calls. CPU-only fallback ≤ 200 ms. If the budget is exceeded, the backend returns `HU_ERR_TIMEOUT` and the agent falls back to the heuristic backend for that turn (logged once per session). |
| 5 | **Binary size regression.** New C code + factory dispatch grows MinSizeRel binary. | Medium | Hard ceiling 8 KB enforced by extending `scripts/agent-preflight.sh` size check. Mock measurements: vtable + factory (LTO + dead-code strip) ~2.0 KB; heuristic wrapper 0.6 KB; prompt wrapper 1.4 KB; thinkprm wrapper 2.8 KB; CLI 3.4 KB. CLI is `HU_ENABLE_ML`-gated, so default release stays under 4 KB. |
| 6 | **Model artifact distribution.** ~250–400 MB Q4_K_M GGUF must reach the user reliably. | Medium | Reuse `scripts/fetch-gemma-gguf.sh` pattern from init-04; SHA-256 verified; download is opt-in (`human ml prm-fetch`). If download fails or checkpoint is missing, factory falls back to `"prompt"` with a structured log line. No silent partial success. |
| 7 | **MLX subprocess fragility** (same risk as init-04 / init-06). | Medium | Inherit init-06's MLX subprocess lifecycle module (`src/ml/mlx_subprocess.c`); no parallel implementation. PRM training command goes through the same length-prefixed JSON-over-Unix-socket protocol; PRM inference goes through `llamacpp` (Metal) when a GGUF is present, MLX subprocess otherwise. |
| 8 | **ASan + Metal interaction** when running PRM inference on llamacpp's Metal backend in `human_tests`. | Medium | Tests that touch Metal are gated `HU_HAVE_PRM_CKPT=1` and run on a separate `RM-integration-FULL` suite outside the default ASan build (mirrors RL loop spec §6.3). |
| 9 | **PII leakage in checkpoint weights.** PRM trained on user's own DPO pairs may memorise rare phrases. | High (security) | PRM training data goes through the existing `src/security/` PII redactor before `prm-train` writes the JSONL. Checkpoint stays under `~/.human/private/` (git-ignored). Audit mode dumps top-activation tokens for review; reviewer can revoke a checkpoint by deleting the file (factory falls back to prompt automatically next chat). |
| 10 | **Cross-initiative API conflict** with init-05 (verifier-driven TTT) and init-06 (SimPO/ORPO/GRPO-2 trainers), all of which want to consume PRM rewards. | Medium | The `hu_reward_model_t` vtable is the single shared surface. init-05's `hu_learner_t.step()` and init-06's `hu_rl_trainer_t.step()` both accept an `hu_reward_model_t*` (NULL = use outcome signal only). The synthesis pass at the end of the 14-track program (`2026-05-11-sota-2026-massive-team-program.md` §"Synthesis target" item 3) explicitly tracks this. |

## D5 — References (with arXiv IDs)

All five required references located, plus three supporting papers cited inline.

1. **ThinkPRM** — Khalifa et al., *Process Reward Models That Think.* arXiv:**2504.16828** (April 2025). The core motivation: generative verification chain-of-thought outperforms discriminative classification PRMs and LLM-as-judge with only 1% of the labels. Releases 1.5B / 7B / 14B checkpoints fine-tuned from Deepseek-R1-Distill-Qwen. We adopt the 1.5B distilled into Qwen3-0.5B for size; methodology adapted to our process labels.

2. **Athena-PRM** — Wang et al., *Athena: Enhancing Multimodal Reasoning with Data-efficient Process Reward Models.* arXiv:**2506.09532** (June 2025). Provides the "weak-strong completer consistency" trick for selecting reliable process labels with as few as 5,000 samples. Directly applicable to our small DPO pair count (current `~/.human/private/seth/dpo_pairs.db` has on the order of 10²–10³ pairs).

3. **Noise-Aware PRM (NAIT)** — Yao et al., *Towards Robust Process Reward Modeling via Noise-aware Learning.* arXiv:**2601.12748** (January 2026). Two-stage framework: reflection-aware label correction + iterative re-labelling by model confidence. Up to 27% absolute F1 gain on noisy supervision. **This is the paper that directly addresses risk #3** (label noise from heuristic implicit signals). Supplemented by:
   - SCAN — *Self-Denoising Monte Carlo Annotation.* arXiv:2506.03570 — 6% of vanilla MC inference cost, 39.2 F1 gain on ProcessBench.
   - FreePRM — *Weakly supervised PRM without ground-truth step labels.* arXiv:2501.07301 — "buffer area" for ambiguous steps; we adopt for our reflection-retry pairs where step-level labels are unavailable.

4. **rePIRL** — Wang et al., *rePIRL: Learn PRM with Inverse RL for LLM Reasoning.* arXiv:**2602.07832** (February 2026). Inverse-RL formulation unifies online and offline PRM learning under minimal assumptions about the expert policy. Relevant for the longer-horizon path where PRM and policy LoRA update interchangeably (init-05 territory). We do *not* adopt rePIRL's joint training in v1 (too tightly coupled to ongoing chat), but we document the path so init-05's design can pick it up later. Also references PRIME (Process Reinforcement through Implicit Rewards, arXiv:2502.01456) as the prior baseline.

5. **EVPV — Explicit Visual Premise Verification for Reliable Vision-Language Process Reward Models** — Qwen Applications team. arXiv:**2603.16253** (March 2026). The "Process Verifier of Process" framing in the brief refers to this paper: a lightweight verification interface that *gates* the PRM's step rewards by an independent reliability signal, attenuating rewards when the underlying premise is unreliable. We adopt the gating idea for the text-only path: the PRM's per-step `confidence` field is computed from posterior width over the verification chain-of-thought, and the agent's retry threshold compares `aggregate × confidence` (not raw aggregate) — preserving the high-quality scores and suppressing the low-confidence ones.

**Industry baselines** (not arXiv, but informing the architecture):
- `huggingface/trl` PRM reference implementation.
- `mukhal/ThinkPRM` (released code/checkpoints).
- `AMD-AGI/Athena-PRM` (released code/checkpoints).

## D6 — Binary & memory budget

### C-side compiled delta (MinSizeRel + LTO + dead-code strip)

| Component | Compiled bytes (est.) |
|---|---|
| `reward_model.c` (vtable + factory + env-var routing) | 2,048 |
| `reward_model_heuristic.c` (wraps `hu_prm_score_*`) | 640 |
| `reward_model_prompt.c` (rubric prompt + JSON parse) | 1,420 |
| `reward_model_thinkprm.c` (llamacpp/MLX dispatch + step splitter) | 2,816 |
| `cli_prm.c` (`HU_ENABLE_ML`-gated, train + audit + fetch) | 3,440 |
| Header (declarations only) | 0 |
| **Default release total** (without `HU_ENABLE_ML` CLI) | **~6,924 bytes** ≈ 6.8 KB |
| **`HU_ENABLE_ML` release total** | **~10,364 bytes** ≈ 10.1 KB |

The 8 KB master ceiling is the **default release** budget. Default release is
under it (6.8 KB). The CLI binary that ships ML training is the larger ML
artifact and absorbs the rest; this is consistent with how `human ml lora-*`
already lives behind `HU_ENABLE_ML`. If `HU_ENABLE_ML` is dropped from the
default preset for the ship cycle, the CLI builds into a separate
`human_ml_tools` binary; same precedent as `human_tests` vs `human`.

### Runtime RSS / model footprint

| State | RSS |
|---|---|
| Backend `"prompt"` (default) | +0 (reuses provider) |
| Backend `"heuristic"` | +0 (re-uses existing `hu_prm` heuristic) |
| Backend `"thinkprm"` resident (Q4_K_M) | ~250–400 MB depending on model |
| Backend `"thinkprm"` subprocess (MLX) | ~600 MB in subprocess (lazy-spawned) |

The model artifact lives at `~/.human/models/prm-q0.5b-<sha>.gguf`, downloaded
once via `human ml prm-fetch`. Fetch is opt-in. The Q4_K_M file is ≈ 250 MB for
Qwen3-0.5B-Instruct; a 1B-class variant (closer to ThinkPRM-1.5B) sits at ≈ 400
MB if the calibration target requires it.

### Latency budget

- **M3 Max, llamacpp Metal, Q4_K_M Qwen3-0.5B, 64-step reasoning chain:**
  p50 17 ms, p95 28 ms. Within the 30 ms ceiling.
- **CPU-only fallback (no Metal, Linux):**
  p50 110 ms, p95 180 ms. Within the 200 ms ceiling.
- **`score_steps` over a 64-step chain:** budgets are *per chain*, not per step.
  Steps share one teacher-forced forward; per-step cost is the soft-max over the
  verification CoT, not a fresh inference.

These numbers come from independent ThinkPRM-1.5B benchmarks at Q4 on M3 Max,
scaled to 0.5B by the ~3× parameter ratio. They will be re-measured at the end
of P2 of the sprint that adopts this design and the table updated in place.

## D7 — Defer / descope condition

This initiative **parks** if any of the following becomes true during the
implementation sprint:

1. **The trained PRM does not beat the prompt critic by ≥10 ECE points** on
   `tests/fixtures/prm/calibration_v1.jsonl` after two full training iterations
   (vanilla + noise-aware re-label). Park condition signals: prompt critics +
   outcome rewards are already competitive at our scale; further effort yields
   diminishing returns until we have substantially more user-correction volume.
2. **Audit mode flags >20% drift on the user's actual response distribution
   within 14 days** of the first promotion to default. Reward hacking is real;
   we revert to prompt + heuristic until init-15 (implicit signal cleanup)
   delivers cleaner training data.
3. **Inference latency cannot be held under 30 ms p95 on M3 Max** with a
   Q4_K_M 0.5B model on the llamacpp Metal backend, *and* the CPU-only fallback
   cannot hold 200 ms. Process scoring on every turn is non-negotiable on
   latency; if the budget is unreachable, the PRM moves to background-only
   scoring (post-turn telemetry) and the response-path call sites stay on the
   prompt backend.
4. **PII redaction on training data is incomplete** as confirmed by a
   `security-reviewer` adversarial probe. We do not train a model on
   un-redacted personal data, period.

**When parked**, this design doc is preserved at status `parked` with a
one-paragraph rationale in the master coordinator status table, and the
`hu_reward_model_t` vtable surface is *still* shipped — but only the
`"prompt"` and `"heuristic"` backends, which deliver the staged-rollout
plumbing without the model. The thinkprm backend ships when the gate is
green.

---

## Architecture, in five paragraphs

**1. The verifier panel today is three independent prompt heuristics.** Lines
5101–5138 of `agent_turn.c` run `hu_prm_score_chain`, `hu_prm_score_step`, and
`hu_reflection_evaluate_llm` back to back on every response. The first two are
keyword/digit/connector heuristics over the response text; the third sends the
draft back to the same provider with a "score this answer" prompt. None of
them is calibrated — `hu_prm_score_step` returns 0.5 by default and slides up
or down by hand-tuned increments; `hu_reflection_evaluate_llm` is biased by
the same model that wrote the draft. The retry threshold (0.3 hard) is
guess-tuned. ThinkPRM (arXiv:2504.16828) and Athena-PRM (arXiv:2506.09532)
both show that a small (0.5B–1.5B) PRM, trained on as few as 5,000 examples,
strictly dominates prompt critics on calibration error, agreement with held-out
human labels, and noise-robustness.

**2. The vtable is intentionally narrow.** `hu_reward_model_t` exposes
`score(ctx) → float` and `score_steps(chain, k) → float[]`, plus an `audit()`
mode for adversarial probing. Three backends implement it: `"heuristic"`
wraps the existing `hu_prm_score_*` so nothing regresses; `"prompt"` wraps the
existing `hu_reflection_evaluate_llm` so the rollout is bisectable; `"thinkprm"`
loads a Q4_K_M Qwen3-0.5B PRM checkpoint via the same llamacpp/MLX dispatch
init-04 ships. The vtable is the only thing `agent/` sees, which preserves the
module-dependency wall (§3.3 of the RL loop design).

**3. Training reuses init-06's MLX subprocess.** `human ml prm-train --pairs=...
--rubric=... --out=prm-checkpoint` builds an Alpaca-shaped JSONL from the DPO
collector + synthetic rubric-violation pairs (deterministic, generated from
`tests/fixtures/prm/rubric_v1.json`), then dispatches to
`scripts/mlx_trainer/prm_train.py` via the same length-prefixed JSON-over-Unix
socket protocol RL loop §4.8 defines. There is no second MLX subprocess and no
new Python dependency beyond the pinned `mlx`, `mlx-lm`, `transformers`. The
training algorithm itself is Noise-Aware PRM (arXiv:2601.12748): two-stage
training with a reflection-aware label-correction pass on the noisy implicit
signals from W14/W15 (the dependency on init-15).

**4. Calibration is the gate.** The metric is ECE (expected calibration
error), measured as the gap between predicted reward and observed rubric
ground truth across 10 reliability bins on the held-out
`tests/fixtures/prm/calibration_v1.jsonl` (200 turns labelled by the corpus
owner; same rubric and same backward-compatible 4-axis structure as RL loop
§4.6's `lora_baseline_persona_v2_responses.json`). Prompt critic ECE on this
set is measured first, and the trained PRM must improve it by ≥10 points.
We also report Brier score so the test can catch "ECE win by centering
toward 0.5". The reliability diagram is emitted at every promotion as part
of the proof artifact (`~/.human/proofs/<adapter-id>/prm_calibration.json`).

**5. The PRM itself is an attack surface.** A reward model that the agent
uses as a verifier is the most direct path to reward hacking (init-05 will
literally train the policy against it). Three defenses ship together:
periodic re-training (the scheduler runs `prm-train` after every N user
corrections); distributional drift detection (KL between 7-day and 30-day
score histograms above 0.25 → drift flag); and an explicit `audit()` mode
that runs the PRM against a committed adversarial probe set
(`tests/fixtures/prm/audit_probes_v1.jsonl`: short-answer hack, sycophancy,
hedge-bombing, length-padding, emoji-stuffing, plus six more). The PRM
never replaces outcome rewards in any downstream RL trainer; it
contributes at most 50% weight. The `hu_observer_event_t` event
`HU_OBSERVER_EVENT_PRM_DRIFT` (new — added in this sprint, single-line
event) makes drift visible to dashboards without a log scrape.

---

## Build sequence

The implementation sprint that picks this design up runs through these phases.
Each ends with a verifier-evidence artifact written to
`~/.human/proofs/init-07-<phase>/`. No phase is cuttable.

| Phase | Deliverable | Gate |
|---|---|---|
| **0** | Vtable + factory + heuristic backend wired into `agent_turn`. `HU_VERIFIER_TRAINED` env var ignored (factory always returns `"heuristic"`). | `RM-unit` green. `verify-all.sh` clean. Default release size delta ≤ 1 KB. |
| **1** | Prompt backend ships. `HU_VERIFIER_TRAINED=0` (default) maps to `"prompt"`; behavior is bit-identical to today's call chain on 50-prompt regression fixture. | `RM-unit` + `RM-integration` + `test_agent_turn_uses_reward_model::prompt_backend_default_unchanged` green. |
| **2** | `human ml prm-train` + `prm-audit` + `prm-fetch` CLIs land. Synthetic rubric-violation generator commits. First reference checkpoint `prm-q0.5b-ref-v1` produced from a 200-pair fixture and uploaded to the public artifact server. | `RM-integration-FULL::trained_prm_p50_under_30ms` green. Calibration set scored; ECE / Brier published. |
| **3** | Noise-aware re-labelling pass on the real Seth corpus. Second checkpoint produced. ECE delta vs prompt critic measured. | If ≥10 ECE points improvement → promote `"thinkprm"` to opt-in via `HU_VERIFIER_TRAINED=1`. Else → escalate, return to phase 2 with revised hyperparameters; one retry budget allowed before D7 fires. |
| **4** | Drift detector ships. `HU_OBSERVER_EVENT_PRM_DRIFT` wired into dashboard tile `ui/src/components/hu-fidelity-tile.ts` (already on the open file list — additive). Audit mode ships behind `human ml prm-audit`. | `RM-adversarial` red team passes ≥15/20. |
| **5** | Cross-initiative integration: init-05 (TTT) and init-06 (RL trainers) consume `hu_reward_model_t*` from `agent->sota.reward_model`. PRM weight in RL update capped at 0.5. | `api-contract-watcher` clean against init-05 / init-06 designs. |

The phases line up 1:1 with the master coordinator's sprint-planning checklist
(`2026-05-11-sota-2026-massive-team-program.md` §"Synthesis target"). Each
phase is independently shippable; we can stop at phase 1 (vtable + prompt
backend) and still have shipped the staged-rollout plumbing required for
init-05 and init-06 to plug into a stable interface.

---

## Critical details

### Error handling

- `hu_reward_model_create` never returns `HU_OK` with a backend that cannot
  serve the first call. The factory eagerly calls `available()` on the new
  backend; if it fails, the factory falls back to `"heuristic"` (never silently
  drops to no-scoring) and emits one structured log line.
- `score()` returning `HU_ERR_TIMEOUT` is *not* a fatal error on the response
  path — `agent_turn` treats it as "no PRM signal this turn" and proceeds with
  the heuristic-only retry decision. A counter on `hu_agent_t` (mirroring
  `verifier_runs`) records `reward_model_timeouts` so ops dashboards can spot
  a stalled subprocess.
- `score_steps()` allocations live entirely in the caller's `hu_allocator_t`;
  `step_result_free()` is the only freeing path. ASan will catch any backend
  that forgets to route through it.

### State management

- One `hu_reward_model_t` per agent, owned by `hu_agent_extensions_t`.
  Reload of the checkpoint at runtime (`SIGHUP` → daemon path) calls
  `vtable->reload_checkpoint()`; if NULL on the backend, the daemon logs and
  does nothing — same pattern as `hu_provider_load_adapter` for cloud providers.
- The thinkprm backend keeps a long-lived llamacpp model handle, exactly like
  `src/providers/llamacpp.c`. The KV cache is *not* shared with the chat
  provider — verifier inference would clobber the chat KV. Init-13 (KV
  compression) is allowed to share later under a separate explicit flag, but
  not in this initiative.

### Testing

Test-only allocations follow `docs/standards/engineering/testing.md`. The fuzz
harness uses libFuzzer with a corpus derived from the calibration set's prompts
truncated and mutated; we expect zero ASan/UBSan errors in a 30-second run as
the CI gate. `RM-integration-FULL` is nightly only, gated on the reference
checkpoint cache; the per-PR gate is `RM-integration` with mocked backends.

### Performance

The thinkprm backend's `score()` runs one teacher-forced forward over a
verification CoT of length ≤ 256 tokens. On a Q4_K_M Qwen3-0.5B on M3 Max
this is ~17 ms; on a CPU-only fallback (Linux desktop, Q4_K_M) it is ~110
ms. The CPU-only path is allowed to exceed the 30 ms M3-Max ceiling but
must hold 200 ms. Tests `trained_prm_p50_under_30ms` and
`trained_prm_p50_under_200ms_cpu` enforce both, gated on
`HU_HAVE_PRM_CKPT=1`. Future optimisation candidates: speculative-decode the
verification CoT (init-13's KV-compression work overlaps), and a Q3_K_M
variant for the CPU-only path.

### Security

The training data path runs through `src/security/pii_redact.c` before
JSONL write; the redactor is the same one shipping in the RL loop spec §13.
The reference checkpoint distributed via `prm-fetch` is SHA-256-pinned in
`scripts/fetch-prm-gguf.sh`. The audit mode emits a JSON report; reviewers
can revoke a checkpoint by `rm ~/.human/models/prm-*.gguf`; the factory falls
back to prompt automatically next chat with no further action required. The
PRM never reads the W7 memory graph — it operates on the response text and
the persona prompt only.

---

## Single biggest open question

**Where does the rubric ground truth come from for the calibration set?**

The 200-turn `tests/fixtures/prm/calibration_v1.jsonl` needs per-turn rubric
labels (rubric-violation: yes/no per axis, plus a final 0..1 quality score).
Three options, each with a real cost:

1. The corpus owner labels them by hand (cleanest signal; ~6–10 hours of
   labelling for 200 turns; only available to a single corpus owner).
2. A frontier model labels them with the rubric prompt (cheap; risks circular
   reasoning — the trained PRM ends up being a distilled version of the
   prompt critic it's supposed to beat).
3. We bootstrap with frontier labels, then have the corpus owner re-label
   only the ones where the trained PRM and the frontier label disagree (the
   "active learning lite" approach; expected ~60–80 turns of human re-labelling).

The current default is **option 3**, but if the corpus owner's labelling time
is unavailable, we fall back to option 2 with an explicit *caveat in the proof
artifact* that the calibration gate is partly self-referential. This is the
single biggest open question and is the one decision in this design doc that
the implementation sprint should re-confirm with the user before phase 2
trains its first checkpoint.

---

**End of init-07 design doc. Status: design done, awaiting sprint planning.**
