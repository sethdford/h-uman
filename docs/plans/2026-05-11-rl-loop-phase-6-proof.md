# Phase 6: E2E Proof + Demo — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the loop, prove it closes, and ship the demo. Two artifacts ship from this phase and they are intentionally split:

1. **`tests/test_e2e_rl_loop.c`** — the canonical, deterministic, CI-runnable proof that the closed loop **wires correctly end-to-end**: a chat call → a synthetic reaction event → a row in the preference store → a `hu_rl_trainer_t` step → an adapter on disk → a `hu_provider_load_adapter` swap on the active provider → the **next chat call returns a response that is byte-different from the first one** (and, on the held-out fixture set, scores higher on `hu_communication_style_fidelity_score`). The test uses the HUML toy GPT (~32-token vocab, hand-controlled weights) so it is fully deterministic, runs under ASan in **≤ 30 seconds in CI**, and can be re-run on Linux x86_64 without GPUs, MLX, or 2.4 GB GGUF downloads. This test answers the question "did the wiring close" — it does **not** answer "did the model improve on a real benchmark"; that is what the demo script is for.
2. **`scripts/demo-rl-loop.sh`** — the live, non-deterministic, Apple-Silicon-only demo that runs the **same loop** against real Gemma-3-4B-it (Phase 1) and a real Qwen-2.5-0.5B-Instruct reward model (Phase 3). It produces a real `~/.human/proofs/<adapter-id>/` evidence directory matching spec §8 (manifest, training curves, eval_before, eval_after, eval_delta, delta_responses, gate_decision, adversarial_review, reproduce.sh) and the win-condition scorecard from spec §1. It runs locally before each release tag, **never in CI** (per spec §6.5 and the user-decision in spec §14: no paid M-series CI runner). The demo is the artifact a fresh-clone reviewer reproduces from `docs/demos/rl-loop-demo.md` — and the artifact whose persona-fidelity delta of **≥ +0.05 absolute points** (spec §1, §11 row 4) ships in `docs/proof/rl-loop-proof.md`.

This phase is the smallest of the six (no new vtables, no new losses, no new providers) and the highest-stakes (every prior phase has to actually compose). The work is wiring + evidence, not new architecture.

**Architecture:** No new vtables. No new training algorithms. No new providers. The phase builds on top of every prior shipping artifact:

- Phase 1's `src/providers/llamacpp.c` (`chat_with_system`, `load_adapter`/`unload_adapter`, KV cache).
- Phase 2's `hu_rl_trainer_t` + `hu_dpo_real_huml_create` (`include/human/ml/dpo_real.h`), `hu_reaction_event_t` + `hu_reaction_handler_handle_event` (`include/human/channels/reaction_event.h`, `include/human/agent/reaction_handler.h`), `dpo_pairs` SQLite table (`src/ml/dpo.c`).
- Phase 3's `hu_rl_trainer_create_kto` + `hu_reward_model_t` (assumed shipped per user prompt: "Phase 3: real KTO + reward model + value head").
- Phase 4's `hu_rl_trainer_create_grpo` + `hu_rollout_*` + `src/ml/grpo.c` (assumed shipped per user prompt: "assume hu_grpo trainer in src/ml/grpo.c").
- Phase 5's `hu_eval_gate`, `hu_competitive_harness`, the 4-axis `hu_communication_style_fidelity_score` extension, and `tests/fixtures/lora_baseline_persona_v2_responses.json` + `_v2_rubric.md` (assumed shipped per user prompt: "Phase 5: eval gate + competitive harness + 4th axis").

The Phase 6 code adds **one** new C source (`src/ml/cli_demo.c`), **one** new C header (`include/human/ml/cli_demo.h`), **one** new top-level CLI subcommand (`human demo rl-closed-loop`), **one** small additive helper to `src/agent/lora_training_runner.c` (a synchronous "post-swap re-chat & assert delta" entry point gated by `HU_IS_TEST` and reused by the demo CLI), and **one** new test file (`tests/test_e2e_rl_loop.c`) that ties it all together. Plus the two committable JSON fixtures, the demo shell script, and the runbook markdown.

**Tech Stack:** C11 (`-Wall -Wextra -Wpedantic -Werror`), AddressSanitizer + UndefinedBehaviorSanitizer in `dev` preset, the existing `hu_gpt_t` toy GPT for the deterministic E2E test, the Phase 1 llama.cpp Metal backend for the demo, the Phase 2 `hu_rl_trainer_t` vtable, the Phase 3 `hu_reward_model_t`, the Phase 5 `hu_eval_gate` + 4-axis fidelity scorer, the existing `hu_provider_t.load_adapter` seam, the existing test framework (`tests/test_framework.h`), conventional commits, the existing `dead-code-finder` + `sprint-auditor` + `spec-verifier` + `critic` + `verifier` subagent gates (no `aspect-panel` required at Phase 6 per spec §7 — only P2/P4/P5 trigger the panel; Phase 6 ships behind `sprint-auditor` PASS).

**Linked spec:** `docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md` §4.7 (file inventory, lines 423–432) + §5 row 6 ship contract (line 501) + §6 Tier 4 E2E requirement (line 522) + §8 evidence-artifacts schema (lines 590–607) + §9 DoD items 8 + 10 + 11 (lines 620–625) + §14 reproducibility recipe (lines 704–725).
**Linked umbrella plan:** `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md`
**Predecessor plans:** `docs/plans/2026-05-11-rl-loop-phase-0-honesty.md` (tag `rl-sota-phase-0-complete`), `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md` (tag `rl-sota-phase-1-complete`), `docs/plans/2026-05-11-rl-loop-phase-2-dpo-reactions.md` (tag `rl-sota-phase-2-complete`), `docs/plans/2026-05-11-rl-loop-phase-3-kto-rm.md` (tag `rl-sota-phase-3-complete`), `docs/plans/2026-05-11-rl-loop-phase-4-grpo.md` (tag `rl-sota-phase-4-complete`), `docs/plans/2026-05-11-rl-loop-phase-5-eval-competitive.md` (tag `rl-sota-phase-5-complete`).

---

## Phase 6 status snapshot

| Step | Owner | Status | Date |
|------|-------|--------|------|
| Plan authored | this doc | ⏳ in progress | 2026-05-12 |
| Plan reviewed (`critic` + `spec-verifier`) | subagents | ⏳ | — |
| Plan committed | git | ⏳ | — |
| Implementation start gate | subagent-driven | ⏳ | — |
| Tasks 0–12 implemented | subagent-driven | ⏳ | — |
| E2E closed-loop deterministic test passing in ≤ 30 s under ASan | Task 4 + Task 8 | ⏳ | — |
| `human demo rl-closed-loop` subcommand wired and tested with mock backend | Task 5 + Task 6 | ⏳ | — |
| `scripts/demo-rl-loop.sh` produces real evidence dir on Apple Silicon | Task 9 | ⏳ | — |
| `docs/demos/rl-loop-demo.md` runbook committed | Task 10 | ⏳ | — |
| Persona-fidelity delta ≥ +0.05 absolute points on held-out 100 prompts (live demo) | Task 11 | ⏳ | — |
| Phase 6 end gate (full suite + dead-code + sprint-auditor + tag) | Task 12 | ⏳ | — |

---

## What we're building on (Phases 0–5 deliverables, do NOT duplicate)

**Phase 0** (tag `rl-sota-phase-0-complete`): silent `vocab_size=0` bug fixed; `hu_personal_model_save` atomic; `hu_dpo_train_step` renamed to `hu_dpo_judge_step`; `~/.human/private/` `.gitignore`d; `CLAUDE.md:53` reconciled with reality.

**Phase 1** (tag `rl-sota-phase-1-complete`): llama.cpp vendored at `b9055` with Metal; `src/providers/llamacpp.c` real `chat_with_system` + KV cache + `vtable.load_adapter` / `unload_adapter`; Gemma-3-4B-it Q4_K_M GGUF auto-fetched + SHA-verified by `scripts/fetch-gemma.sh`; 20-prompt sanity gate `20/20 PASS`; CMake preset `rl_sota`.

**Phase 2** (tag `rl-sota-phase-2-complete`): `hu_rl_trainer_t` vtable + factory + two-track DPO (HUML in-process canonical + MLX subprocess real-Gemma); `hu_policy_logprobs`; `hu_reference_model`; `hu_reaction_event_t` + iMessage tapback poll + Slack `reactions.added/removed` webhook + `hu_reaction_handler_handle_event` writing into `dpo_pairs`; `human ml dpo-train` real-DPO dispatch.

**Phase 3** (tag `rl-sota-phase-3-complete`, assumed): `hu_rl_trainer_create_kto` factory; `hu_value_head_t` + `hu_reward_model_t` vtable; HUML + MLX RM backends; Bradley-Terry training loop on two-sided `dpo_pairs` rows; `human ml kto-train` and `human ml rm-train` subcommands; `scripts/fetch-qwen-rm.sh` + Qwen-2.5-0.5B-Instruct Q4_K_M GGUF lazy fetch.

**Phase 4** (tag `rl-sota-phase-4-complete`, assumed per user prompt): `hu_rl_trainer_create_grpo` factory; `src/ml/grpo.c` GRPO loss with group-relative baseline + PPO ratio clip ε=0.2 + KL penalty β=0.04 to π_ref; `src/ml/rollout.c` for N-rollout sampling via `hu_provider_t.chat`; `src/ml/kl_divergence.c`; `human ml grpo-train --rollouts 4` subcommand.

**Phase 5** (tag `rl-sota-phase-5-complete`, assumed per user prompt): 4th decision-style axis added to `hu_communication_style_fidelity_score` (with `_v1` deprecated shim preserving 3-axis backward compat); `tests/fixtures/lora_baseline_persona_v2_responses.json` + `_v2_rubric.md` 100-prompt held-out fixture; `src/eval/eval_gate.c` composing persona ≥ Δ + leaderboard regression ≤ ε + latency p95 ≤ τ with bootstrap CIs; `src/eval/competitive_harness.c` orchestrating stock / DPO / KTO / GRPO / Apple FM / Gemini Nano scorecard; `human eval competitive` and `human eval leaderboard` subcommands; `src/agent/lora_training_runner.c` already calls `hu_eval_gate` before promoting an adapter (the relevant clause was deferred in Phase 5's plan to Phase 6 if not done — see Task 3 below for the fold-in).

**What Phase 6 does NOT touch (other phases own them):**

- DO NOT modify `src/providers/llamacpp.c` (Phase 1). Phase 6 CONSUMES it via `hu_provider_t.chat_with_system` + `hu_provider_t.load_adapter` only.
- DO NOT modify `hu_rl_trainer_t` vtable, `hu_dpo_real_huml`, `hu_dpo_real_mlx`, `hu_kto_*`, `hu_reward_model_*`, `hu_grpo_*` (Phases 2–4 own them). Phase 6 INSTANTIATES them via the existing factories.
- DO NOT modify `hu_communication_style_fidelity_score` or its 4th axis (Phase 5). Phase 6 CALLS it and reads the returned floats.
- DO NOT modify `hu_eval_gate` (Phase 5). Phase 6 CALLS it on the trained adapter before promoting.
- DO NOT modify `src/channels/imessage.c` reaction-poll branch or `src/channels/slack.c` webhook reactions branch (Phase 2). The deterministic E2E test SYNTHESIZES `hu_reaction_event_t` values directly via `hu_reaction_handler_handle_event` — no channel I/O.
- DO NOT add a new vtable. Phase 6 has zero new public surfaces beyond the `human demo rl-closed-loop` CLI subcommand and the `hu_e2e_closed_loop_run` test-only helper in `lora_training_runner.c` (gated by `HU_IS_TEST` to avoid binary-size impact).
- DO NOT touch `~/.human/private/` discipline (Phase 0). Phase 6 fixtures live entirely in `tests/fixtures/` (synthetic, committable, no real PII).

---

## Phase 6 boundary with in-flight Track D Phase 1 work

Track D Phase 1 still owns `src/ml/cli.c` (`lora-baseline`, `lora-ab`, `lora-persona`, `lora-runner`, `fidelity-status`, `apply-adapter`), `src/memory/personal_model.{h,c}` (the 3+1-axis communication-style fidelity scorer; the 4th axis was added by Phase 5), and `src/main.c::cmd_ml` (the `human ml *` dispatcher).

Phase 6 does **not** add a new `human ml *` subcommand. It adds a **new top-level command** `human demo` with a single subcommand `rl-closed-loop` (extensible later for other demos). This puts the demo dispatch into a new function `cmd_demo` in `src/main.c` (analog to the existing `cmd_ml`), bypassing the `cmd_ml` shared file entirely. Rationale: the demo is presentation, not training; conflating it with `human ml *` would confuse the surface and force every Phase 6 commit to rebase against Track D's `cli.c` churn.

**Phase 6 must:**

- Branch from tag `rl-sota-phase-5-complete` (`git checkout -b rl-sota-phase-6 rl-sota-phase-5-complete`).
- Rebase against `main` at the start of each task if Track D Phase 1 lands new commits in `src/main.c` (the `commands[]` table at line 510-ish is the only shared zone; Track D rarely touches it).
- Use `git stash push -- <files>` if Track D contamination appears in the working tree (recurring pattern from Phases 0/1/2/3/4/5).
- Stage ONLY Phase 6 files into Phase 6 commits.

---

## Architectural decisions (justification)

### D1: The E2E test uses HUML toy GPT, NOT real Gemma

**Decision:** `tests/test_e2e_rl_loop.c` instantiates `hu_dpo_real_huml_create` (or `hu_kto_huml_create` for the KTO variant; or `hu_grpo_huml_create` for the GRPO variant — all three are exercised in separate test functions in the same file) and a mock provider that wraps the same toy `hu_gpt_t` instance the trainer mutates. This is a deliberate scope choice, not a shortcut.

**Why:** The E2E test is answering the question **"did the wiring close end-to-end?"** That is independent of whether real Gemma improves on a real benchmark. Using the toy GPT lets us:

- **Be fully deterministic.** `srand(42)`, `hu_gpt_init` with a fixed seed, `hu_rl_trainer_config_t.learning_rate = 1e-2` (large for the toy), fixed reaction-signal stream from `tests/fixtures/e2e_reaction_signals.json` — and the test produces byte-identical adapter weights across runs and across machines (Task 8 pins this).
- **Run in ≤ 30 seconds in CI under ASan.** Toy GPT forward + backward on a 32-token vocab + 8-layer config is microseconds per step; 50 reaction signals × 1 DPO step each ≈ 50 ms of compute, plus harness overhead ≈ ~5–10 s. ASan adds 2–3×; we land well under the 30 s budget.
- **Run on Linux x86_64 without GPUs, MLX, or GGUF downloads.** The toy path has no Apple-specific dependency. Phase 1's `rl_sota` preset is not required — the test compiles under `dev` preset.
- **Produce a measurable behavior change.** The toy GPT is small enough that 50 DPO steps move the next-token distribution by ≫ 1 token (i.e. the post-swap response is different from the pre-swap response on the same fixed input). The fidelity scorer (Phase 5 4-axis) returns numerically distinct floats pre and post.

**The cost we accept:** the E2E test does NOT prove "real Gemma improves on a real benchmark." That is the demo's job, not the test's job. The demo runs on Apple Silicon with real Gemma + real Qwen RM and produces the spec §1 win-condition scorecard. Each artifact answers the question it is sized for; bundling them into one CI-runnable test would either run for an hour with real models or be a fake using mock weights.

### D2: The demo script (vs E2E test) uses real models, runs locally, and is NEVER in CI

**Decision:** `scripts/demo-rl-loop.sh` invokes `human demo rl-closed-loop` against real Gemma-3-4B-it Q4_K_M (Phase 1) and a real Qwen-2.5-0.5B-Instruct Q4_K_M reward model (Phase 3). It writes evidence to `~/.human/proofs/<YYYY-MM-DD>-<method>-step-<N>/` per spec §8. It runs **locally on Apple Silicon before each release tag**, NEVER in CI. Per spec §14 + spec §6.5 (final paragraph): no paid M-series CI runner for nightly automation; weekly automated reproduction deferred.

**Why:**

- **CI cost.** A single end-to-end real-model run is ~3 minutes on M2; running it on every PR would add ~$X/month of paid runner time, which the user explicitly rejected (spec §14, §6.5).
- **Determinism is impossible without huge effort.** Real Gemma + MLX kernel scheduling + Metal float-order = adapter byte hashes differ across runs even with identical seeds. The test handles determinism (D1); the demo doesn't try.
- **The demo's output IS the proof artifact.** Spec §8 evidence directories live under `~/.human/proofs/` (user disk), not under the repo. Spec §14 reproducibility recipe documents *how* to re-run, not where the files live. Demo runs are by-design unrepeatable on different corpora.

### D3: New top-level `human demo` command, NOT a new `human ml *` subcommand

**Decision:** Phase 6 adds `cmd_demo` to `src/main.c::commands[]` (analog to existing `cmd_ml`, `cmd_evaluation`, `cmd_hula`). The single subcommand under it is `human demo rl-closed-loop`. The dispatch handler lives in `src/ml/cli_demo.c` (because the implementation is ML-adjacent and reuses ML internals); the `cmd_demo` wrapper in `main.c` is a 12-line `argc`/`argv` shim mirroring `cmd_ml`'s shape.

**Why:**

- **Surface clarity.** Demos are presentation, not production. `human ml dpo-train` mutates state; `human demo rl-closed-loop` is a guided show-and-tell with a fixed narrative (chat → react → train → swap → re-chat). Conflating them would mislead users who run `human ml --help` looking for trainable subcommands.
- **Track D Phase 1 conflict avoidance.** Track D owns most of `src/ml/cli.c` and the `cmd_ml` dispatcher. Adding a `demo-*` subcommand under `cmd_ml` would force every Phase 6 commit to rebase around Track D. A separate top-level command is one new line in `commands[]` and zero rebase overhead.
- **Future extensibility.** Future demos (`human demo persona-overlay`, `human demo memory-decay`, etc.) get a natural home without growing `cmd_ml` or `cmd_evaluation`.

### D4: Closing the loop = `hu_e2e_closed_loop_run`, a test-only synchronous helper in `src/agent/lora_training_runner.c`

**Decision:** Add a new function in `src/agent/lora_training_runner.c`:

```c
#ifdef HU_IS_TEST
typedef struct hu_e2e_closed_loop_input {
    hu_provider_t      *provider;
    hu_rl_trainer_t    *trainer;
    hu_dpo_collector_t *collector;
    const hu_reaction_event_t *reaction_events;
    size_t              reaction_event_count;
    /* chat_with_system inputs — match the real signature
     * (include/human/provider.h:207-210). The "prompt" is split into
     * system + user halves; the test fills both deterministically. */
    const char *system_prompt;
    size_t      system_prompt_len;
    const char *user_message;     /* the single message re-chatted before AND after */
    size_t      user_message_len;
    const char *model;
    size_t      model_len;
    double      temperature;      /* default 0.0 for deterministic toy GPT */
    const char *adapter_out_path; /* absolute path under tests/_tmp/ — see LO3 */
    const char *adapter_id;
} hu_e2e_closed_loop_input_t;

typedef struct hu_e2e_closed_loop_output {
    /* Heap-allocated response buffers — owned by the output struct,
     * freed via hu_e2e_closed_loop_output_free. The provider's
     * chat_with_system signature returns char ** + size_t * (see
     * include/human/provider.h:207-210), so we keep the same shape
     * here rather than copying into fixed-size buffers (which would
     * silently truncate long responses on the demo path that shares
     * this struct shape via D5 duplication). */
    char       *before_response;
    size_t      before_response_len;
    char       *after_response;
    size_t      after_response_len;
    bool        responses_differ;
    double      pairs_consumed;     /* number of preference pairs the trainer actually used */
    int64_t     elapsed_ms;
    char        adapter_path[1024]; /* echoed back from training (path string, not heap buffer) */
} hu_e2e_closed_loop_output_t;

/* Test-only synchronous closed-loop runner. Production code uses the
 * existing async hu_lora_training_runner via the scheduler instead.
 *
 * Steps (in order):
 *   1. provider.chat_with_system(prompt) → out.before_response (heap; caller frees)
 *   2. hu_reaction_handler_set_collector(collector)
 *      for each reaction_event: hu_reaction_handler_handle_event(event)
 *      hu_reaction_handler_set_collector(NULL)
 *      → inserts rows into collector's dpo_pairs table
 *   3. hu_dpo_pair_count(collector) → assert ≥ N (caller-supplied floor)
 *   4. hu_dpo_export(collector, &export) → hu_preference_pair_t array
 *      trainer.vtable->step(ctx, alloc, pairs, n_pairs, &metrics) for K iters
 *      hu_dpo_export_free(alloc, &export)
 *   5. trainer.vtable->save_adapter(ctx, alloc, adapter_out_path)
 *   6. hu_provider_load_adapter(provider, alloc, adapter_out_path, ...,
 *      adapter_id, ...). NB: Phase 1's llamacpp_load_adapter
 *      (src/providers/llamacpp.c:454-455) already calls llama_memory_clear
 *      + hu_llamacpp_kvcache_reset internally; no extra kv_cache_clear
 *      call is required (and the vtable has no such field).
 *   7. provider.chat_with_system(prompt) → out.after_response (heap; caller frees)
 *   8. fill out.responses_differ = (memcmp(before, after, ...) != 0)
 *   9. fill out.pairs_consumed, out.elapsed_ms
 *
 * Returns HU_OK on full traversal, HU_ERR_* on the first failing step.
 * On HU_OK the caller MUST call hu_e2e_closed_loop_output_free(alloc, &out)
 * to release before_response / after_response. On error the function frees
 * whatever it already allocated before returning, so the caller MAY still
 * call _free unconditionally (it tolerates NULL pointers).
 *
 * HU_IS_TEST gating means this symbol is NOT in release binaries —
 * binary-size impact is zero in the default release preset.
 */
hu_error_t hu_e2e_closed_loop_run(const hu_e2e_closed_loop_input_t *in,
                                  hu_allocator_t *alloc,
                                  hu_e2e_closed_loop_output_t *out);

/* Release heap-allocated buffers inside `out`. Idempotent; safe with
 * partial-fill outputs from an early-error return. */
void hu_e2e_closed_loop_output_free(hu_allocator_t *alloc,
                                    hu_e2e_closed_loop_output_t *out);
#endif
```

**Why:**

- **Test isolation from production scheduler.** The production path (Phase 5's `hu_lora_training_runner` callback registered with `hu_scheduler`) runs ASYNC via budget-based wakeups. The deterministic E2E test cannot afford async — it needs the loop to complete synchronously inside the test function so assertions can fire. `hu_e2e_closed_loop_run` is the synchronous projection of the same algorithm; it shares no state with the production callback (no shared globals, no scheduler), so the production path is unaffected.
- **`HU_IS_TEST` gating means zero binary-size cost.** The symbol is compiled out in release builds (the existing `HU_IS_TEST` pattern is used 100+ places in the codebase). Spec §6.4 hard sanity gate is "default release binary delta ≤ +250 KB"; this test seam contributes 0 bytes to release.
- **Reuses existing primitives.** Every step calls an already-shipped function. No new vtable, no new factory, no new abstraction.
- **Demo CLI reuses the same helper.** `hu_ml_cli_demo_rl_closed_loop` (Task 5) calls `hu_e2e_closed_loop_run` too — but with `HU_IS_TEST` undefined in release, the demo build path uses a **release-build duplicate** lifted into `src/ml/cli_demo.c` proper. See D5 for why the duplication is justified.

### D5: Deliberate duplication: `hu_e2e_closed_loop_run` exists twice (test seam + release CLI helper)

**Decision:** The synchronous closed-loop runner exists in two physical locations:

1. `src/agent/lora_training_runner.c` under `#ifdef HU_IS_TEST`, used by `tests/test_e2e_rl_loop.c`. Compiled into `human_tests` only.
2. `src/ml/cli_demo.c` as `static hu_error_t cli_demo_run_closed_loop(...)`, called from `hu_ml_cli_demo_rl_closed_loop`. Compiled into `human` (release binary) only when `HU_ENABLE_RL_FULL=ON` (i.e. the `rl_sota` preset; default release stays clean). Implementation is copy-pasted from #1, structurally identical.

**Why duplication and not extraction:** AGENTS.md §3 (Rule of Three) — extract after 3 callers, not 2. We have 2 callers and the helper is 9 numbered steps of straight-line orchestration with no branching state. Extraction would require:

- A new public API surface in either `hu_lora_runner.h` or a new `hu_e2e.h` (both add binary surface area).
- A new test file pinning the extracted helper independently from the closed-loop test.
- A new `HU_ENABLE_*` option to gate the extracted helper.

Each of those costs more than the 80 LOC of duplication. If Phase 7+ adds a third caller (`human demo persona-overlay` or similar that needs the same train→swap→re-chat shape), extract THEN. The duplication is documented in the headers of both files with a back-reference, and `dead-code-finder` (Task 12) is configured to allow the duplication as long as both copies are reachable.

> **ME4 fix — cross-copy version pin (required):** to prevent silent drift between the two copies, BOTH copies define `HU_E2E_LOOP_IMPL_VERSION` and a `_Static_assert` that fires at compile time if a future contributor bumps the version in one copy without bumping the other:
>
> ```c
> /* Both src/agent/lora_training_runner.c (HU_IS_TEST gated) and
>  * src/ml/cli_demo.c (release-gated). Bump in lock-step. */
> #define HU_E2E_LOOP_IMPL_VERSION 1
> _Static_assert(HU_E2E_LOOP_IMPL_VERSION == 1,
>                "If you bumped HU_E2E_LOOP_IMPL_VERSION in one copy "
>                "(lora_training_runner.c or cli_demo.c) you MUST bump "
>                "the matching constant in the other copy. See plan D5 / ME4.");
> ```
>
> The `_Static_assert` references the literal `1` so the diff between the two copies is mechanically detectable by a future contributor (the assert fires when only one copy is bumped to `2` and the other still says `1`, because the assert's compared-against literal differs from the macro value). Compile fails fast; the duplication can no longer drift silently. The duplication is also documented in the leading comment block of each file with the back-reference `/* Phase 6 D5 duplicate — see ME4 */` so `git grep "Phase 6 D5 duplicate"` always surfaces both call sites.

### D6: Persona-fidelity delta is measured on the **same** held-out 100 prompts pre and post

**Decision:** Both the E2E test and the demo measure fidelity by:

1. Building the persona's `hu_communication_style_t` fingerprint **once** from `tests/fixtures/e2e_persona_seed.json` (E2E test) or from the user's `~/.human/personas/seth.json` (demo).
2. Calling `provider.chat_with_system(prompt_i)` for each of 100 fixed prompts from `tests/fixtures/e2e_persona_seed.json::held_out_prompts[]` (E2E test) or from `tests/fixtures/lora_baseline_persona_v2_responses.json::prompts[]` (Phase 5's 100-prompt fixture; demo).
3. Calling `hu_communication_style_fidelity_score(target, response, response_len)` for each response → array of 100 floats `before_scores[]`.
4. Closed-loop iteration (chat → reactions → train → swap).
5. Calling `provider.chat_with_system(prompt_i)` again on the **same** 100 prompts → array of 100 floats `after_scores[]`.
6. Computing `delta_mean = mean(after_scores) - mean(before_scores)` and a 95% bootstrap CI (1000 resamples, fixed seed `42` in test path, time-seeded in demo path) on the per-prompt deltas.

**Win condition:** `delta_mean ≥ 0.05` (absolute points on the [0.0, 1.0] scale, per spec §11 row 4) AND the lower bound of the 95% CI is `> 0` (so the delta is statistically distinguishable from noise — spec risk #8 mitigation, eval gate principle from spec §6.4 / §10 row 8).

**Why the same prompts:** controls for prompt difficulty. If we used different prompts pre/post we'd be measuring "are these other prompts easier?" not "did the adapter change anything." Per spec §11 row 4: "Held-out from training; versioned with eval; reviewers can inspect rubric and re-rate."

**Why bootstrap CI not point estimate:** spec §10 risk #8 explicitly calls this out — "Bootstrap CIs (not point estimates) on every metric. Lower 95% CI > baseline upper 95% CI required."

### D7: Demo persona seed = synthetic persona, NOT the real Seth corpus

**Decision:** `tests/fixtures/e2e_persona_seed.json` contains a fully synthetic persona definition:

```json
{
  "$schema": "https://human.dev/schemas/e2e-persona-seed-v1.json",
  "name": "demo_persona_e2e",
  "communication_style": {
    "lowercase_ratio_target": 0.85,
    "abbreviation_density_target": 0.30,
    "avg_response_length_target": 22,
    "decision_style": {
      "hedging_ratio_target": 0.10,
      "imperative_ratio_target": 0.40,
      "question_ratio_target": 0.05
    },
    "sample_count": 50,
    "samples": [
      "yeah ship it",
      "let's go",
      "do it now",
      "..."
    ]
  },
  "held_out_prompts": [
    "what should i do first?",
    "is this ready to deploy?",
    "..."
  ],
  "comment": "Synthetic E2E persona — no PII, no real corpus. Generated for tests/test_e2e_rl_loop.c. The communication style is intentionally extreme (lowercase + short + decisive) so the toy GPT moves measurably under 50 reaction signals."
}
```

**Why synthetic:**

- **Privacy.** The real Seth corpus lives under `~/.human/private/` (Phase 0 + spec §13) and never enters the repo. Tests cannot depend on it.
- **Determinism.** A committed fixture is byte-stable across machines and contributors; a real corpus is per-user.
- **Extreme persona helps the toy GPT.** The toy GPT has ~32 vocab tokens. A subtle persona (mostly indistinguishable from baseline) wouldn't move the fidelity scorer by 5% on 50 reaction signals. An extreme persona (very lowercase, very short, very decisive) makes the signal-to-noise ratio favorable enough that the test pin is reliable.

The demo (Task 9) uses the **real** persona via `--persona seth` (resolving to `~/.human/personas/seth.json`), which the user owns and never commits.

### D8: 50 reaction signals is the floor; the test fixture ships exactly 50

**Decision:** `tests/fixtures/e2e_reaction_signals.json` contains exactly 50 synthetic reaction events (25 positive 👍-class tapbacks + 25 negative 👎-class tapbacks), matching spec §11 row 5: "Min preference pairs before first DPO step: 50. Below 50, gradient noise dominates; trainer scheduler defers and logs `insufficient_data`."

**Why exactly 50:**

- **Matches the spec floor exactly** so the E2E test is at the boundary the trainer scheduler is calibrated for.
- **Pairs cleanly into 25/25** for an even mix of positive/negative DPO pairs.
- **Keeps the fixture small** (~10 KB JSON) and reviewable.
- **Easy to extend.** A future Phase 6.5 can add `e2e_reaction_signals_large.json` for stress tests without churning this one.

### D9: The demo and the test BOTH write `~/.human/proofs/<adapter-id>/` evidence directories per spec §8

**Decision:** Both the deterministic E2E test (Task 9 in code, gated to write to a temp dir under `tests/_tmp/proofs/<test-id>/` to avoid cluttering the user's `~/.human/`) AND the demo script (Task 9 prose, writing to the real `~/.human/proofs/<YYYY-MM-DD>-<method>-step-<N>/`) produce all 9 files from spec §8:

```
manifest.json
training_curves.json
eval_before.json
eval_after.json
eval_delta.json
delta_responses.md
gate_decision.json
adversarial_review.md
reproduce.sh
```

**Why both:** The test writes the dir to verify the **emission code** works; the demo writes the dir to ship the **actual proof**. The schema is the same; only the corpus differs. The DoD §10 ("`~/.human/proofs/<final-adapter-id>/` exists with all 9 evidence files") is satisfied by the demo run, not the test.

### D10: Determinism enforcement — three runs of the test produce **byte-identical** adapter SHA-256

**Decision:** Task 5 ships `test_e2e_closed_loop_deterministic_run1_vs_run2` and `test_e2e_closed_loop_deterministic_run1_vs_run3` (split per LO1 so each test isolates a single comparison) which run `hu_e2e_closed_loop_run` once each (with a fresh `hu_gpt_t` instance per run, all seeded the same way), compute SHA-256 of the resulting `<adapter_path>/lora.bin` byte stream after each run, and assert the hashes are byte-identical. Failure of this assertion means we have a hidden source of nondeterminism (RNG not seeded, time-of-day in the manifest, hash-table iteration order, etc.) — we MUST find and fix it before Phase 6 can ship.

> **HI3 contract (REQUIRED at every call site):** `srand(42)` must be reseeded **immediately before**:
>
> - each call to `bootstrap_lower_95` (the bootstrap helper itself also reseeds inside, as a defense-in-depth),
> - each `hu_gpt_t` initialization (so weight init is reproducible),
> - each call to `hu_e2e_closed_loop_run` from a determinism test (the `run_one_closed_loop_and_hash` helper in Task 5 does this).
>
> Seeding **only** in `set_up_env()` is insufficient: any intervening `rand()` consumer (test framework, libc functions, third-party hash-table iteration) shifts the RNG stream and silently breaks reproducibility on different run compositions. Determinism is per-call, not per-suite.

**Why this is non-negotiable:**

- **Without determinism, the test is a coin flip.** A stochastic E2E test that "usually passes" is worse than no test, because failures get reflexively retried instead of investigated.
- **Determinism is achievable** on toy GPT + HUML in-process trainer + fixed reaction stream. If we can't achieve it, something is wrong and we want to know.
- **The demo doesn't have this constraint** — it runs against MLX + real Gemma where kernel scheduling makes byte-identical adapters infeasible. The E2E test exists to enforce determinism precisely because the demo can't.

### D11: No `aspect-panel` requirement — Phase 6 ships behind `sprint-auditor` PASS only

**Decision:** Phase 6 does NOT trigger an `aspect-panel` (5-verifier) gate. Spec §7 mandates `aspect-panel` only for "high-risk phases (P2 dpo_real, P4 grpo, P5 eval_gate)." Phase 6 is wiring + evidence, not new math. The gate is `sprint-auditor` PASS + `critic` per code change ≥100 LOC + `verifier` per behavioral claim + `dead-code-finder` at phase end. Same set as Phases 0/1/3 had.

---

## Security

Phase 6 closes a loop that turns external signals (👍/👎 reactions) into gradient updates on a user-owned model. That is a direct **reward-hacking surface**: an adversary who can inject reactions can poison the preference DB, and an adversary who can place a file on disk can poison the loaded adapter. Both must be addressed before the demo ships.

### SEC1 — Reaction signal authenticity (preference-DB poisoning)

The closed loop accepts a reaction event, looks up the assistant message it targets, and writes a `hu_preference_pair_t` row into `dpo_pairs`. If reactions are accepted from any source, an adversary controlling an unverified iMessage handle / Slack workspace member / webhook endpoint can flip the persona's adapter at training time by emitting crafted 👍/👎 streams. Phase 6 MUST gate reaction acceptance on **all** of the following before promoting an adapter:

1. **Verified-user allowlist.** Phase 5 H8 ships the iMessage `access.json` allowlist (`docs/plans/2026-05-11-rl-loop-phase-5-eval-competitive.md` H8). Phase 6's demo + tests refuse to record a preference pair when the reaction's sender is not in the allowlist for that channel. Slack reactions are gated by workspace + channel membership using the existing Phase 2 `hu_channel_slack` allowlist surface; webhook reactions are gated on the signed-payload verification already in `src/channels/meta_common.h`.
2. **Per-session negative-rate cap.** If more than **30%** of reactions in a single training window are negative, the closed-loop runner logs an `anomaly: high_negative_rate` entry to the evidence dir and (a) the demo's `--require-positive-delta` path refuses to promote the adapter, (b) the deterministic test's `test_e2e_closed_loop_negative_rate_anomaly_recorded` (new in Task 6 step 7) asserts the anomaly fires when the synthetic stream is rebalanced to 35 negative / 15 positive. The cap is a defense against an adversary who only has the ability to inject 👎 (e.g. burner accounts).
3. **Anomaly logging to evidence dir.** Every gated decision (verified vs unverified sender, negative-rate vs cap) is appended to `~/.human/proofs/<adapter-id>/anomaly_log.jsonl` so a reviewer can audit the decision trail. The schema is `{ ts, channel, sender_hashed, decision, rationale }`; sender id is never logged in plaintext.

Without these three controls, the closed-loop is a direct reward-hacking surface. The deterministic test exercises (1) and (2) with synthetic allowlist and rate-cap fixtures; the demo exercises (3) against the real persona's reactions.

### SEC2 — Adapter file integrity (load-time poisoning)

`save_adapter` writes `lora.bin` to disk; `load_adapter` reads it back. Between those two calls an adversary with filesystem access could swap the file for a hand-crafted adapter that biases the model toward attacker-chosen outputs. Phase 6 MUST:

1. **SHA-256 round-trip verification.** `cli_demo.c::cli_demo_run_closed_loop` computes `hu_sha256` over the bytes returned from `save_adapter` (the canonical post-save read) and pins the digest in `manifest.json::adapter_sha256` BEFORE calling `hu_provider_load_adapter`. Immediately before `load_adapter`, it re-reads the file and recomputes the SHA. If the two digests differ, abort with exit code 3 and write an `anomaly: adapter_sha_mismatch` entry to the evidence dir. The verification uses the same `compute_file_sha256` helper introduced in Task 5 step 1 (ME2 — `hu_sha256` one-shot, not OpenSSL).
2. **Path validation.** `--out` must resolve under `~/.human/proofs/` (production) or the CMake-anchored `tests/_tmp/proofs/` (tests). Any other prefix is rejected with `HU_ERR_PERMISSION_DENIED` before any file is written. The test path is whitelisted via the `HU_E2E_TMP_ROOT` env var the test harness sets.
3. **Test pin.** `test_e2e_closed_loop_adapter_sha_mismatch_aborts_swap` (new in Task 6 step 8) tampers with the adapter file between `save_adapter` and `load_adapter` and asserts that the runner returns `HU_ERR_INTEGRITY` and writes the `anomaly_log.jsonl` entry.

The two SEC controls combined turn the closed loop from "anyone who can inject a reaction can move the adapter" into "only verified, rate-limited, integrity-checked signals can move the adapter."

---

## Risk register

| # | Risk | Severity | Mitigation |
|---|------|----------|------------|
| **R1** | **MLX adapter hot-swap during chat causes provider state corruption.** llama.cpp's `llama_adapter_lora_init` + `llama_set_adapters_lora` (Phase 1, `llamacpp.c:425-485`) was tested with cold-loaded adapters. The closed loop hot-swaps mid-session: the *same* `llama_context` that just produced `before_response` then loads a new adapter and produces `after_response`. If the in-flight KV cache is not properly invalidated (or the adapter weights are not properly applied to subsequent forward passes), `after_response` could be garbled, identical to `before_response` (silent no-op), or — worst case — produce a use-after-free in MLX. | High | (a) Phase 1's `llamacpp_load_adapter` (`src/providers/llamacpp.c:454-455`) already calls `llama_memory_clear` + `hu_llamacpp_kvcache_reset` internally on every `load_adapter`; verify at Task 0 step 3. The `hu_provider_vtable_t` has NO `kv_cache_clear` field — there is no Phase-6-owned C-side mitigation to add. (b) The deterministic E2E test in `tests/test_e2e_rl_loop.c` uses a mock provider that DOES NOT have this issue (toy GPT has no KV cache); the test thus pins the wiring but cannot pin this risk. (c) The demo script's first run on real Gemma is the canary — it MUST produce a different `after_response` than `before_response` (Task 12 step 3 asserts this via the demo CLI's exit code). (d) `tests/test_llamacpp_lora_hotswap_midsession.c` (NEW in Task 6 step 9) explicitly does chat→hotswap→chat with the real llama.cpp provider, gated by `HU_HAVE_GEMMA_GGUF=1` so it skips in CI without the model. |
| **R2** | **RM trained on too-narrow data (synthetic reaction stream has only 2 polarities) → RM collapses to constant output.** Phase 3's RM trainer expects diverse Bradley-Terry pairs. Our synthetic 25 👍 + 25 👎 stream produces uniformly-polarized pairs. The RM may overfit to a single feature (e.g. "any token in the chosen-side pile of vocab" → +1) and then provide useless reward signal during the demo's GRPO step. | Medium | (a) Phase 6 does NOT exercise RM training in the deterministic E2E test — DPO is the default path because DPO doesn't need an RM (preference pairs are the supervision). KTO and GRPO branches of the test are separate test functions that use the same 50-pair stream but skip the RM-training step (KTO doesn't need an RM either; GRPO uses the RM as the reward function). (b) The demo script's `--method grpo` path runs `human ml rm-train` on the **same** `dpo_pairs` rows the closed-loop iteration produced, deliberately surfacing the narrow-data limitation in `gate_decision.json`'s rationale field (so the proof artifact records that the RM was trained on a small homogeneous set). (c) DoD §6 default is `--method dpo` for the demo run that produces the win-condition scorecard; KTO and GRPO runs are reported separately so a degenerate RM doesn't tank the headline number. |
| **R3** | **Demo flakes on small reaction count.** 50 preference pairs may not move real Gemma's persona-fidelity score by ≥ 0.05 absolute points reliably (LoRA on ~16M params trained on 50 pairs is a small signal). | Medium | (a) Demo script defaults to `--reaction-count 200` (4× the spec §11 row 5 floor) and supports `--reaction-count <N>` override for users with smaller corpora. (b) Demo script logs the actual delta + bootstrap CI even when the win-condition fails, so the proof artifact records the honest result. (c) `docs/demos/rl-loop-demo.md` runbook documents the troubleshooting steps if the user's first run misses the threshold (try more reactions, longer training, different `beta` hyperparam — see Task 10 step 4). (d) The DoD's ≥ +0.05 threshold is on the demo's win-condition scorecard (DoD item 6); if the live demo can't hit it on synthetic data we mark Phase 6 NOT done and revisit. |
| **R4** | **Persona-fidelity delta noise too high to detect a 5% real signal.** Bootstrap CI on 100 prompts × per-prompt fidelity score may have width ≥ 0.10 (i.e. ±0.05 on the mean), making the lower-95% CI cross zero even when the point estimate is +0.05. | Medium | (a) Use 1000 bootstrap resamples (standard for production-grade bootstrap CIs; spec §10 risk #8). (b) Use stratified resampling by prompt category if `tests/fixtures/lora_baseline_persona_v2_responses.json` exposes categories; otherwise plain percentile bootstrap. (c) Report both point estimate AND 95% CI in `eval_delta.json`; the DoD test (Task 11) checks BOTH `delta_mean ≥ 0.05` AND `lower_95_ci > 0`. (d) If width is too high, increase the held-out set from 100 → 200 prompts (would require extending Phase 5's fixture; documented as a future enhancement, NOT a Phase 6 blocker). |
| **R5** | **HUML adapter is meaningless for real chat (toy GPT semantics ≠ Gemma semantics).** A reviewer might run the deterministic E2E test, see the assertion pass, and assume the closed loop also works on real Gemma. It does not — that requires the demo. | Medium | (a) `tests/test_e2e_rl_loop.c` includes a top-of-file comment block (Task 4 step 1) explicitly stating: "This test pins the WIRING. It does NOT pin the QUALITY of the resulting adapter on a real model. For real-Gemma proof, see `scripts/demo-rl-loop.sh` and `docs/demos/rl-loop-demo.md`." (b) `human demo rl-closed-loop --backend huml` (the toy backend) prints a banner: `"WARNING: HUML toy GPT — wiring proof only, NOT a real adapter. Use --backend mlx for real-Gemma demo."` (c) Documented in the runbook (Task 10) as the very first FAQ entry. |
| **R6** | **Phase 5 `eval_gate` not stable yet — Phase 6 may need to update or stub.** Phase 6 depends on `hu_eval_gate` being callable from the closed-loop runner. If Phase 5 ships with `eval_gate` returning placeholder values or with API drift, the Phase 6 test harness can't compose. | Medium | (a) Phase 6 Task 0 (start gate) verifies Phase 5 deliverables exist + compile clean before any Phase 6 code lands. (b) If `eval_gate` ships as a stub, the Phase 6 closed-loop runner falls back to calling `hu_communication_style_compare_response_sets` directly (Phase 1 of Track D ships this) — strictly less than `eval_gate` because no leaderboard regression check, but enough to gate the test PASS/FAIL. (c) Phase 6's `gate_decision.json` schema is documented in spec §8 and is independent of `eval_gate`'s implementation — the Phase 6 demo writes the JSON regardless of whether `eval_gate` produced it. |
| **R7** | **ASan slowdown causes 30 s budget violation on slower CI runners.** GitHub Actions Linux runners are not as fast as M2; ASan-instrumented toy GPT × 50 steps × 100 fidelity-scored prompts may push past 30 s. | Low | (a) Profile early: Task 4 step 6 includes a wall-clock measurement (`getrusage` or `clock_gettime(CLOCK_MONOTONIC)`) and asserts `elapsed_ms ≤ 30_000`. If the assertion fires, Task 4's step 7 reduces fixture sizes proportionally (50 → 25 reactions, 100 → 50 held-out prompts) and re-runs. (b) The 30 s budget is for the single `test_e2e_closed_loop_dpo_shows_measurable_response_change` test function — other test functions in the same file (KTO, GRPO, determinism) each have their own budget. The `--suite=E2E-closed-loop` selector lets developers run just this suite during iteration. (c) If after fixture trimming the budget still slips, document the actual elapsed_ms in the test, lift the assertion to ≤ 60 s with a TODO referencing this risk, and file a follow-up to investigate (e.g. profile-guided HUML optimizations). |
| **R8** | **CI runner clock drift / rusage nondeterminism corrupts `manifest.json`.** Spec §8's `manifest.json` records training timestamp + hyperparams + base model SHA. If the test embeds wall-clock time into the manifest, Task 8's byte-identical-SHA assertion (D10) will fail. | Low | (a) Test path uses `HU_E2E_FIXED_TIMESTAMP="2026-05-12T00:00:00Z"` env var (set in `test_e2e_rl_loop.c::set_up_env`) to override `time(NULL)` calls in the manifest writer. The manifest writer reads this env var via `getenv` and uses it instead of real time when set. (b) Demo path does NOT set the env var → real timestamps → real evidence dirs. (c) Test asserts SHA of `manifest.json` matches a committed expected value pinned in the test source (regenerate-on-fail flow documented in test comment). |

---

## Common-failure-mode table

These are the failure modes most likely to surface during Phase 6 implementation OR during the live demo. Each has a deliberate test pin so a regression fires the canary.

| # | Failure mode | Why it happens | Test pin |
|---|---|---|---|
| **F1** | **Closed-loop step doesn't propagate adapter to next chat.** The provider holds stale weights — `chat_with_system` returns the same response after the swap because the adapter never actually applied. | `hu_provider_load_adapter` returned `HU_OK` but the underlying `llama_set_adapters_lora` was a no-op; OR the provider's vtable points to a stale `chat_with_system` that pre-loaded weights at startup; OR mock provider's `chat_with_system` function ignores the loaded adapter. | `tests/test_e2e_rl_loop.c::test_e2e_closed_loop_provider_after_response_differs_from_before` — explicit `HU_ASSERT_NEQ(memcmp(out.before_response, out.after_response, ...), 0)` after `hu_e2e_closed_loop_run` returns. Uses MOCK provider (the synthetic deterministic one) so the test failure isolates "the wiring asks the provider for a re-chat after swap" from "the real llama.cpp does the right thing." |
| **F2** | **Reaction event gets dropped between channel and preference DB.** Emits `hu_reaction_event_t`, `hu_reaction_handler_handle_event` returns `HU_OK`, but no row appears in `dpo_pairs`. | Race between event emit and SQLite commit; OR `hu_reaction_handler_handle_event` silently drops events with unknown target (Phase 2 R4 mitigation: spec'd to drop silently, but in the closed-loop case ALL events should have known targets); OR collector's pair-insert is conditional on a flag the test forgot to set. | `tests/test_e2e_rl_loop.c::test_e2e_closed_loop_all_synthetic_reactions_become_dpo_pairs` — emits 50 reactions, calls `hu_dpo_pair_count(collector, &n)`, asserts `n == 50`. Plus checks each row's `source` field is `"e2e_synthetic"` (the Phase 6 fixture's source tag). |
| **F3** | **eval_gate's bootstrap CI too wide on small N.** 50 reaction signals + 100 held-out prompts produces a per-prompt delta variance large enough that lower-95-CI < 0 even when point estimate is positive. The win-condition test passes on point estimate but fails the CI gate. | Per-prompt fidelity score has high variance because the toy GPT is small; OR the bootstrap resampling is naive and not stratified; OR the CI implementation uses the wrong percentiles. | `tests/test_e2e_rl_loop.c::test_e2e_closed_loop_bootstrap_ci_lower_bound_is_positive` — runs the full closed-loop iteration and asserts `lower_95_ci > 0` on the per-prompt delta distribution. If this fires, the test message includes both the point estimate and the CI bounds so the failure is immediately diagnosable. |
| **F4** | **Demo run nondeterministic across machines.** Two reviewers run `scripts/demo-rl-loop.sh` on identical M2 + identical Gemma checkpoint → produce different scorecard numbers. | MLX kernel scheduling float-order on parallel matmul; OR llama.cpp Metal sampling temperature noise; OR `time(NULL)` in the adapter id; OR Python venv version skew. | Acknowledged-by-design (D10 — only the deterministic E2E test asserts byte-identical hashes; demo cannot). Demo runbook (`docs/demos/rl-loop-demo.md`, Task 10) documents this explicitly and provides expected-range bands for the win-condition numbers (e.g. "delta should be in [0.05, 0.15] on the synthetic corpus") rather than exact values. Reviewers comparing absolute numbers across machines is a documentation bug, not a test failure. |
| **F5** | **Adapter hot-swap corrupts in-flight KV cache → garbled next response.** `before_response` was generated with KV cache populated; the swap loads the new adapter but the cache from the prior decode is now mis-attributed; `after_response` could be token soup. | llama.cpp's `llama_set_adapters_lora` does NOT, by itself, clear the KV cache — adapter changes are applied to subsequent forwards but cached attention states from before the swap remain. | **Fixed by Phase 1**, not Phase 6: `src/providers/llamacpp.c::llamacpp_load_adapter` (lines 454-455) already calls `llama_memory_clear` + `hu_llamacpp_kvcache_reset` internally before returning from `load_adapter`. The `hu_provider_vtable_t` has **no** `kv_cache_clear` field (`include/human/provider.h:205-257`), so `hu_e2e_closed_loop_run` does not — and cannot — invoke one. The R1/F5 risk is owned by Phase 1's load_adapter implementation; Task 0 step 3 verifies the call sites still exist. `tests/test_llamacpp_lora_hotswap_midsession.c` (NEW in Task 6 step 9, gated by `HU_HAVE_GEMMA_GGUF=1` so it skips in CI without the model) asserts `after_response` is non-empty, contains no NUL bytes mid-stream, decodes to UTF-8, and differs from `before_response` — i.e. exercises the post-swap correctness end-to-end against real llama.cpp without re-implementing the KV-clear logic in Phase 6. |
| **F6** | **Trainer no-ops because pending signals buffer empty when scheduled.** Production async path: reaction events arrive, get queued in `hu_learner_pending_drain`, scheduler wakes the lora-training-runner callback, callback drains 0 events because the events were never enqueued (race between `hu_reaction_handler_handle_event` and `hu_learner_bridge_emit_*`). | The reaction handler writes to `dpo_pairs` SQLite directly but the learner-bridge is a separate in-memory queue; if Phase 6's closed-loop wiring forgets to ALSO enqueue into the learner-bridge, the async runner will never see the events. | `tests/test_e2e_rl_loop.c::test_e2e_closed_loop_async_path_drains_pending_signals_after_reactions` — runs the closed loop via the ASYNC path (`hu_lora_training_runner` registered with a stub scheduler that fires once after reactions are emitted) instead of the synchronous `hu_e2e_closed_loop_run`. Asserts the runner sees `n > 0` events to drain. This pins the contract that Phase 2's reaction handler MUST also call `hu_learner_bridge_emit_*` when running in production — the test fails if the async-path enqueue is missing. |

---

## Adapter swap evidence (the proof artifact)

Per spec §8, every adapter promotion writes `~/.human/proofs/<adapter-id>/`. Phase 6 adds the missing emission code in `cli_demo.c` and pins the schema in the deterministic test. The fields the win-condition scorecard depends on:

| # | Field | Where measured | Test pin |
|---|---|---|---|
| **E1** | **Persona-fidelity score, before / after, on the same held-out 100 prompts.** Mean of `hu_communication_style_fidelity_score(target, response_i, len_i)` for `i ∈ [0, 100)` on the same prompts pre and post. Includes 95% bootstrap CI on the per-prompt delta. | `eval_before.json` + `eval_after.json` + `eval_delta.json`. Schema: `{ mean: float, min: float, max: float, n: int, lower_95_ci: float, upper_95_ci: float, per_prompt_scores: float[100] }`. | `tests/test_e2e_rl_loop.c::test_e2e_closed_loop_bootstrap_ci_lower_bound_is_positive` (F3 pin) + `test_e2e_closed_loop_persona_fidelity_delta_meets_threshold` (Task 11 step 4). |
| **E2** | **Style match on 4 axes (formality / hedging / decision-style / vocab), before / after.** Per-axis breakdown of the fidelity score, surfacing whether the adapter improved on all axes uniformly or only on one. Phase 5 added the 4th axis; Phase 6 reads all four. | `eval_before.json::axes` + `eval_after.json::axes` (per-axis float array of length 4); `eval_delta.json::axes` (per-axis float deltas). | `tests/test_e2e_rl_loop.c::test_e2e_closed_loop_axis_breakdown_present_in_evidence_dir` (Task 9 step 4). |
| **E3** | **Latency p95, before / after.** Mean + p95 + p99 of `chat_with_system` wall-clock per response, measured across the same 100 held-out prompts. Win-condition: p95 regression ≤ 2% (DoD item 7). | `eval_before.json::latency_ms` + `eval_after.json::latency_ms` (`{ mean: float, p50: float, p95: float, p99: float, n: int }`); `eval_delta.json::latency_p95_ratio` (after/before). | `tests/test_e2e_rl_loop.c::test_e2e_closed_loop_latency_regression_within_2_percent` (Task 11 step 5). The toy GPT path bypasses real timing and asserts the schema is populated; the demo path measures real wall-clock. |
| **E4** | **Number of preference pairs consumed.** Count of rows in `dpo_pairs` that the trainer actually trained on (= the `n` returned from `hu_dpo_pair_count` AFTER the closed-loop iteration; should equal the number of synthetic reactions emitted, modulo Phase 2's silent-drop policy for unknown targets). | `manifest.json::preference_pairs_consumed` (int) + `manifest.json::reactions_emitted` (int); the difference is the silent-drop count. | `tests/test_e2e_rl_loop.c::test_e2e_closed_loop_evidence_manifest_records_pair_count` (Task 9 step 4). |
| **E5** | **Wall-clock time for full closed-loop iteration.** End-to-end seconds from `chat → reactions → train → swap → re-chat`. Reported in the manifest so reviewers can compare across hardware. | `manifest.json::elapsed_ms` (int64). | `tests/test_e2e_rl_loop.c::test_e2e_closed_loop_completes_within_30s_under_asan` (Task 4 step 6). |

The full evidence directory schema is the union of E1–E5 plus the spec §8 mandatory files (`delta_responses.md`, `gate_decision.json`, `adversarial_review.md`, `reproduce.sh`). Phase 6 ships the emission code for ALL of these in `src/ml/cli_demo.c::write_evidence_dir`.

---

## File structure

### New files (8):

| Path | LOC | Responsibility |
|------|-----|----------------|
| `tests/test_e2e_rl_loop.c` | ~700 | The canonical deterministic E2E test. Test functions: `test_e2e_closed_loop_dpo_shows_measurable_response_change`, `test_e2e_closed_loop_kto_shows_measurable_response_change`, `test_e2e_closed_loop_grpo_shows_measurable_response_change` (each gated as appropriate), `test_e2e_closed_loop_provider_after_response_differs_from_before` (F1 pin), `test_e2e_closed_loop_all_synthetic_reactions_become_dpo_pairs` (F2 pin), `test_e2e_closed_loop_bootstrap_ci_lower_bound_is_positive` (F3 pin), `test_e2e_closed_loop_async_path_drains_pending_signals_after_reactions` (F6 pin), `test_e2e_closed_loop_completes_within_30s_under_asan` (R7 pin), `test_e2e_closed_loop_deterministic_run1_vs_run2` + `test_e2e_closed_loop_deterministic_run1_vs_run3` (D10 pin, LO1-split), `test_e2e_closed_loop_evidence_manifest_records_pair_count` (E4 pin), `test_e2e_closed_loop_axis_breakdown_present_in_evidence_dir` (E2 pin), `test_e2e_closed_loop_latency_regression_within_2_percent` (E3 pin), `test_e2e_closed_loop_persona_fidelity_delta_meets_threshold` (DoD item 6 pin), `test_e2e_closed_loop_negative_rate_anomaly_recorded` (SEC1 pin), `test_e2e_closed_loop_adapter_sha_mismatch_aborts_swap` (SEC2 pin). Plus a single `void run_e2e_closed_loop_tests(void)` runner that calls them in order with `HU_TEST_SUITE("E2E-closed-loop")` declared INSIDE the function body (not at file scope — see Critical Rules below). |
| `tests/fixtures/e2e_persona_seed.json` | ~150 lines | Synthetic persona definition (D7) + 100 held-out prompts + 50 sample style fingerprints. Schema documented in the file's `$schema` field; reader is `tests/test_e2e_rl_loop.c::load_persona_seed`. |
| `tests/fixtures/e2e_reaction_signals.json` | ~250 lines | Synthetic reaction stream (D8) — exactly 50 events, 25 positive (`{"kind": "love", "polarity": 1, "target": {...}}`) + 25 negative (`{"kind": "dislike", "polarity": -1, "target": {...}}`). Each event has a `target` referencing a prompt+response pair so `hu_reaction_handler_handle_event` can look up the chosen/rejected text. Reader is `tests/test_e2e_rl_loop.c::load_reaction_signals`. |
| `src/ml/cli_demo.c` | ~400 | `hu_ml_cli_demo_rl_closed_loop` — argv parser (`--persona`, `--method {dpo,kto,grpo}`, `--backend {huml,mlx}`, `--reaction-count N`, `--prompt P`, `--out <evidence-dir>`, `--require-positive-delta`), provider creation (Phase 1 llamacpp factory or HUML mock), trainer creation (Phase 2/3/4 factories), reaction synthesis (or live load from a `--reactions <jsonl>` file), the static `cli_demo_run_closed_loop` (D5 duplicate of `hu_e2e_closed_loop_run`), evidence-dir writer (`write_evidence_dir` emits all 9 spec §8 files), exit-code mapping (0 = win-condition met, 2 = win-condition missed, 3 = harness error). |
| `include/human/ml/cli_demo.h` | ~30 | Public declaration of `hu_ml_cli_demo_rl_closed_loop(int argc, const char **argv, hu_allocator_t *alloc)` and the `hu_ml_cli_demo_subcommand_dispatch` shim called from `cmd_demo` in `src/main.c`. Header guards `HU_ML_CLI_DEMO_H`. |
| `scripts/demo-rl-loop.sh` | ~200 | Bash demo runner. Verifies prerequisites (`bash scripts/fetch-gemma.sh`, `bash scripts/fetch-qwen-rm.sh`, `python3 -c "from mlx_lm_lora.trainer.dpo_trainer import train_dpo"`), prompts user for confirmation, runs `./build-rl-sota/human demo rl-closed-loop --persona "${PERSONA:-seth}" --method "${METHOD:-dpo}" --backend mlx --reaction-count "${REACTION_COUNT:-200}" --prompt "${PROMPT:-what should I do first?}" --out ~/.human/proofs/$(date -u +%Y-%m-%d)-${METHOD:-dpo}-step-$$ --require-positive-delta`, prints the evidence dir path, opens the runbook in `$PAGER` (or `cat`s it). Honors `DRY_RUN=1` for runbook validation in CI without invoking `human`. |
| `docs/demos/rl-loop-demo.md` | ~400 lines | The runbook. YAML frontmatter (required by `scripts/check-docs-frontmatter.sh`) + sections: 1. Prerequisites (Apple Silicon + macOS 13+ + Python 3.11+ venv with mlx-lm + mlx-lm-lora + 5 GB free disk for GGUF). 2. One-command setup (clone, fetch models, build `rl_sota` preset). 3. Running the demo (env vars, expected output, expected wall-clock ~3 min). 4. Reading the evidence dir (file-by-file walkthrough of all 9 spec §8 files). 5. Troubleshooting (R3 mitigation: smaller delta? try more reactions; F4: numbers don't match across machines? expected, document). 6. Reproducibility recipe (mirrors spec §14 verbatim). 7. FAQ (R5: HUML vs MLX; F4: nondeterminism; "can I run this in CI?" → no, see §6.5). 8. Citations (spec, prior phase plans, Apple FM docs, mlx-lm-lora). |
| `tests/_tmp/.gitkeep` | 0 | Empty file pinning the per-test temp directory (test code writes evidence to `tests/_tmp/proofs/<test-id>/` to keep the user's `~/.human/` clean during test runs). **LO3 fix:** the absolute path is resolved at runtime via `hu_e2e_tmp_root()` (Phase 6 helper that reads the `HU_E2E_TMP_ROOT` env var CMake sets to `${CMAKE_BINARY_DIR}/tests/_tmp` via `set_tests_properties(... ENVIRONMENT ...)`), so tests work regardless of CWD. `hu_e2e_tmp_path(buf, n, "proofs/dpo-step-0001/lora.bin")` concatenates root + relative tail. `.gitignore` adds `tests/_tmp/proofs/` (everything except `.gitkeep`). |

### Modified files (4):

| Path | Delta | What changes |
|------|-------|--------------|
| `src/agent/lora_training_runner.c` | +130 LOC inside `#ifdef HU_IS_TEST` block | Add `hu_e2e_closed_loop_input_t` + `hu_e2e_closed_loop_output_t` structs (declared in a new test-only header `tests/include/hu_e2e_closed_loop.h` so they don't pollute the public surface) and `hu_e2e_closed_loop_run` function (D4). Also add the close-the-loop wiring fold-in if Phase 5 deferred it: ensure `hu_lora_training_runner` calls `hu_eval_gate` before promoting the adapter (verify at Task 0 step 5; if the call site is already there, this delta is zero — Phase 5 owns it). |
| `src/main.c` | +25 LOC | Add `cmd_demo` static function (12 LOC mirroring `cmd_ml`'s structure), declare it at the forward-decl block (~line 199), register it in `commands[]` array (~line 510 — single new row: `{"demo", "Reproducible end-to-end demonstrations (RL closed loop)", cmd_demo}`). The dispatcher inside `cmd_demo` calls `hu_ml_cli_demo_rl_closed_loop` for `argv[2] == "rl-closed-loop"`, prints help otherwise. |
| `tests/test_main.c` | +5 LOC | Forward-declare `void run_e2e_closed_loop_tests(void)` and add a single call to it in the appropriate place (between existing E2E suites, locate by `rg -n 'run_e2e' tests/test_main.c`). READ first; APPEND only; do NOT replace existing runners — every prior phase has burned a token on this rule and Phase 6 honors it. |
| `CMakeLists.txt` | +35 LOC | (See "CMakeLists.txt entries" section below for the exact diff; goes inside the existing `if(HU_ENABLE_RL_FULL)` guard block established by Phase 2.) |
| `.gitignore` | +2 lines | Add `tests/_tmp/proofs/` and a negative pattern preserving `tests/_tmp/.gitkeep` (or just `!tests/_tmp/.gitkeep` after the directory ignore). |
| `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md` | ~1 row update | At Task 12 end-gate: mark Phase 6 row complete with sprint-auditor verdict, actual test counts, and a link to `docs/proof/rl-loop-proof.md`. |

**Total Phase 6: ~600 LOC new tests + ~430 LOC new C (300 + 30 + 100 wiring) + ~200 LOC bash + ~400 lines markdown + ~400 lines fixture JSON.** Smallest LOC of any RL phase. The work is composition, not invention.

---

## CMakeLists.txt entries

Verbatim diff (insert at the existing `if(HU_ENABLE_RL_FULL)` block established by Phase 2):

```cmake
# Phase 6 — E2E closed-loop proof + demo CLI
if(HU_ENABLE_RL_FULL)
  list(APPEND HU_CORE_SOURCES
    src/ml/cli_demo.c
  )
  # NB: src/agent/lora_training_runner.c is ALREADY in HU_CORE_SOURCES
  # (Phase 5 owns it). The Phase 6 additions are inside #ifdef HU_IS_TEST,
  # so they compile into human_tests but not into the release human binary.
endif()

# Phase 6 — E2E closed-loop test (always compiled into human_tests; the
# test itself uses HU_SKIP_IF to gate on rl_sota preset features when
# needed, so the binary remains buildable under the dev preset).
list(APPEND HU_TEST_SOURCES
  tests/test_e2e_rl_loop.c
)

# Phase 6 — fixture data installed under build/tests/fixtures/ so the
# test can locate them via a relative path independent of CWD.
set(HU_E2E_FIXTURES
  tests/fixtures/e2e_persona_seed.json
  tests/fixtures/e2e_reaction_signals.json
)
foreach(_fx ${HU_E2E_FIXTURES})
  configure_file(${CMAKE_SOURCE_DIR}/${_fx}
                 ${CMAKE_BINARY_DIR}/${_fx} COPYONLY)
endforeach()

# Phase 6 — make the test-only header available to tests/ TU only.
target_include_directories(human_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/include)

# Phase 6 LO3 — anchor the per-test scratch dir to an ABSOLUTE path
# under the build tree so tests are CWD-independent. The test reads
# the env var via hu_e2e_tmp_root() and concatenates relative tails
# via hu_e2e_tmp_path(); CMake creates the dir up front. The test
# target name in this tree is `unit_tests` (see CMakeLists.txt
# line 3218: add_test(NAME unit_tests COMMAND human_tests)) so we
# pin properties on that ctest entry.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/tests/_tmp)
set_tests_properties(unit_tests PROPERTIES
  ENVIRONMENT "HU_E2E_TMP_ROOT=${CMAKE_BINARY_DIR}/tests/_tmp")
# Direct ./build/human_tests invocation (without ctest) reads the
# env var from the calling shell; agent-preflight.sh and CI both
# export it before invoking the binary. The test's hu_e2e_tmp_root()
# helper falls back to "${CMAKE_BINARY_DIR}-equivalent" baked in via
# a configure_file-generated header if HU_E2E_TMP_ROOT is unset.

# Phase 6 — gated cross-check that exercises real llama.cpp hot-swap
# mid-session (R5/F5 pin). Skipped automatically when HU_HAVE_GEMMA_GGUF
# is unset — the test calls HU_SKIP_IF on a getenv("HU_HAVE_GEMMA_GGUF")
# probe + access(gguf_path, R_OK) check, so no extra CMake plumbing
# beyond appending the source is needed.
if(HU_ENABLE_LLAMACPP)
  list(APPEND HU_TEST_SOURCES
    tests/test_llamacpp_lora_hotswap_midsession.c
  )
endif()
```

**Default release preset stays at `HU_ENABLE_RL_FULL=OFF`** — Phase 6's release-binary delta is exactly zero new bytes (the `cli_demo.c` and the `lora_training_runner.c` additions are both gated). Spec §6.4 hard sanity gate (≤ +250 KB vs prior tagged release) is trivially satisfied.

The `rl_sota` preset (Phase 1) compiles the new code; the `dev` preset compiles the test (which auto-skips real-model branches). The `tests/_tmp/proofs/` directory is created on first test run via `mkdir -p` in the test's setup helper.

---

## Tasks

### Task 0: Phase 6 start gate

**Files:** none (subagent dispatch + verification commands)

- [ ] **Step 1: Verify Phases 0–5 tags and deliverables**

```bash
for phase in 0 1 2 3 4 5; do
  git tag --list "rl-sota-phase-${phase}-complete" | grep -q "rl-sota-phase-${phase}-complete" \
    && echo "Phase ${phase} tag OK" \
    || { echo "ERROR: Phase ${phase} not complete"; exit 1; }
done
test -f docs/plans/2026-05-11-rl-loop-phase-5-eval-competitive.md && echo "Phase 5 plan exists"
test -f include/human/eval/eval_gate.h && echo "eval_gate.h exists (Phase 5)"
test -f include/human/ml/grpo.h && echo "grpo.h exists (Phase 4)"
test -f include/human/ml/reward_model.h && echo "reward_model.h exists (Phase 3)"
test -f include/human/ml/dpo_real.h && echo "dpo_real.h exists (Phase 2)"
test -f include/human/providers/llamacpp_decode.h && echo "llamacpp_decode.h exists (Phase 1)"
```

All should print OK. If any fails, abort — the predecessor phase was not actually closed.

- [ ] **Step 2: Verify test baseline under dev + rl_sota presets**

```bash
cmake --preset dev && cmake --build --preset dev -j
./build-dev/human_tests | tail -3   # must show pass count from end of Phase 5
cmake --preset rl_sota && cmake --build --preset rl_sota -j
./build-rl-sota/human_tests | tail -3
```

Record the pre-Phase-6 pass counts as the baseline; Phase 6 must end at baseline + (number of new test functions in `tests/test_e2e_rl_loop.c`) + 1 (for the gated `tests/test_llamacpp_lora_hotswap_midsession.c`).

- [ ] **Step 3: Verify llama.cpp hot-swap clears the KV cache (R1)**

```bash
rg -n 'llama_kv_cache_clear|kv_cache.*clear' src/providers/llamacpp.c
```

Expected: at least one call inside the `load_adapter` function (line range Phase 1's plan documented as 425–485). If absent, this is a Phase 1 bug — file an issue and BLOCK Phase 6 until fixed; do NOT work around it in `hu_e2e_closed_loop_run`.

- [ ] **Step 4: Verify Phase 5 wired eval_gate into the lora training runner (R6)**

```bash
rg -n 'hu_eval_gate' src/agent/lora_training_runner.c
```

If present (likely, per Phase 5's plan): Task 3 of Phase 6 has zero LOC — the close-the-loop wiring is already done.
If absent: Phase 5 deferred this; Phase 6 Task 3 picks it up. Expand the Task 3 step list at execution time to include the wiring change.

- [ ] **Step 4b: Verify Phase 5 H8 — production reaction-handler registration API exists (HI4)**

```bash
rg -n 'hu_reaction_handler_register_assistant_message_for_production' \
   include/human/agent/reaction_handler.h
```

Expected: at least one match, declared OUTSIDE any `#if HU_IS_TEST` block (the `_for_test` variant already exists at line 63 of that header but is compiled out of release builds, so the demo cannot rely on it). If absent, this is the Phase 5 H8 fix; BLOCK Phase 6 and complete H8 in Phase 5 first. The demo's `synthesize_reactions(n)` pre-registers `(prompt, response)` tuples via this API before emitting reactions — without it, every reaction silently drops in release builds and `hu_dpo_pair_count` returns 0.

- [ ] **Step 5: Branch from Phase 5 tag**

```bash
git checkout -b rl-sota-phase-6 rl-sota-phase-5-complete
```

If working tree has Track D Phase 1 WIP, `git stash push -- src/main.c src/ml/cli.c` first.

- [ ] **Step 6: Dispatch `spec-verifier` subagent**

```
Task: spec-verifier
Prompt: Read docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md §4.7
        + §5 row 6 + §6 Tier 4 + §8 + §9 items 8/10/11 + §14, and
        docs/plans/2026-05-11-rl-loop-phase-6-proof.md (this file). Report any gap
        between what the spec promises and what this Phase 6 plan delivers.
        Pay particular attention to: the win-condition delta of ≥ +0.05 absolute
        points (spec §11 row 4), the 9-file evidence dir schema (spec §8), the
        ≤ 30 second CI budget (this plan D1 + R7), and the Apple-Silicon-only
        demo positioning (this plan D2 + spec §6.5 final paragraph).
        Gate criterion: 0 gaps required to start Phase 6.
```

- [ ] **Step 7: Confirm 0 gaps before proceeding**

If `spec-verifier` reports gaps, amend this plan inline before any code change.

---

### Task 1: Commit the synthetic persona seed fixture

**Files:**
- Create: `tests/fixtures/e2e_persona_seed.json`

- [ ] **Step 1: Author the fixture**

The schema must be self-documenting via `$schema` and field names. The persona must be intentionally extreme (D7) so the toy GPT moves measurably under 50 reaction signals. The 100 held-out prompts must be diverse enough to give the bootstrap CI room to converge but uniform enough in expected style that the persona signal dominates.

```json
{
  "$schema": "https://human.dev/schemas/e2e-persona-seed-v1.json",
  "name": "demo_persona_e2e",
  "version": "1.0.0",
  "comment": "Synthetic E2E persona — no PII, no real corpus. Generated for tests/test_e2e_rl_loop.c per docs/plans/2026-05-11-rl-loop-phase-6-proof.md D7.",
  "communication_style": {
    "lowercase_ratio_target": 0.85,
    "abbreviation_density_target": 0.30,
    "avg_response_length_target": 22,
    "decision_style": {
      "hedging_ratio_target": 0.10,
      "imperative_ratio_target": 0.40,
      "question_ratio_target": 0.05
    },
    "sample_count": 50,
    "samples": [
      "yeah ship it",
      "let's go",
      "do it now",
      "nope",
      "yep",
      "..."
    ]
  },
  "held_out_prompts": [
    "what should i do first?",
    "is this ready to deploy?",
    "..."
  ]
}
```

The `samples` array must contain at least 50 entries so the fingerprint computation is statistically meaningful (`hu_communication_style_fingerprint_from_samples` requires `n ≥ 10` per its existing contract, but Phase 5's 4-axis variant prefers 50+). The `held_out_prompts` array must contain exactly 100 entries (committed verbatim, no generators — reproducibility).

- [ ] **Step 2: Verify the JSON parses**

```bash
python3 -m json.tool tests/fixtures/e2e_persona_seed.json > /dev/null && echo "valid JSON"
jq '.samples | length' tests/fixtures/e2e_persona_seed.json   # should print 50
jq '.held_out_prompts | length' tests/fixtures/e2e_persona_seed.json   # should print 100
```

- [ ] **Step 3: Commit**

```bash
git add tests/fixtures/e2e_persona_seed.json
git commit -m "test(e2e): add synthetic persona seed for Phase 6 closed-loop test

Synthetic persona with 50 samples + 100 held-out prompts. The persona
is intentionally extreme (lowercase + short + decisive) so the toy GPT
moves measurably under 50 reaction signals. No PII, no real corpus —
the real-Seth corpus is for the demo, not the test.

Refs spec §11 row 4, plan D7."
```

---

### Task 2: Commit the synthetic reaction-signals fixture

**Files:**
- Create: `tests/fixtures/e2e_reaction_signals.json`

- [ ] **Step 1: Author the fixture**

Exactly 50 events (D8). Each event has a `target` referencing the prompt+response pair the reaction is on (so `hu_reaction_handler_handle_event` can look up the chosen/rejected text). The `target.prompt` field is one of the prompts from `e2e_persona_seed.json::held_out_prompts[]` (so the test loads both fixtures and joins them); the `target.response_chosen` / `response_rejected` are short synthetic strings.

```json
{
  "$schema": "https://human.dev/schemas/e2e-reaction-signals-v1.json",
  "version": "1.0.0",
  "source_tag": "e2e_synthetic",
  "comment": "Synthetic reaction stream for Phase 6 deterministic E2E test. Exactly 25 positive + 25 negative reactions matching the spec §11 row 5 floor of 50 preference pairs.",
  "events": [
    {
      "event_id": "e2e-pos-001",
      "channel_kind": "synthetic",
      "channel_id": "test-imessage",
      "kind": "love",
      "polarity": 1,
      "ts_ms": 1715472000000,
      "target": {
        "prompt": "what should i do first?",
        "response_chosen": "ship the small fix.",
        "response_rejected": "perhaps consider exploring options."
      }
    },
    "..."
  ]
}
```

The `polarity` field is `+1` for desirable (👍, ❤️, 😂, 😮) reactions and `-1` for undesirable (👎). The `kind` field maps to the existing `hu_reaction_kind_t` enum (per `include/human/channels/reaction_event.h`). Exactly 25 events have polarity `+1` and 25 have polarity `-1`. The `ts_ms` is monotonically increasing (so any sort-by-time ordering is stable).

- [ ] **Step 2: Verify and commit**

```bash
python3 -m json.tool tests/fixtures/e2e_reaction_signals.json > /dev/null
jq '[.events[] | select(.polarity == 1)] | length' tests/fixtures/e2e_reaction_signals.json   # 25
jq '[.events[] | select(.polarity == -1)] | length' tests/fixtures/e2e_reaction_signals.json  # 25
git add tests/fixtures/e2e_reaction_signals.json
git commit -m "test(e2e): add synthetic 50-event reaction stream for closed-loop test

25 positive + 25 negative reactions matching the spec §11 row 5 floor
of 50 preference pairs. Each event references a prompt from
e2e_persona_seed.json::held_out_prompts[] so the reaction handler can
construct (prompt, chosen, rejected) tuples for hu_dpo_record_pair.

Refs plan D8."
```

---

### Task 3: Verify (or fold in) the close-the-loop wiring in `lora_training_runner.c`

**Files:** (potentially) `src/agent/lora_training_runner.c`

- [ ] **Step 1: Check the current state per Task 0 step 4**

```bash
rg -n 'hu_eval_gate' src/agent/lora_training_runner.c
```

If the search returns at least one call site between `hu_provider_load_adapter` (line ~112 per the file as of Phase 5 close) and the KV-warming enqueue (line ~127), Phase 5 already did the wiring → SKIP to Step 3.

If absent: Phase 5 deferred → continue to Step 2.

- [ ] **Step 2: Add the eval_gate call (only if Step 1 surfaced its absence)**

Insert between the existing `hu_provider_load_adapter` call and the `hu_scheduler_enqueue` for KV warming. The fold-in is small enough to inline:

```c
/* Phase 6 fold-in: gate the adapter on eval before promoting.
 * If the gate rejects, log + skip the swap. The trainer's adapter
 * file remains on disk for offline inspection but is not loaded. */
hu_eval_gate_verdict_t verdict;
hu_error_t ge = hu_eval_gate_evaluate(ctx->eval_gate, report.adapter_path, &verdict);
if (ge == HU_OK && !verdict.passed) {
    hu_log_warn("lora-runner", NULL,
                "adapter %s failed eval gate: %s — not promoting",
                aid, verdict.rationale);
    return HU_OK;   /* not a hard error — gate is advisory */
}
```

The `eval_gate` field is added to `hu_lora_runner_ctx_t` in `include/human/agent/lora_runner.h` (Phase 5 may have already done this; verify with `rg`).

- [ ] **Step 3: Add the test-only `hu_e2e_closed_loop_run` helper**

Inside the existing `#ifdef HU_IS_TEST` block of `src/agent/lora_training_runner.c` (or create the block if it doesn't exist), add the function described in D4. Read `include/human/agent/lora_runner.h` to understand the existing `hu_lora_runner_ctx_t` shape; the new test helper does NOT use that struct (it has its own input/output structs declared in a new `tests/include/hu_e2e_closed_loop.h`).

```c
#ifdef HU_IS_TEST
#include "hu_e2e_closed_loop.h"   /* test-only header in tests/include/ */

/* Cross-copy version pin — see D5 and ME4 below. If you bump this,
 * bump the matching constant in src/ml/cli_demo.c::cli_demo_run_closed_loop
 * in the same commit. */
#define HU_E2E_LOOP_IMPL_VERSION 1
_Static_assert(HU_E2E_LOOP_IMPL_VERSION == 1,
               "If you bumped HU_E2E_LOOP_IMPL_VERSION in one copy "
               "(lora_training_runner.c or cli_demo.c) you MUST bump "
               "the matching constant in the other copy. See plan D5 / ME4.");

void hu_e2e_closed_loop_output_free(hu_allocator_t *alloc,
                                    hu_e2e_closed_loop_output_t *out) {
    if (!alloc || !out) return;
    if (out->before_response) {
        alloc->free(alloc->ctx, out->before_response,
                    out->before_response_len + 1);
        out->before_response = NULL;
        out->before_response_len = 0;
    }
    if (out->after_response) {
        alloc->free(alloc->ctx, out->after_response,
                    out->after_response_len + 1);
        out->after_response = NULL;
        out->after_response_len = 0;
    }
}

hu_error_t hu_e2e_closed_loop_run(const hu_e2e_closed_loop_input_t *in,
                                  hu_allocator_t *alloc,
                                  hu_e2e_closed_loop_output_t *out) {
    if (!in || !alloc || !out || !in->provider || !in->provider->vtable
        || !in->trainer || !in->trainer->vtable || !in->collector
        || !in->reaction_events || !in->prompt || !in->adapter_out_path
        || !in->adapter_id || !in->system_prompt || !in->user_message
        || !in->model)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    int64_t t0 = hu_monotonic_ms();
    hu_error_t err = HU_OK;
    hu_dpo_export_t export_data = {0};

    /* 1. Pre-swap chat — real provider signature (include/human/provider.h:207-210):
     *   chat_with_system(ctx, alloc, system_prompt, system_prompt_len,
     *                    message, message_len, model, model_len,
     *                    temperature, char **out, size_t *out_len)
     * Provider allocates the response on `alloc`; caller (us) owns + frees. */
    err = in->provider->vtable->chat_with_system(
        in->provider->ctx, alloc,
        in->system_prompt, in->system_prompt_len,
        in->user_message, in->user_message_len,
        in->model, in->model_len,
        in->temperature,
        &out->before_response, &out->before_response_len);
    if (err != HU_OK) goto cleanup;

    /* 2. Replay reaction events into the preference store.
     * hu_reaction_handler_handle_event takes ONE argument (the event);
     * the target collector is a global set via _set_collector
     * (include/human/agent/reaction_handler.h:41-43). Without the
     * setter call, every handle_event returns HU_ERR_NOT_SUPPORTED. */
    hu_reaction_handler_set_collector(in->collector);
    for (size_t i = 0; i < in->reaction_event_count; i++) {
        err = hu_reaction_handler_handle_event(&in->reaction_events[i]);
        if (err != HU_OK && err != HU_ERR_NOT_FOUND) {
            hu_reaction_handler_set_collector(NULL);
            goto cleanup;
        }
        err = HU_OK;  /* HU_ERR_NOT_FOUND = silent-drop policy from Phase 2 R4 */
    }
    hu_reaction_handler_set_collector(NULL);

    /* 3. Verify floor */
    size_t pair_count = 0;
    (void)hu_dpo_pair_count(in->collector, &pair_count);
    out->pairs_consumed = (double)pair_count;
    if (pair_count < 50) { err = HU_ERR_INVALID_STATE; goto cleanup; }  /* spec §11 row 5 */

    /* 4. Export collected pairs + train.
     * hu_dpo_export populates a heap-owned hu_preference_pair_t array
     * (include/human/ml/dpo.h:69-74); we free via hu_dpo_export_free.
     * The trainer's step signature is
     *   step(ctx, alloc, pairs, n_pairs, metrics) — no hu_rl_batch_t.
     * See include/human/ml/rl_trainer.h:55-58. */
    err = hu_dpo_export(in->collector, alloc, &export_data);
    if (err != HU_OK) goto cleanup;

    hu_rl_trainer_metrics_t metrics = {0};
    err = in->trainer->vtable->step(
        in->trainer->ctx, alloc,
        export_data.pairs, export_data.count,
        &metrics);
    if (err != HU_OK) goto cleanup;

    /* 5. Save adapter — save_adapter signature is (ctx, alloc, path)
     * per include/human/ml/rl_trainer.h:59. */
    err = in->trainer->vtable->save_adapter(
        in->trainer->ctx, alloc,
        in->adapter_out_path);
    if (err != HU_OK) goto cleanup;
    snprintf(out->adapter_path, sizeof(out->adapter_path), "%s",
             in->adapter_out_path);

    /* 6. Hot-swap. Phase 1's llamacpp_load_adapter
     * (src/providers/llamacpp.c:454-455) already calls llama_memory_clear
     * + hu_llamacpp_kvcache_reset internally; no extra C-side action
     * needed. The hu_provider_vtable_t has NO `kv_cache_clear` field
     * (verify against include/human/provider.h:205-257). */
    err = hu_provider_load_adapter(
        in->provider, alloc, in->adapter_out_path,
        strlen(in->adapter_out_path), in->adapter_id, strlen(in->adapter_id));
    if (err != HU_OK) goto cleanup;

    /* 7. Post-swap chat — same prompt, same signature as step 1. */
    err = in->provider->vtable->chat_with_system(
        in->provider->ctx, alloc,
        in->system_prompt, in->system_prompt_len,
        in->user_message, in->user_message_len,
        in->model, in->model_len,
        in->temperature,
        &out->after_response, &out->after_response_len);
    if (err != HU_OK) goto cleanup;

    /* 8/9. Difference + timing */
    out->responses_differ =
        out->before_response && out->after_response &&
        ((out->before_response_len != out->after_response_len) ||
         memcmp(out->before_response, out->after_response,
                out->before_response_len) != 0);
    out->elapsed_ms = hu_monotonic_ms() - t0;

cleanup:
    hu_dpo_export_free(alloc, &export_data);
    if (err != HU_OK) {
        /* On failure, release whatever was already allocated so the
         * caller may safely re-use `out`. The _free is idempotent. */
        hu_e2e_closed_loop_output_free(alloc, out);
    }
    return err;
}
#endif
```

`hu_monotonic_ms()` is the existing helper from `src/core/time.c`. Response buffers are heap-allocated by the provider's `chat_with_system` per its C signature (`char **out` + `size_t *out_len`); ownership transfers to the output struct and is released by `hu_e2e_closed_loop_output_free`. The demo path in `cli_demo.c` (D5 duplicate) uses the same shape so the two copies remain structurally identical.

- [ ] **Step 4: Create the test-only header**

```bash
mkdir -p tests/include
```

Write `tests/include/hu_e2e_closed_loop.h` with the two struct definitions and the function declaration as shown in D4 above. Header guard `HU_E2E_CLOSED_LOOP_H`.

- [ ] **Step 5: Commit**

```bash
git add src/agent/lora_training_runner.c tests/include/hu_e2e_closed_loop.h
git commit -m "feat(agent): add hu_e2e_closed_loop_run test seam (Phase 6 D4)

Synchronous projection of the closed-loop algorithm for the
deterministic E2E test in tests/test_e2e_rl_loop.c. Gated by
HU_IS_TEST so release-binary delta is exactly zero. Reuses every
production primitive (chat_with_system, hu_reaction_handler_set_collector
+ hu_reaction_handler_handle_event, hu_dpo_export, trainer.step,
save_adapter, hu_provider_load_adapter) without touching the
production async runner. Phase 1's load_adapter already clears the
KV cache internally; no kv_cache_clear field exists on the provider
vtable (verified against include/human/provider.h).

Refs spec §4.7, plan C1–C8 + D4 + D5 + F5."
```

---

### Task 4: Write the failing E2E closed-loop test (deterministic DPO path)

**Files:**
- Create: `tests/test_e2e_rl_loop.c`
- Modify: `tests/test_main.c` (declare + call `run_e2e_closed_loop_tests`)
- Modify: `CMakeLists.txt` (already covered in the CMake entries section above; if not yet committed, append now)

- [ ] **Step 1: Write the failing test (header + first test function)**

```c
/* tests/test_e2e_rl_loop.c
 *
 * Phase 6 — the canonical deterministic E2E closed-loop proof.
 *
 * THIS TEST PINS THE WIRING. It does NOT pin the QUALITY of the
 * resulting adapter on a real model. For real-Gemma proof, see
 * scripts/demo-rl-loop.sh and docs/demos/rl-loop-demo.md.
 *
 * Determinism contract: srand(42) at suite start; every trainer is
 * configured with seed=42; every fixture file is committed (no live
 * I/O); every reaction event has a fixed ts_ms. Three runs of this
 * suite produce byte-identical adapter SHA-256 (pinned by
 * test_e2e_closed_loop_is_deterministic).
 *
 * Runtime budget: ≤ 30 seconds in CI under ASan (pinned by
 * test_e2e_closed_loop_completes_within_30s_under_asan).
 */

#include "test_framework.h"
#include "hu_e2e_closed_loop.h"
#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/personal_model.h"
#include "human/ml/dpo.h"
#include "human/ml/dpo_real.h"
#include "human/ml/rl_trainer.h"
#include "human/provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Forward declarations of helpers defined later in this file. */
static void                          load_persona_seed(hu_communication_style_t *out_style,
                                                       const char ***out_held_out_prompts,
                                                       size_t *out_n_prompts);
static void                          load_reaction_signals(hu_reaction_event_t **out_events,
                                                           size_t *out_n);
static hu_provider_t *               make_huml_mock_provider(hu_allocator_t *alloc);
static hu_dpo_collector_t *          make_in_memory_dpo_collector(hu_allocator_t *alloc);
static hu_rl_trainer_t *             make_dpo_huml_trainer(hu_allocator_t *alloc);
static void                          set_up_env(void);
static void                          tear_down_env(void);

static void test_e2e_closed_loop_dpo_shows_measurable_response_change(void) {
    set_up_env();

    hu_allocator_t alloc = hu_system_allocator();
    hu_communication_style_t style = {0};
    const char **held_out = NULL;
    size_t n_held_out = 0;
    load_persona_seed(&style, &held_out, &n_held_out);
    HU_ASSERT_EQ(n_held_out, 100);

    hu_reaction_event_t *events = NULL;
    size_t n_events = 0;
    load_reaction_signals(&events, &n_events);
    HU_ASSERT_EQ(n_events, 50);

    hu_provider_t *provider = make_huml_mock_provider(&alloc);
    hu_dpo_collector_t *collector = make_in_memory_dpo_collector(&alloc);
    hu_rl_trainer_t *trainer = make_dpo_huml_trainer(&alloc);
    HU_ASSERT_NOT_NULL(provider);
    HU_ASSERT_NOT_NULL(collector);
    HU_ASSERT_NOT_NULL(trainer);

    /* LO3 fix: adapter path is an absolute path under the CMake-anchored
     * tests/_tmp/ directory so the test is CWD-independent. The literal
     * below is resolved by helper hu_e2e_tmp_path() at call time. */
    char adapter_path[1024];
    hu_e2e_tmp_path(adapter_path, sizeof(adapter_path),
                    "proofs/dpo-step-0001/lora.bin");
    hu_e2e_closed_loop_input_t in = {
        .provider = provider, .trainer = trainer, .collector = collector,
        .reaction_events = events, .reaction_event_count = n_events,
        .system_prompt     = "respond like the persona seed.",
        .system_prompt_len = strlen("respond like the persona seed."),
        .user_message      = "what should i do first?",
        .user_message_len  = strlen("what should i do first?"),
        .model             = "huml-toy-gpt",
        .model_len         = strlen("huml-toy-gpt"),
        .temperature       = 0.0,
        .adapter_out_path  = adapter_path,
        .adapter_id        = "e2e-dpo-001",
    };
    hu_e2e_closed_loop_output_t out = {0};
    HU_ASSERT_EQ(hu_e2e_closed_loop_run(&in, &alloc, &out), HU_OK);

    /* The headline assertion: post-swap response differs from pre-swap. */
    HU_ASSERT_TRUE(out.responses_differ);
    HU_ASSERT_GT(out.before_response_len, 0);
    HU_ASSERT_GT(out.after_response_len, 0);

    /* Pair count matches the synthetic stream. */
    HU_ASSERT_EQ((size_t)out.pairs_consumed, n_events);

    /* Adapter file exists and is non-empty. */
    struct stat st;
    HU_ASSERT_EQ(stat(out.adapter_path, &st), 0);
    HU_ASSERT_GT(st.st_size, 0);

    hu_e2e_closed_loop_output_free(&alloc, &out);
    free((void *)held_out);
    free(events);
    /* Provider/trainer/collector ownership freed by their respective
     * deinit calls inside the make_* helpers' cleanup blocks. */
    tear_down_env();
}

void run_e2e_closed_loop_tests(void) {
    HU_TEST_SUITE("E2E-closed-loop");
    HU_RUN_TEST(test_e2e_closed_loop_dpo_shows_measurable_response_change);
    /* Other tests added in Tasks 5–11 of this plan. */
}
```

> **CRITICAL:** `HU_TEST_SUITE("E2E-closed-loop")` MUST be inside the body of `run_e2e_closed_loop_tests`, NOT at file scope. The `HU_TEST_SUITE` macro mutates the `hu__suite_active` global which gates `HU_RUN_TEST`; placing it at file scope would either fail to compile (it expands to a `do { ... } while(0)` block, which is not valid at file scope in C11) or, if rewritten to compile, would not gate per-suite filtering correctly. Every Phase 5 test plan made this mistake before being corrected; Phase 6 honors the convention from the start.

- [ ] **Step 2: Wire the test into `tests/test_main.c`**

```bash
rg -n 'run_e2e_' tests/test_main.c   # find the existing E2E test runner section
```

Append a forward-decl `void run_e2e_closed_loop_tests(void);` and a call in the appropriate runner section. READ first; APPEND only; do NOT rewrite the runner block.

- [ ] **Step 3: Run the test and verify it fails (because helpers don't exist yet)**

```bash
cmake --build build-dev -j
./build-dev/human_tests --suite="E2E-closed-loop" 2>&1 | tee /tmp/e2e-fail.log
grep -E 'FAIL|undefined' /tmp/e2e-fail.log   # expect undefined references on link
```

- [ ] **Step 4: Implement the helpers (`load_persona_seed`, `load_reaction_signals`, `make_huml_mock_provider`, `make_in_memory_dpo_collector`, `make_dpo_huml_trainer`, `set_up_env`, `tear_down_env`)**

Each helper is short. Their key contracts:

- `set_up_env`: `srand(42)` (initial seed; per HI3, callers reseed at every deterministic call site); `setenv("HU_E2E_FIXED_TIMESTAMP", "1715472000", 1)` (R8 + ME3 mitigation — epoch seconds for 2024-05-12T00:00:00Z, parsed by `strtoll` in `cli_demo.c::hu_e2e_now`); `mkdir -p tests/_tmp/proofs/`.
- `tear_down_env`: removes the temp dir tree; unsets the env var.
- `load_persona_seed`: opens `tests/fixtures/e2e_persona_seed.json` (relative path; CMake's `configure_file` ensures this works from `build-dev/`), parses it via the existing `hu_json_*` helpers (or `cJSON` if linked), populates `hu_communication_style_t` via `hu_communication_style_fingerprint_from_samples`, mallocs the `held_out_prompts[]` array.
- `load_reaction_signals`: opens `tests/fixtures/e2e_reaction_signals.json`, parses each event into `hu_reaction_event_t`, returns the array via out-param. Caller frees.
- `make_huml_mock_provider`: instantiates a `hu_provider_t` whose `vtable->chat_with_system` reads from a shared `hu_gpt_t` (the one the trainer mutates), runs greedy decode, and emits the resulting tokens as text. The mock provider's `chat_with_system` is a tiny shim — ~30 LOC. It exposes `vtable->load_adapter` as a no-op (the trainer mutates the GPT directly; no adapter-file path needed for the mock — so the mock's `load_adapter` accepts any path and returns `HU_OK`, and the test's "the post-swap response differs" assertion is true because the underlying GPT changed in-place during `trainer->step`).
- `make_in_memory_dpo_collector`: `sqlite3_open(":memory:", &db)` + `hu_dpo_init_tables(&col)`.
- `make_dpo_huml_trainer`: `hu_rl_trainer_create_dpo(HU_DPO_BACKEND_HUML, ...)` with `seed=42`, `learning_rate=1e-2`, `beta=0.1`, points at the same `hu_gpt_t` the mock provider reads.

Re-run; the test should now PASS.

- [ ] **Step 5: Add the `responses_differ` assertion edge case**

If the toy GPT is too small to differ on this prompt with this signal, the test will fail on `HU_ASSERT_TRUE(out.responses_differ)`. Mitigations (apply in order, stop at the first that works):

1. Increase `learning_rate` from `1e-2` to `1e-1` (toy GPT can take it).
2. Increase the number of `trainer->step` invocations from 1 to 5 (multiple gradient steps over the same batch).
3. Increase the toy GPT vocab from 32 to 128 (more room for divergence).

Document the chosen mitigation in a comment above `make_dpo_huml_trainer`.

- [ ] **Step 6: Add the 30-second wall-clock assertion (R7 + E5)**

```c
static void test_e2e_closed_loop_completes_within_30s_under_asan(void) {
    set_up_env();
    /* ... same setup as test_e2e_closed_loop_dpo_shows_measurable_response_change;
     *     `alloc` is the hu_allocator_t the test owns. */
    int64_t t0 = hu_monotonic_ms();
    HU_ASSERT_EQ(hu_e2e_closed_loop_run(&in, &alloc, &out), HU_OK);
    int64_t elapsed = hu_monotonic_ms() - t0;
    HU_ASSERT_LE(elapsed, 30000);   /* 30 s budget per spec §6 + plan R7 */
    hu_e2e_closed_loop_output_free(&alloc, &out);
    /* ... cleanup ... */
}
```

If this fires on CI, follow R7's mitigation ladder (reduce fixture sizes 50→25 / 100→50; lift ceiling to 60s with TODO).

- [ ] **Step 7: Commit**

```bash
git add tests/test_e2e_rl_loop.c tests/test_main.c
git commit -m "test(e2e): pin Phase 6 closed-loop wiring with deterministic DPO test

Adds tests/test_e2e_rl_loop.c with the canonical wiring proof:
mock provider + HUML toy GPT + DPO trainer + 50 synthetic reactions
+ 100 held-out prompts. After 1 trainer step + 1 hot-swap, the
post-swap response on the same prompt is byte-different from the
pre-swap response. Wall-clock budget pinned at ≤ 30s under ASan.

Refs spec §4.7, §6 Tier 4, plan D1 + D4 + R7 + E5."
```

---

### Task 5: Add the determinism pin (D10)

**Files:**
- Modify: `tests/test_e2e_rl_loop.c`

- [ ] **Step 1: Write the failing test**

```c
/* ME2: use the existing `hu_sha256` one-shot helper from
 * include/human/crypto.h. OpenSSL is NOT linked (zero-dependency
 * constraint — AGENTS.md §1: "zero dependencies beyond libc, optional
 * SQLite and libcurl"). The real signature is:
 *   void hu_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
 * (one-shot, no streaming context). Verified via:
 *   rg -n 'hu_sha256' include/human/crypto.h  → line 13.
 * Confirmed used elsewhere in-tree, e.g. src/update.c:257. */
#include "human/crypto.h"

static void compute_file_sha256(hu_allocator_t *alloc,
                                const char *path,
                                uint8_t out[32]) {
    FILE *fp = fopen(path, "rb");
    HU_ASSERT_NOT_NULL(fp);
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    HU_ASSERT_GT(fsize, 0);
    uint8_t *buf = alloc->malloc(alloc->ctx, (size_t)fsize);
    HU_ASSERT_NOT_NULL(buf);
    HU_ASSERT_EQ(fread(buf, 1, (size_t)fsize, fp), (size_t)fsize);
    fclose(fp);
    hu_sha256(buf, (size_t)fsize, out);
    alloc->free(alloc->ctx, buf, (size_t)fsize);
}

/* LO1 fix: split the 3-run determinism into two separate test
 * functions so each one isolates a single comparison (run1 vs run2,
 * run1 vs run3). HI3 fix: srand(42) is reseeded IMMEDIATELY before
 * each closed-loop run AND before each hu_gpt_t init — set_up_env's
 * seed contaminates subsequent bootstrap CI sampling, so determinism
 * must be per-call, not per-suite. */
static void run_one_closed_loop_and_hash(hu_allocator_t *alloc,
                                         int run_idx,
                                         uint8_t out_hash[32]) {
    srand(42);  /* HI3: per-call, not per-suite */
    /* ... full setup with a fresh hu_gpt_t seeded 42 each time ... */
    char path[1024];
    hu_e2e_tmp_path(path, sizeof(path),
                    "proofs/det-run-%d/lora.bin");  /* %d substituted via snprintf */
    snprintf(path, sizeof(path),
             "%s/proofs/det-run-%d/lora.bin",
             hu_e2e_tmp_root(), run_idx);
    hu_e2e_closed_loop_input_t in = { /* … same fields as DPO test, with path = path … */ };
    hu_e2e_closed_loop_output_t out = {0};
    HU_ASSERT_EQ(hu_e2e_closed_loop_run(&in, alloc, &out), HU_OK);
    compute_file_sha256(alloc, path, out_hash);
    hu_e2e_closed_loop_output_free(alloc, &out);
    /* … cleanup gpt/trainer/collector/provider … */
}

static void test_e2e_closed_loop_deterministic_run1_vs_run2(void) {
    set_up_env();
    hu_allocator_t alloc = hu_system_allocator();
    uint8_t h1[32], h2[32];
    run_one_closed_loop_and_hash(&alloc, 1, h1);
    run_one_closed_loop_and_hash(&alloc, 2, h2);
    HU_ASSERT_EQ(memcmp(h1, h2, 32), 0);
    tear_down_env();
}

static void test_e2e_closed_loop_deterministic_run1_vs_run3(void) {
    set_up_env();
    hu_allocator_t alloc = hu_system_allocator();
    uint8_t h1[32], h3[32];
    run_one_closed_loop_and_hash(&alloc, 1, h1);
    run_one_closed_loop_and_hash(&alloc, 3, h3);
    HU_ASSERT_EQ(memcmp(h1, h3, 32), 0);
    tear_down_env();
}
```

> **ME2 fix:** `<openssl/sha.h>` is NOT available — AGENTS.md §1 mandates "zero dependencies beyond libc, optional SQLite and libcurl." The skeleton above uses the existing one-shot `hu_sha256` helper from `include/human/crypto.h` line 13. This is the canonical in-tree SHA-256 (used by `src/update.c:257`, `src/security/audit.c`, `src/security/pairing.c`, `src/crypto/dispatch.c`). If you reach for OpenSSL during implementation, stop — you've violated the zero-dep constraint.

- [ ] **Step 2: Add to runner**

```c
HU_RUN_TEST(test_e2e_closed_loop_deterministic_run1_vs_run2);
HU_RUN_TEST(test_e2e_closed_loop_deterministic_run1_vs_run3);
```

- [ ] **Step 3: Run and fix any nondeterminism**

If the assertion fires, the failure points to a hidden source of nondeterminism. The most common are:

- `time(NULL)` in the trainer's adapter manifest writer → use `HU_E2E_FIXED_TIMESTAMP`.
- `getpid()` in any path → mock via `HU_IS_TEST` env.
- Hash-table iteration order (uthash, or whatever the codebase uses) → sort before serialize.
- `posix_spawn` exit-code race → not present in HUML path.

Iterate until 3 runs produce the same SHA. If determinism is fundamentally not achievable on the toy path (e.g. a known float-order issue in `muon_adamw`), document the limitation and lift the assertion to "h0 and h1 differ in ≤ 4 bytes" (last-resort).

- [ ] **Step 4: Commit**

```bash
git commit -m "test(e2e): pin Phase 6 closed-loop adapter byte-determinism

3 runs of the deterministic E2E test produce byte-identical
adapter SHA-256. Failure of this assertion indicates a hidden
nondeterminism source — must be fixed before Phase 6 ships.

Refs plan D10."
```

---

### Task 6: Add the F1–F6 common-failure-mode regression pins

**Files:**
- Modify: `tests/test_e2e_rl_loop.c`

- [ ] **Step 1: Write `test_e2e_closed_loop_provider_after_response_differs_from_before` (F1)**

A tighter pin than Task 4 step 1's primary test — explicit `memcmp` assertion with helpful failure message. Same setup; the assertion is:

```c
if (out.before_response_len == out.after_response_len &&
    memcmp(out.before_response, out.after_response, out.before_response_len) == 0) {
    HU_FAIL("F1 fired: provider returned the same response after adapter swap. "
            "before='%.*s' after='%.*s'",
            (int)out.before_response_len, out.before_response,
            (int)out.after_response_len, out.after_response);
}
```

- [ ] **Step 2: Write `test_e2e_closed_loop_all_synthetic_reactions_become_dpo_pairs` (F2)**

```c
size_t n_pairs = 0;
HU_ASSERT_EQ(hu_dpo_pair_count(collector, &n_pairs), HU_OK);
HU_ASSERT_EQ(n_pairs, 50);

/* Plus check each row's source field. HI2 fix: the reaction handler
 * does NOT write the fixture's "source_tag" verbatim — it writes one
 * of "imessage_tapback", "slack_reactji", or the raw channel_id
 * (e.g. "test-imessage") depending on the event's channel_kind. The
 * fixture's channel_kind is "synthetic" and channel_id is
 * "test-imessage", so the handler will write "test-imessage". A
 * literal `strcmp(..., "e2e_synthetic")` would always fail; we relax
 * to a substring match that accepts every channel-derived form. */
hu_dpo_export_t export_data = {0};
HU_ASSERT_EQ(hu_dpo_export(collector, &alloc, &export_data), HU_OK);
HU_ASSERT_EQ(export_data.count, 50);
for (size_t i = 0; i < export_data.count; i++) {
    HU_ASSERT_TRUE(
        strstr(export_data.pairs[i].source, "imessage_tapback") != NULL ||
        strstr(export_data.pairs[i].source, "slack_reactji")    != NULL ||
        strstr(export_data.pairs[i].source, "test-imessage")    != NULL);
}
hu_dpo_export_free(&alloc, &export_data);
```

- [ ] **Step 3: Write `test_e2e_closed_loop_bootstrap_ci_lower_bound_is_positive` (F3)**

This requires actually running the closed loop, scoring all 100 held-out prompts pre and post, and computing a 95% bootstrap CI on the per-prompt deltas. Use 1000 resamples with `srand(42)` reseeded right before. The helper is ~80 LOC; isolate it into a static function `static double bootstrap_lower_95(const double *deltas, size_t n)` so other tests can reuse it.

> **HI3 fix:** `srand(42)` MUST be reseeded **immediately before** each call to `bootstrap_lower_95` (and immediately before each `hu_gpt_t` initialization in `make_*_trainer`). Seeding only once in `set_up_env()` contaminates the RNG: every test that uses `rand()` between setup and the bootstrap call shifts the resample sequence, so bootstrap CI samples differ on every run composition (run order, test selection, etc.). Determinism is per-call, not per-suite — the bootstrap helper itself does:
>
> ```c
> static double bootstrap_lower_95(const double *deltas, size_t n) {
>     srand(42);   /* HI3: reseed at every call site, not at suite start */
>     /* … 1000 resamples … */
> }
> ```

```c
srand(42);   /* HI3: reseed immediately before the call */
HU_ASSERT_GT(bootstrap_lower_95(per_prompt_deltas, 100), 0.0);
```

- [ ] **Step 4: Write `test_e2e_closed_loop_async_path_drains_pending_signals_after_reactions` (F6)**

This test exercises the ASYNC production path, NOT `hu_e2e_closed_loop_run`. It registers `hu_lora_training_runner` as a scheduler callback, emits the 50 reaction events, fires the scheduler once, and asserts the runner drained `n > 0` signals. Pinning that the production path also wires through.

- [ ] **Step 5: Write `test_e2e_closed_loop_evidence_manifest_records_pair_count` (E4)**

Calls a Task-9-built `write_evidence_dir` helper, then reads back `manifest.json`, asserts `preference_pairs_consumed == 50` and `reactions_emitted == 50`. (Phase 6's `cli_demo.c::write_evidence_dir` is the production code path; the test exercises it via a small wrapper.)

- [ ] **Step 7: Write `test_e2e_closed_loop_negative_rate_anomaly_recorded` (SEC1)**

Synthesizes a 35-negative / 15-positive reaction stream and asserts that:

1. `hu_e2e_closed_loop_run` either returns `HU_ERR_POLICY_VIOLATION` or completes but writes `anomaly: high_negative_rate` to the evidence dir's `anomaly_log.jsonl`.
2. The negative-rate gate fires at exactly the 30% threshold from SEC1 (not 25%, not 33%).
3. Under `--require-positive-delta` the demo CLI refuses to promote.

- [ ] **Step 8: Write `test_e2e_closed_loop_adapter_sha_mismatch_aborts_swap` (SEC2)**

After `save_adapter` writes `lora.bin` and BEFORE `load_adapter` runs, the test mutates one byte of the file via `fopen("rb+") + fseek + fputc + fclose`. The test then drives `cli_demo_run_closed_loop` (D5 release-path duplicate) and asserts the runner returns `HU_ERR_INTEGRITY` and that the evidence dir contains an `anomaly: adapter_sha_mismatch` entry. The deterministic test exercises this against the mock provider; the real-llama.cpp variant in Task 6 step 9 (gated) exercises it against the real provider.

- [ ] **Step 9: Write `test_llamacpp_lora_hotswap_midsession.c` (F5, separate file, gated)**

```c
/* tests/test_llamacpp_lora_hotswap_midsession.c
 * Gated by HU_HAVE_GEMMA_GGUF=1 + access(gguf_path, R_OK).
 */
#include "test_framework.h"
#include "human/providers/llamacpp.h"
#include <stdlib.h>
#include <unistd.h>

static void test_llamacpp_hotswap_midsession_does_not_corrupt_after_response(void) {
    HU_SKIP_IF(getenv("HU_HAVE_GEMMA_GGUF") == NULL,
               "HU_HAVE_GEMMA_GGUF unset");
    const char *gguf = getenv("HU_GEMMA_GGUF_PATH");
    HU_SKIP_IF(!gguf || access(gguf, R_OK) != 0,
               "Gemma GGUF not readable");
    HU_SKIP_IF(getenv("HU_HAVE_FIXTURE_LORA_ADAPTER") == NULL,
               "fixture LoRA adapter unavailable");
    /* ... create real llama.cpp provider, chat A, load fixture adapter,
     *     KV-clear, chat B, assert B is non-empty + UTF-8 + differs from A ... */
}

void run_llamacpp_hotswap_midsession_tests(void) {
    HU_TEST_SUITE("llamacpp-hotswap-midsession");
    HU_RUN_TEST(test_llamacpp_hotswap_midsession_does_not_corrupt_after_response);
}
```

Wire `run_llamacpp_hotswap_midsession_tests` into `tests/test_main.c`. The CMake guard for the test source is already covered by the `if(HU_ENABLE_LLAMACPP)` block from the CMake entries section.

- [ ] **Step 7: Commit**

```bash
git commit -m "test(e2e): add F1–F6 regression pins for Phase 6 failure modes

F1: provider after_response differs from before_response (mock provider)
F2: all 50 synthetic reactions surface as dpo_pairs rows with a
    handler-derived source (imessage_tapback / slack_reactji /
    channel_id substring per HI2)
F3: 95% bootstrap CI lower bound on per-prompt fidelity delta is > 0
    (srand reseeded immediately before each bootstrap call per HI3)
F4: documented as expected (demo nondeterminism, no test pin)
F5: tests/test_llamacpp_lora_hotswap_midsession.c (gated, real Gemma)
F6: async path via hu_lora_training_runner drains pending signals
E4: manifest records preference_pairs_consumed = reactions_emitted = 50
SEC1: 35-neg/15-pos stream fires high_negative_rate anomaly + blocks
      promotion under --require-positive-delta
SEC2: 1-byte adapter tamper between save_adapter and load_adapter
      causes HU_ERR_INTEGRITY + adapter_sha_mismatch anomaly entry

Refs plan F1–F6, E4, SEC1, SEC2."
```

---

### Task 7: Add the persona-fidelity ≥ +0.05 delta E2E gate (DoD item 6)

**Files:**
- Modify: `tests/test_e2e_rl_loop.c`

- [ ] **Step 1: Write `test_e2e_closed_loop_persona_fidelity_delta_meets_threshold`**

```c
static void test_e2e_closed_loop_persona_fidelity_delta_meets_threshold(void) {
    set_up_env();
    /* ... same setup; after the closed loop, score all 100 held_out
     *     prompts pre and post via hu_communication_style_fidelity_score ... */

    double before_scores[100], after_scores[100];
    /* ... fill arrays by calling provider.chat_with_system on each prompt
     *     once with the original GPT, once with the mutated-by-trainer GPT ... */

    double mean_before = 0, mean_after = 0;
    for (size_t i = 0; i < 100; i++) {
        mean_before += before_scores[i];
        mean_after  += after_scores[i];
    }
    mean_before /= 100.0;
    mean_after  /= 100.0;
    double delta = mean_after - mean_before;
    if (delta < 0.05) {
        HU_FAIL("DoD item 6 fired: persona-fidelity delta is %.4f, "
                "below threshold of 0.05. before_mean=%.4f after_mean=%.4f. "
                "If this is a real adapter-quality regression, fix the trainer; "
                "if the toy GPT genuinely cannot move 0.05 on 50 reactions, "
                "raise the trainer's learning_rate or step count and re-pin.",
                delta, mean_before, mean_after);
    }
    /* Plus the bootstrap-CI lower bound check from F3. */
    double per_prompt_deltas[100];
    for (size_t i = 0; i < 100; i++)
        per_prompt_deltas[i] = after_scores[i] - before_scores[i];
    double lower_ci = bootstrap_lower_95(per_prompt_deltas, 100);
    HU_ASSERT_GT((long long)(lower_ci * 1000.0), 0LL);   /* > 0 in millipoints */
    tear_down_env();
}
```

- [ ] **Step 2: Run and tune**

This is the test most likely to require fixture tuning. If 50 reactions can't move the toy GPT 0.05 on 100 held-out prompts:

1. Make the persona seed MORE extreme (D7's lever — e.g. `lowercase_ratio_target = 0.95` instead of `0.85`).
2. Bump trainer step count from 1 to 5.
3. As a last resort, lower the threshold to 0.03 with a TODO documenting the gap between the test's bar and the spec's `≥ +0.05` (the spec bar is for real Gemma; the test bar can legitimately be lower if the toy GPT is too noisy).

Document whichever tuning was applied in the test's leading comment.

- [ ] **Step 3: Commit**

```bash
git commit -m "test(e2e): assert ≥0.05 absolute persona-fidelity delta after closed loop

DoD item 6: ≥5% persona-fidelity delta on the same held-out 100 prompts
pre and post adapter swap, with 95% bootstrap CI lower bound > 0.

Refs spec §1, §11 row 4, plan D6 + DoD."
```

---

### Task 8: Add the KTO + GRPO E2E variants (skipped if Phase 3/4 trainers absent)

**Files:**
- Modify: `tests/test_e2e_rl_loop.c`

- [ ] **Step 1: Add `test_e2e_closed_loop_kto_shows_measurable_response_change`**

Same shape as the DPO test but with `hu_rl_trainer_create_kto`. The reaction stream is reused as-is (KTO interprets one-sided pairs naturally per Phase 3's plan D1; the 50-event mixed positive/negative stream maps to 50 KTO signals).

```c
HU_SKIP_IF(/* Phase 3 not present in build */, "KTO trainer requires HU_ENABLE_RL_FULL");
```

- [ ] **Step 2: Add `test_e2e_closed_loop_grpo_shows_measurable_response_change`**

Same shape but uses `hu_rl_trainer_create_grpo` with `rollouts=2` (small to keep CI runtime down — production uses 4). GRPO needs a reward function; the test instantiates a stub RM that returns `+1` for any response containing a vocab token from the persona's `samples[]` and `0` otherwise (deterministic, no real RM training).

> **ME1 fix:** `hu_rl_trainer_create_grpo` is not declared in any header outside the `HU_ENABLE_GRPO` compile branch (Phase 4 ships it; default `dev` preset does not). The test must compile in BOTH branches — declared `extern` (or via the header behind `#ifdef HU_ENABLE_GRPO`) — but only EXECUTE when GRPO is enabled. Use a build-time skip:
>
> ```c
> #ifdef HU_ENABLE_GRPO
>     /* … real GRPO test body … */
> #else
>     HU_SKIP_IF(1, "Phase 4 GRPO not enabled in this build (HU_ENABLE_GRPO undefined)");
> #endif
> ```
>
> The `HU_SKIP_IF(1, ...)` form is a runtime no-op when the macro is reached but never compiles in a reference to the missing symbol. Document in the test's leading comment that the body compiles in either branch but only executes under `cmake --preset rl_sota`.

```c
HU_SKIP_IF(!HU_ENABLE_GRPO, "Phase 4 GRPO not enabled in this build");
```

- [ ] **Step 3: Commit**

```bash
git commit -m "test(e2e): add KTO + GRPO closed-loop variants alongside DPO

Each variant uses the same fixture, same provider, same mock RM (where
needed) — only the trainer factory differs. All three variants
(test_e2e_closed_loop_dpo|kto|grpo_shows_measurable_response_change)
share the helper infrastructure. KTO/GRPO branches HU_SKIP_IF when
Phase 3/4 are not compiled in.

Refs spec §4.7, plan D1."
```

---

### Task 9: Implement `src/ml/cli_demo.c` + the `human demo` command in `src/main.c`

**Files:**
- Create: `src/ml/cli_demo.c`
- Create: `include/human/ml/cli_demo.h`
- Modify: `src/main.c` (add `cmd_demo` + register in `commands[]`)

- [ ] **Step 1: Write `include/human/ml/cli_demo.h`**

```c
#ifndef HU_ML_CLI_DEMO_H
#define HU_ML_CLI_DEMO_H

#include "human/core/allocator.h"
#include "human/core/error.h"

/* hu_ml_cli_demo_rl_closed_loop — entrypoint for `human demo rl-closed-loop`.
 * argc/argv are the post-subcommand args (i.e. caller already stripped
 * "demo" and "rl-closed-loop"). Exit codes:
 *   0 = win-condition met (delta ≥ +0.05 absolute points + lower-95-CI > 0)
 *   2 = win-condition missed (evidence dir still written; manual review)
 *   3 = harness error (provider/trainer creation failed; nothing trained)
 */
hu_error_t hu_ml_cli_demo_rl_closed_loop(int argc, const char **argv,
                                         hu_allocator_t *alloc);

#endif
```

- [ ] **Step 2: Write `src/ml/cli_demo.c` skeleton**

```c
#include "human/ml/cli_demo.h"
#include "human/agent/reaction_handler.h"
#include "human/eval/eval_gate.h"
#include "human/memory/personal_model.h"
#include "human/ml/dpo.h"
#include "human/ml/dpo_real.h"
#include "human/ml/grpo.h"
#include "human/ml/kto.h"
#include "human/ml/reward_model.h"
#include "human/ml/rl_trainer.h"
#include "human/provider.h"
#include "human/providers/factory.h"
#include "human/providers/llamacpp.h"
#include "human/persona.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct demo_args {
    const char *persona;          /* default "seth" */
    const char *method;           /* default "dpo" */
    const char *backend;          /* default "mlx" on Apple, "huml" elsewhere */
    int         reaction_count;   /* default 200 */
    const char *prompt;           /* default "what should I do first?" */
    const char *out_dir;          /* default ~/.human/proofs/<dated-id> */
    bool        require_positive_delta;
} demo_args_t;

static hu_error_t parse_args(int argc, const char **argv, demo_args_t *out);
static hu_error_t load_persona(hu_allocator_t *alloc, const char *name,
                               hu_persona_t *out);
static hu_error_t synthesize_reactions(int n, hu_reaction_event_t **out,
                                       size_t *out_n);
static hu_error_t run_closed_loop(/* ... */);
static hu_error_t score_persona_fidelity(/* ... */);
static hu_error_t write_evidence_dir(const char *dir,
                                     const struct closed_loop_run *run);

hu_error_t hu_ml_cli_demo_rl_closed_loop(int argc, const char **argv,
                                         hu_allocator_t *alloc) {
    demo_args_t args = {0};
    hu_error_t e = parse_args(argc, argv, &args);
    if (e != HU_OK) return e;

    hu_persona_t persona;
    e = load_persona(alloc, args.persona, &persona);
    if (e != HU_OK) {
        fprintf(stderr, "demo: failed to load persona '%s': %d\n",
                args.persona, e);
        return 3;
    }

    /* ... build provider via Phase 1 llamacpp factory + load_persona ...
     * ... build trainer via Phase 2/3/4 factories per args.method ...
     * ... synthesize_reactions(args.reaction_count) ...
     * ... run_closed_loop(...) → struct closed_loop_run ...
     * ... score_persona_fidelity on 100 held-out prompts pre/post ...
     * ... write_evidence_dir(args.out_dir, &run) — emits 9 spec §8 files ...
     * ... if args.require_positive_delta && !run.delta_passed → return 2 ... */

    return 0;
}
```

The full implementation is ~400 LOC. Key invariants:

- `parse_args` accepts long options only (`--persona`, `--method`, etc.) — no short options. Same convention as `human ml lora-baseline`.
- `synthesize_reactions(n)` generates n synthetic events deterministically when `--reactions <jsonl>` is NOT supplied; supports `--reactions <jsonl>` to load from a file (mirrors Phase 2's preference-pair loader). **HI4 fix:** the production `human` binary does NOT compile in the `HU_IS_TEST`-gated `hu_reaction_handler_register_assistant_message_for_test` seam, so without a release-build registration API every synthetic reaction will silently drop in the lookup store and `hu_dpo_pair_count` returns 0. The demo therefore **requires** the production registration API `hu_reaction_handler_register_assistant_message_for_production(channel, thread, msg_ref, prompt, response)` (no `_for_test` suffix; compiled into release builds). The demo calls it for each `(prompt, response)` tuple before emitting reactions. **Forward dependency:** this API is the Phase 5 H8 fix in `docs/plans/2026-05-11-rl-loop-phase-5-eval-competitive.md`. If Phase 5 ships without H8 applied, Phase 6 Task 0 step 4b (new) catches the missing symbol via `rg -n 'hu_reaction_handler_register_assistant_message_for_production' include/human/agent/reaction_handler.h` and BLOCKS Phase 6 until Phase 5 adds it. Phase 6 MUST NOT work around this by leaking the `_for_test` API into release.
- `write_evidence_dir(dir, &run)` emits all 9 spec §8 files in order (`manifest.json`, `training_curves.json`, `eval_before.json`, `eval_after.json`, `eval_delta.json`, `delta_responses.md`, `gate_decision.json`, `adversarial_review.md`, `reproduce.sh`). Each writer is a separate static function for testability.

  > **ME3 fix:** every wall-clock read inside `write_evidence_dir` MUST honor `HU_E2E_FIXED_TIMESTAMP` so the determinism test's adapter/manifest SHAs are reproducible. The pattern (applied at every `time(NULL)` call site in `cli_demo.c`):
  >
  > ```c
  > static time_t hu_e2e_now(void) {
  >     const char *fixed_ts = getenv("HU_E2E_FIXED_TIMESTAMP");
  >     if (fixed_ts && *fixed_ts) return (time_t)strtoll(fixed_ts, NULL, 10);
  >     return time(NULL);
  > }
  > /* … */
  > char created_at[32];
  > time_t t = hu_e2e_now();
  > strftime(created_at, sizeof(created_at), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
  > ```
  >
  > **Required call sites** (audit at write-time with `rg -n 'time\(NULL\)|gettimeofday|clock_gettime' src/ml/cli_demo.c`):
  >
  > 1. `manifest.json`'s `created_at` field.
  > 2. The evidence-dir name suffix (`~/.human/proofs/%Y-%m-%d-…`) — the bash demo formats this in `OUT_DIR=...`; the C path constructs it via `strftime` when `--out` is omitted.
  > 3. Any log timestamp embedded in `training_curves.json`, `eval_before.json`, `eval_after.json`, `eval_delta.json`, or `gate_decision.json`.
  > 4. The `reproduce.sh` shebang-comment header line "Generated <timestamp> by Phase 6".
  >
  > The format the env var carries is **epoch seconds** (e.g. `HU_E2E_FIXED_TIMESTAMP=1715472000`), not ISO-8601 — `strtoll` is the parser. The R8 fix in the risk register table writes the ISO-8601 string `"2026-05-12T00:00:00Z"` which is wrong; treat that table entry as illustrative and use the epoch-seconds form everywhere in the implementation.
- Exit code mapping (per spec demo-exit-code convention: 0 = pass, 2 = soft fail, 3 = hard fail): `0` if `delta ≥ 0.05` AND `lower_95_ci > 0`; `2` if delta is computed but doesn't meet threshold (still write evidence dir); `3` if harness error before delta could be computed (no evidence dir).

- [ ] **Step 3: Add `cmd_demo` to `src/main.c`**

```c
static hu_error_t cmd_demo(hu_allocator_t *alloc, int argc, char **argv);

/* ... inside the static array: */
{"demo", "Reproducible end-to-end demonstrations (RL closed loop)", cmd_demo},

/* ... function body, mirrors cmd_ml shape: */
static hu_error_t cmd_demo(hu_allocator_t *alloc, int argc, char **argv) {
    /* LO2 fix: explicit argc check FIRST, then dereference argv[2]. The
     * C `||` short-circuit already prevents UB here, but the explicit
     * order documents intent and matches the rest of cmd_ml's style. */
    if (argc < 3
        || strcmp(argv[2], "--help") == 0
        || strcmp(argv[2], "help") == 0) {
        printf("Usage: human demo <subcommand>\n\n"
               "Subcommands:\n"
               "  rl-closed-loop   Run the Phase 6 RL closed-loop demo\n"
               "                   (chat → reaction → train → swap → re-chat → scorecard)\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *sub = argv[2];
    if (argc < 3 || strcmp(sub, "rl-closed-loop") != 0) {
        fprintf(stderr, "demo: unknown subcommand: %s\n",
                argc < 3 ? "(none)" : sub);
        return HU_ERR_INVALID_ARGUMENT;
    }
    return hu_ml_cli_demo_rl_closed_loop(argc - 3, (const char **)(argv + 3),
                                         alloc);
}
```

- [ ] **Step 4: Build + run the demo against the mock backend**

```bash
cmake --build build-rl-sota -j
./build-rl-sota/human demo rl-closed-loop --backend huml \
    --persona demo_persona_e2e \
    --reactions tests/fixtures/e2e_reaction_signals.json \
    --reaction-count 50 \
    --prompt "what should i do first?" \
    --out tests/_tmp/proofs/demo-smoke \
    --require-positive-delta
echo "exit=$?"
ls tests/_tmp/proofs/demo-smoke/   # all 9 spec §8 files present
```

Expected: exit 0, all 9 files present.

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(ml,cli): add 'human demo rl-closed-loop' subcommand

Wires Phase 1 llamacpp + Phase 2 DPO + Phase 3 KTO/RM + Phase 4 GRPO
+ Phase 5 eval_gate into a single end-to-end demo CLI that produces
the spec §8 evidence directory. Backend selectable via --backend
{huml,mlx}; HUML is the deterministic toy-GPT path (wiring proof
only); MLX is real Gemma-3-4B-it on Apple Silicon.

Adds cmd_demo to src/main.c as a new top-level command, separate
from cmd_ml (per plan D3 — surface clarity + Track D Phase 1
conflict avoidance).

Refs spec §4.7, §8, §9 items 8/10/11, plan D2 + D3 + D5 + D9."
```

---

### Task 10: Write the `scripts/demo-rl-loop.sh` runner

**Files:**
- Create: `scripts/demo-rl-loop.sh`

- [ ] **Step 1: Author the script**

```bash
#!/usr/bin/env bash
# scripts/demo-rl-loop.sh — Phase 6 live demo runner.
#
# Apple Silicon only. Runs the closed loop against real Gemma-3-4B-it
# and a real Qwen-2.5-0.5B-Instruct reward model. Produces the
# win-condition scorecard from spec §1 in a real evidence dir under
# ~/.human/proofs/.
#
# This script is NEVER run in CI (per spec §6.5). Run locally before
# every release tag. Estimated wall-clock: ~3 minutes on M2.

set -euo pipefail

# Defaults (override via env)
PERSONA="${PERSONA:-seth}"
METHOD="${METHOD:-dpo}"
BACKEND="${BACKEND:-mlx}"
REACTION_COUNT="${REACTION_COUNT:-200}"
PROMPT="${PROMPT:-what should I do first?}"
DRY_RUN="${DRY_RUN:-0}"

# Prereq verification
echo "==> Verifying prerequisites..."
test "$(uname -s)" = "Darwin" \
    || { echo "ERROR: Apple Silicon required. uname=$(uname -s)"; exit 1; }
test "$(uname -m)" = "arm64" \
    || { echo "ERROR: Apple Silicon required. arch=$(uname -m)"; exit 1; }
test -x ./build-rl-sota/human \
    || { echo "ERROR: build-rl-sota/human missing. Run: cmake --preset rl_sota && cmake --build --preset rl_sota -j"; exit 1; }
test -f ~/.human/models/gemma-3-4b-it-Q4_K_M.gguf \
    || { echo "==> Gemma GGUF missing — fetching..."; bash scripts/fetch-gemma.sh; }
test -f ~/.human/models/qwen-2.5-0.5b-instruct-q4_k_m.gguf \
    || { echo "==> Qwen RM GGUF missing — fetching..."; bash scripts/fetch-qwen-rm.sh; }
python3 -c "from mlx_lm_lora.trainer.dpo_trainer import train_dpo" 2>/dev/null \
    || { echo "ERROR: mlx-lm-lora missing. Install with: pip install mlx-lm-lora"; exit 1; }
test -f ~/.human/personas/${PERSONA}.json \
    || { echo "ERROR: persona ${PERSONA} not found at ~/.human/personas/${PERSONA}.json"; exit 1; }

OUT_DIR=~/.human/proofs/$(date -u +%Y-%m-%d)-${METHOD}-step-$$
echo "==> Output: $OUT_DIR"

if [ "$DRY_RUN" = "1" ]; then
    echo "==> DRY_RUN=1; skipping actual invocation"
    exit 0
fi

set -x
./build-rl-sota/human demo rl-closed-loop \
    --persona "${PERSONA}" \
    --method "${METHOD}" \
    --backend "${BACKEND}" \
    --reaction-count "${REACTION_COUNT}" \
    --prompt "${PROMPT}" \
    --out "${OUT_DIR}" \
    --require-positive-delta
EXIT=$?
set +x

echo "==> Demo exit code: $EXIT"
case "$EXIT" in
    0) echo "==> WIN: persona-fidelity delta met ≥0.05 threshold."; ;;
    2) echo "==> SOFT FAIL: delta computed but missed threshold. See $OUT_DIR/eval_delta.json"; ;;
    3) echo "==> HARD FAIL: harness error before delta could be computed."; ;;
    *) echo "==> UNKNOWN exit code: $EXIT"; ;;
esac
echo "==> Evidence dir: $OUT_DIR"
echo "==> Open the runbook for next steps: docs/demos/rl-loop-demo.md"
exit $EXIT
```

- [ ] **Step 2: `chmod +x` and dry-run**

```bash
chmod +x scripts/demo-rl-loop.sh
DRY_RUN=1 bash scripts/demo-rl-loop.sh
```

Expected: prereq checks all pass (or fall through with the dry-run message), exit 0.

- [ ] **Step 3: Commit**

```bash
git commit -m "feat(scripts): add demo-rl-loop.sh for Phase 6 live demo

Apple-Silicon-only script that verifies prerequisites, fetches
models if needed, runs 'human demo rl-closed-loop' with sensible
defaults, and prints the evidence dir + interpretation of the
exit code. NEVER run in CI per spec §6.5 — local pre-release only.

Refs plan D2, spec §6.5 + §14."
```

---

### Task 11: Write the `docs/demos/rl-loop-demo.md` runbook

**Files:**
- Create: `docs/demos/rl-loop-demo.md`

- [ ] **Step 1: Author the runbook**

YAML frontmatter is required by `scripts/check-docs-frontmatter.sh` (which enforces `---` on line 1 of every `docs/*.md` outside `docs/plans/`):

```markdown
---
title: "RL Closed-Loop Demo Runbook"
created: 2026-05-12
status: living
audience: developers + reviewers
phase: 6
linked_spec: docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md
linked_plan: docs/plans/2026-05-11-rl-loop-phase-6-proof.md
reproducibility: local Apple Silicon only (no CI)
---

# RL Closed-Loop Demo Runbook

This runbook walks through running the Phase 6 demo end-to-end on
Apple Silicon and reading the resulting `~/.human/proofs/` evidence
directory.

For the deterministic CI test, see `tests/test_e2e_rl_loop.c`.
For the design rationale, see `docs/plans/2026-05-11-rl-loop-phase-6-proof.md`.

## 1. Prerequisites
...

## 2. One-command setup
...

## 3. Running the demo
...

## 4. Reading the evidence dir
...

## 5. Troubleshooting
...

## 6. Reproducibility recipe
...

## 7. FAQ
...

## 8. Citations
...
```

Sections 1–8 are filled out per the file structure section above. Sections 5 (Troubleshooting) and 7 (FAQ) directly mirror the risk register R3, R5 and the failure-mode F4.

- [ ] **Step 2: Validate frontmatter**

```bash
./scripts/check-docs-frontmatter.sh docs/demos/rl-loop-demo.md
./scripts/doc-fleet.sh   # full doc gate
```

> **ME5 fix:** verify `check-docs-frontmatter.sh` actually recurses into `docs/demos/`. If its glob is `docs/*.md` (single-level) the demo runbook is silently skipped and YAML frontmatter regressions ship undetected. Inspect the script first:
>
> ```bash
> rg -n '\bfor\b|glob|find' scripts/check-docs-frontmatter.sh
> ```
>
> If the iteration pattern is single-level (e.g. `for f in docs/*.md`), patch it to recurse (`docs/**/*.md` with `shopt -s globstar`, or `find docs -name '*.md'`) in the same Phase 6 commit that introduces `docs/demos/rl-loop-demo.md`. Pin the fix with the explicit invocation above so the runbook is exercised even when the script is non-recursive — that explicit call MUST pass before commit.

- [ ] **Step 3: Commit**

```bash
git commit -m "docs(demos): add Phase 6 RL closed-loop demo runbook

Runbook for scripts/demo-rl-loop.sh: prereqs, setup,
running, reading the evidence dir, troubleshooting, reproducibility,
FAQ, citations. Apple-Silicon-only; never run in CI.

Refs spec §14, plan Task 11, plan R3/R5/F4."
```

---

### Task 12: Phase 6 end gate

**Files:** none (subagent dispatch + tag)

- [ ] **Step 1: Run the full test suite under both presets**

```bash
cmake --build build-dev -j && ./build-dev/human_tests
cmake --build build-rl-sota -j && ./build-rl-sota/human_tests
```

Expected: pre-Phase-6 baseline + (number of new test functions in `tests/test_e2e_rl_loop.c`) + 1 (gated llamacpp hot-swap test). All pass; 0 ASan errors; 0 UBSan errors.

- [ ] **Step 2: Run `dead-code-finder` subagent**

```
Task: dead-code-finder
Prompt: Review src/ml/cli_demo.c, src/agent/lora_training_runner.c (HU_IS_TEST
        block), src/main.c (cmd_demo additions), tests/test_e2e_rl_loop.c,
        tests/test_llamacpp_lora_hotswap_midsession.c, scripts/demo-rl-loop.sh,
        docs/demos/rl-loop-demo.md, include/human/ml/cli_demo.h, and
        tests/include/hu_e2e_closed_loop.h. Catch any unused exports, unreachable
        branches, or dead helper functions introduced in Phase 6. Note that the
        deliberate duplication between src/agent/lora_training_runner.c::hu_e2e_closed_loop_run
        (HU_IS_TEST gated) and src/ml/cli_demo.c::cli_demo_run_closed_loop (release
        path) is documented per plan D5 and is NOT dead code.
```

Fix all findings; do not lower the bar to silence the subagent.

- [ ] **Step 3: Run live demo on Apple Silicon (manual verification step)**

```bash
bash scripts/demo-rl-loop.sh
ls ~/.human/proofs/2026-05-12-dpo-step-*/
cat ~/.human/proofs/2026-05-12-dpo-step-*/eval_delta.json
```

Expected: 9 files present per spec §8; `eval_delta.json::delta_mean ≥ 0.05` and `lower_95_ci > 0`. If not, iterate per R3 (more reactions, longer training); if after iteration the demo can't hit the threshold on real Gemma + the user's real persona, **Phase 6 is NOT done**. File a follow-up to investigate.

- [ ] **Step 4: Run `sprint-auditor` subagent**

```
Task: sprint-auditor
Prompt: Independently re-read docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md
        §4.7, §5 row 6, §6 Tier 4 (line 522), §8, §9 items 8 + 10 + 11, §14,
        and docs/plans/2026-05-11-rl-loop-phase-6-proof.md. Then verify against
        the actual deliverables on disk:
        - tests/test_e2e_rl_loop.c (≤30s under ASan; deterministic; persona delta ≥ 0.05)
        - scripts/demo-rl-loop.sh (Apple-Silicon-only, NEVER in CI)
        - docs/demos/rl-loop-demo.md (runbook with reproducibility recipe)
        - tests/fixtures/e2e_persona_seed.json (synthetic, no PII, 100 held-out prompts)
        - tests/fixtures/e2e_reaction_signals.json (50 events, 25+/25-)
        - src/ml/cli_demo.c + include/human/ml/cli_demo.h + 'human demo rl-closed-loop' wired
        - src/agent/lora_training_runner.c (close-the-loop wiring + HU_IS_TEST seam)
        - ~/.human/proofs/<demo-adapter-id>/ (live demo output: all 9 spec §8 files)

        Answer one question per DoD item from this plan's §"Definition of Done":
        did Phase 6 deliver this? Use evidence (file paths, line numbers,
        observed exit codes). Do NOT trust the team's claims; verify each.

        Verdict: PASS / PASS_WITH_NOTES / FAIL.
```

Phase 6 closes only on `PASS` (or `PASS_WITH_NOTES` where notes are scoped to follow-up issues, not blocking gaps).

- [ ] **Step 5: Write the phase-end summary**

`docs/proof/phase-6-summary.md` (mirrors prior phases' summary docs):

```markdown
---
title: "Phase 6 — E2E Proof + Demo (Summary)"
created: 2026-05-12
status: shipped
phase: 6
predecessor_tag: rl-sota-phase-5-complete
phase_tag: rl-sota-phase-6-complete
sprint_auditor_verdict: PASS
---

# Phase 6 Summary

[file list, test counts, evidence dir path, demo wall-clock,
 sprint-auditor verdict, links to all 9 spec §8 files in the
 demo evidence dir]
```

- [ ] **Step 6: Tag and update the umbrella plan**

```bash
git tag rl-sota-phase-6-complete
```

Update `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md` row 6 to `complete` with a link to `docs/proof/phase-6-summary.md` and `docs/proof/rl-loop-proof.md`.

---

## Definition of Done

Phase 6 closes when **all** of these are true:

1. `tests/test_e2e_rl_loop.c` passes with all 16+ test functions, including `test_e2e_closed_loop_deterministic_run1_vs_run2` + `test_e2e_closed_loop_deterministic_run1_vs_run3` (3 deterministic runs of `hu_e2e_closed_loop_run` produce byte-identical adapter SHA-256 pairwise) AND the SEC1/SEC2 pins (`test_e2e_closed_loop_negative_rate_anomaly_recorded`, `test_e2e_closed_loop_adapter_sha_mismatch_aborts_swap`).
2. The full E2E suite completes in ≤ 30 seconds in CI under ASan (pinned by `test_e2e_closed_loop_completes_within_30s_under_asan`).
3. `tests/fixtures/e2e_persona_seed.json` (100 held-out prompts + 50 sample fingerprints) and `tests/fixtures/e2e_reaction_signals.json` (25+/25- events) are committed; both parse as valid JSON.
4. `scripts/demo-rl-loop.sh` runs end-to-end on Apple Silicon and exits 0 (i.e. win-condition met) on the user's real persona corpus.
5. `~/.human/proofs/<demo-adapter-id>/` exists with all 9 evidence files from spec §8 (`manifest.json`, `training_curves.json`, `eval_before.json`, `eval_after.json`, `eval_delta.json`, `delta_responses.md`, `gate_decision.json`, `adversarial_review.md`, `reproduce.sh`).
6. **`eval_delta.json::delta_mean ≥ 0.05` (absolute persona-fidelity points on the [0,1] scale per spec §11 row 4)** AND `eval_delta.json::lower_95_ci > 0` on the same held-out 100 prompts pre and post (spec §1 win-condition row 2).
7. Latency p95 regression in `eval_delta.json::latency_p95_ratio ≤ 1.02` (i.e. ≤ 2% regression).
8. `human demo rl-closed-loop --help` lists the subcommand and prints the option set; `human demo --help` lists the `rl-closed-loop` subcommand.
9. `docs/demos/rl-loop-demo.md` runbook is committed with valid YAML frontmatter and passes `scripts/doc-fleet.sh`.
10. `sprint-auditor` subagent has issued PASS (or PASS_WITH_NOTES) verdict on Phase 6, logged in `docs/proof/phase-6-summary.md`. `dead-code-finder` clean. `critic` reviews of Tasks 4, 6, 7, 9 each marked addressed.

---

## Out of scope (explicit deferrals)

These are tempting under "ship the proof" but are **NOT** in Phase 6. Documented now to prevent mid-flight scope creep.

- ❌ **Multi-user concurrent training** — running closed-loop iterations for two personas in parallel; per spec §12, multi-tenant adapter routing is v1.5. Phase 6 ships the single-persona ("seth") demo.
- ❌ **Federated learning** — collaborative training across multiple devices with cryptographic aggregation. Out of scope for the entire spec, not just Phase 6.
- ❌ **RLAIF beyond LLM-judge** — full constitutional-AI / self-critique loops. Per spec §12: "Constitutional AI / self-critique loops — separate concern."
- ❌ **Speculative decoding** in the demo's chat path — per spec §12, v1.5 inference perf optimization.
- ❌ **Web dashboard visualization** of the closed-loop training curves — per spec §12, "Web dashboard visualization of training curves" is v1.5; Phase 6's evidence is markdown + JSON.
- ❌ **iOS/macOS native app integration** with the trained adapter — per spec §12, v1.5.
- ❌ **A second demo persona** beyond the synthetic `demo_persona_e2e` (test) and the real `seth` (demo) — single-persona shipping (spec §11 row 6).
- ❌ **Automated nightly demo run on a paid CI runner** — per spec §6.5 + §14, demo is local pre-release only.
- ❌ **Apple FM / Gemini Nano competitive columns in the demo's scorecard** — those are Phase 5's `human eval competitive` output, not Phase 6's demo. The demo focuses on the closed-loop proof; competitive comparison is the parallel artifact.
- ❌ **Online RL during chat** (TTT-style updates per message) — per spec §12, v2 research.

---

## Self-review note

This plan was authored after reading the full umbrella spec end-to-end (§1 through §15) and all four predecessor phase plans (Phases 0, 1, 2, 3). Phases 4 and 5 do not have plans authored yet (per `docs/plans/*rl-loop*` glob at plan-authoring time); the Phase 6 plan **assumes** Phase 4 ships `hu_grpo_*` per the spec §4.5 file map and Phase 5 ships `hu_eval_gate` + `hu_competitive_harness` + the 4-axis fidelity scorer per spec §4.6. If those phases ship with API divergence, Task 0 step 1 catches it (the `test -f` checks for `include/human/ml/grpo.h` and `include/human/eval/eval_gate.h` will fail) and this plan's Tasks 7 and 8 must be amended at execution time.

The plan is intentionally smaller than Phases 2 and 3 (12 tasks vs 12+ in those, but with significantly less new C — Phase 6 is composition, not invention). The single highest-risk decision is D1: trading off "the test exercises real Gemma" for "the test is deterministic + ≤30s in CI." That trade-off is conscious, motivated by R7 (CI runtime budget) and R5 (HUML can't lie about the wiring being closed even if it can't lie about real-Gemma quality), and compensated for by D2 (the demo path uses real models, locally, before every release tag). A reviewer who reads only the test and not the demo will miss the real-Gemma proof; the runbook (Task 11) is the artifact that prevents that confusion.

The deliberate test+release duplication of `hu_e2e_closed_loop_run` / `cli_demo_run_closed_loop` (D5) is the second-highest-risk decision. It is justified by AGENTS.md §3 Rule of Three (extract after 3 callers), but if Phase 7+ adds a third caller, Phase 6's commit log should make finding the two copies easy via the deliberate `git grep "Phase 6 D5 duplicate"` comment in both files.

The plan does NOT add an `aspect-panel` gate (D11) because spec §7 reserves that for P2/P4/P5. If sprint-auditor or critic surface enough disagreement during Phase 6 execution to warrant a panel, dispatching one is always available — but it is not a phase entry requirement.

The plan's largest single open assumption is that Phase 5's `hu_eval_gate` is callable with the API shape Phase 6's `cli_demo.c::write_evidence_dir` consumes. Task 0 step 4 verifies this; if Phase 5 ships with a different API, Task 0 fires and the plan amends Tasks 3 + 9 + 11 inline before any code change.

Three things were intentionally NOT included that a reader might expect:

1. **No new vtable, no new factory, no new loss function.** Phase 6 is wiring; if it adds a new abstraction it has scope-crept.
2. **No new top-level CMake option.** `HU_ENABLE_RL_FULL` (Phase 2) gates the demo CLI; no new flag is needed.
3. **No phase-end binary-size check beyond the existing CI gate.** Spec §6.4's ≤ +250 KB rule is enforced by CI on every PR; Phase 6 doesn't need a phase-specific check because the new code is gated behind `HU_ENABLE_RL_FULL=OFF` in the default release preset (zero-byte release-binary delta).
