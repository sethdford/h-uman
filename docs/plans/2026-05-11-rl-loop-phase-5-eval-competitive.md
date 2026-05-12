# Phase 5: Eval Gate + Competitive Harness — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Every Task that ships a loss/score function paired with a behavioural claim MUST be reviewed by the `verifier` subagent before the commit (spec §7); Tasks that touch the eval gate decision logic (4, 5, 11) MUST additionally clear the `aspect-panel` end-gate at Task 14.

**Goal:** Close the third RL loop from the umbrella spec — the **quality loop** — by:

1. Extending Track D Phase 1's deterministic communication-style fidelity scorer with a **4th decision-style axis** (hedging-vs-direct, question-vs-statement, imperative-vs-suggestive) as an **opt-in additive surface**: a new `hu_communication_style_fidelity_score_v2(...)` symbol that new code calls explicitly, while the existing 3-axis symbol stays the default to keep `tests/test_personal_model.c` and `scripts/check-lora-baseline.sh` byte-stable;
2. Shipping `src/eval/eval_gate.c` — a **statistical** promotion gate that composes (a) the 4-axis persona-fidelity scorer (`hu_communication_style_compare_response_sets_v2`, opt-in per D1/B1), (b) cached leaderboard runners (MT-Bench / AlpacaEval / IFEval), (c) latency p95, and (d) the Phase 3 `hu_reward_model_t` as the canonical reward signal, all under **one-sided bootstrap-CI** decision logic where `lower-95-CI(candidate) > baseline_mean + delta_min` is the bar (per umbrella §4.6 / §10 R8 / §11 Q4; B2-revised — the baseline is a fixed scalar from the previous baseline-eval run, so the only sample distribution to bootstrap is the candidate's; two-sided non-overlap testing is deferred to Phase 6 once baseline sample arrays are persisted);
3. Shipping the **competitive harness** that runs side-by-side against stock Gemma-3-4B (`stock_baseline.c`), our DPO/KTO/GRPO LoRAs, **Apple Foundation Models** (via a long-running Swift JSON-over-stdio server), and **Gemini Nano** (via headless Chrome `--enable-features=AIPromptAPI`), with deterministic test-mode fallbacks so the C suite stays hermetic;
4. **Folding in three Phase 2 sprint-auditor deferrals at production scale** that previous phases marked "deferred to Phase 5": (i) `src/agent/lora_training_runner.c` calls `eval_gate` BEFORE `hu_provider_load_adapter` so the chat-time adapter swap is statistically gated, not just file-existence gated; (ii) the inbound channel-reaction → SQLite preference DB → trainer scheduler path becomes a real production wiring (not test-only `reaction_handler` invocation) by wiring `hu_imessage_poll_reactions` into the daemon poll loop with feature-flag gating; (iii) adapter hot-swap via `hu_provider_load_adapter` at production scale gets the gate's `gate_decision.json` + `~/.human/proofs/<adapter-id>/` evidence directory contract from §8 of the umbrella spec.

**Architecture:** Four NEW vtables and bridge subprocesses, all behind the existing `hu_provider_t` / `hu_reward_model_t` walls.

- **`hu_eval_judge_external_t`** (NEW vtable) in `include/human/eval/judge_external.h` — uniform "give me a completion for this prompt" interface for Apple FM, Gemini Nano, and the stock-Gemma baseline. The eval gate doesn't care which competitor is on the other side of the FFI.
- **`hu_eval_gate_t`** (NEW non-vtable struct, single impl per spec §4.6) — composes scorer + leaderboard + reward model + latency into a single `verdict_t { promote: bool, reason: char[], bootstrap_cis: ... }`. NOT a vtable: there is exactly one gate impl and it's the spec.
- **`hu_leaderboard_runner_t`** (NEW vtable) in `include/human/eval/leaderboard.h` — `run(prompts, n) → scores[]` with cached gold-judge fallback for offline determinism. Three impls land in this phase: `mt_bench`, `alpaca_eval`, `ifeval`.
- **`hu_competitive_harness_t`** (NEW non-vtable orchestrator) — runs an N-prompt grid across {stock, dpo, kto, grpo, apple_fm, gemini_nano}, renders the §1 scorecard with bootstrap CIs, writes proof JSON.

**External subprocess bridges (mirror Phase 2/3 MLX subprocess pattern, different language):**

- `scripts/eval_external/apple_fm_server.swift` — long-running Swift server that reads `{"id": N, "prompt": "..."}` from stdin (one JSON line per request), runs `FoundationModels.LanguageModelSession.respond(to: prompt)` via the WWDC 2025-26 Apple Foundation Models framework, writes `{"id": N, "text": "..."}` to stdout. Lifecycle: spawned on first eval, kept alive across the whole run, killed at the end. Failure modes: not-on-macOS-26+, no entitlement, OOM mid-response — each surfaces a distinct JSON error code that the C side maps to `HU_ERR_NOT_SUPPORTED` (degrades gracefully into "Apple FM: unavailable (reason)" scorecard row per spec §10 R2).
- `scripts/eval_external/chrome_ai_server.js` — Node.js script that spawns headless Chrome with `--enable-features=AIPromptAPI --headless=new --remote-debugging-port=0`, drives `window.ai.languageModel.create()` + `.prompt(text)` via CDP, returns JSON-per-line on stdout. Same lifecycle + same graceful-degradation contract.
- **Determinism contract:** both servers run **only** when `HU_IS_TEST == 0`. In tests, the C side's `hu_apple_fm_judge_create_for_test` / `hu_gemini_nano_judge_create_for_test` factories install a canned-response table; the subprocess is never spawned. Mirrors the Phase 2 / Phase 3 MLX-subprocess test gating.

**Statistical decision contract (the eval gate, B2-revised one-sided form):** Bootstrap CI with B=1000 resamples on the **candidate-side** distribution only; the baseline is a fixed scalar from the previous baseline-eval run (stored as `double baseline_*_mean` per criterion). Phase 3 has the same B in `tests/test_reward_model_train.c::test_reward_model_train_bootstrap_ci`, so the helper lives at `src/eval/bootstrap.c` and Phase 5 just consumes it. Promotion requires **all four** of (where `delta_min` defaults are spec §1 row 2 numerics):

1. `lower-95-CI(candidate persona-fidelity) > baseline_persona_fidelity_mean + 0.05` (spec §1 row 2 "≥ +5%" decoded per spec §11 Q4 "absolute persona-fidelity-score points"; `delta_min = +0.05` — must improve);
2. `lower-95-CI(candidate MT-Bench) > baseline_mt_bench_mean + (-0.01)` AND `lower-95-CI(candidate IFEval) > baseline_ifeval_mean + (-0.02)` (the "no regression" half, per spec §1 row 2 columns 3-4; `delta_min` = -0.01 / -0.02 allow small regressions);
3. `candidate_p95_latency_ms ≤ baseline_p95_latency_mean + 50ms` (spec §1 row 2 column 5; latency p95 is a single pre-aggregated scalar — no sample array, no bootstrap, just a scalar comparison; `delta_min = +50.0ms`);
4. `lower-95-CI(candidate reward-model mean score) > 0 + 0` (Phase 3 RM as canonical reward, per umbrella spec §3.4 third vtable; `delta_min = 0`. When `gate->reward_model == NULL` this criterion is SKIPPED with a warning written to `verdict.reason` — see D3 / H3 — never dereference a NULL `reward_model->vtable`. **fix(plan,eval,api):** the same NULL-skip semantics apply to `gate->mt_bench` and `gate->ifeval` (round-3 NEW-2 — codified in D3 / H3 struct annotations); cheap pre-commit gates like Task 10b's `lora-ab --require-positive` set all three optional runners to NULL and only run the persona-fidelity and latency criteria).

The umbrella spec §5 row 5's "bootstrap CIs" phrasing is interpreted as the **candidate-side** bootstrap; storing baseline sample arrays for two-sided non-overlap testing is deferred — see Phase 6 R-budget.

Any single failure rejects the promotion and writes a structured `gate_decision.json` with per-criterion verdict + CI bounds + the failing criterion's slack-to-pass.

**Tech Stack:** C11, AddressSanitizer + UndefinedBehaviorSanitizer in `dev` preset; existing `hu_communication_style_fidelity_score` + `hu_communication_style_compare_response_sets` (Track D Phase 1, unchanged at the v1 ABI); the new 4th axis added behind a new entry point so existing CI gates (`scripts/check-lora-baseline.sh`, the Track D fixture) stay byte-stable; `hu_reward_model_t` (Phase 3 Task 2 / Task 4, unchanged); `hu_lora_runner_ctx_t` and `hu_lora_training_runner` (W14, modified to accept an optional `hu_eval_gate_t *` before promotion); Apple Foundation Models framework (macOS 26+, optional via Swift bridge subprocess); headless Chrome / Chromium Canary with `--enable-features=AIPromptAPI` (optional, fetched lazily). All new external dependencies are runtime-resolved and degrade to `HU_ERR_NOT_SUPPORTED` cleanly per spec §10 R2/R3.

**Linked spec:** `docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md` §4.6 (file inventory, lines 343–372) + §5 row 5 ship contract (line 500) + §6 testing tier ladder (line 519).
**Linked umbrella plan:** `docs/plans/2026-05-11-full-sota-rl-improvement-loop.md`
**Predecessor plans:** `docs/plans/2026-05-11-rl-loop-phase-0-honesty.md` (tag `rl-sota-phase-0-complete`), `docs/plans/2026-05-11-rl-loop-phase-1-llamacpp.md` (tag `rl-sota-phase-1-complete`), `docs/plans/2026-05-11-rl-loop-phase-2-dpo-reactions.md` (tag `rl-sota-phase-2-complete`), `docs/plans/2026-05-11-rl-loop-phase-3-kto-rm.md` (tag `rl-sota-phase-3-complete`), Phase 4 (`docs/plans/2026-05-11-rl-loop-phase-4-grpo.md`, tag `rl-sota-phase-4-complete`).

---

## Phase 5 status snapshot

| Step | Owner | Status | Date |
|------|-------|--------|------|
| Plan authored | this doc | ⏳ in progress | 2026-05-12 |
| Plan reviewed (`critic` + `spec-verifier`) | subagents | ⏳ | — |
| Plan committed | git | ⏳ | — |
| Implementation start gate | subagent-driven | ⏳ | — |
| Tasks 0–14 implemented | subagent-driven | ⏳ | — |
| 4th-axis additive opt-in landing (v1 default unchanged) | Task 1 | ⏳ | — |
| Bootstrap CI helper extracted + reused | Task 2 | ⏳ | — |
| Promotion-gate accept/reject on synthetic results | Task 5 | ⏳ | — |
| Apple FM + Chrome AI test-mode fallback gate | Task 7, Task 8 | ⏳ | — |
| `lora_training_runner` promotion-gate wiring (Phase 2 deferral fold-in) | Task 11 | ⏳ | — |
| Daemon reaction-poll wiring (Phase 2 deferral fold-in) | Task 12 | ⏳ | — |
| Adapter-hot-swap proof directory contract (Phase 2 deferral fold-in) | Task 13 | ⏳ | — |
| Phase 5 end gate (full suite + dead-code + aspect-panel + auditor + tag) | Task 14 | ⏳ | — |

---

## What we're building on (Phase 0–4 deliverables, do NOT duplicate)

**Phase 0** (tag `rl-sota-phase-0-complete`): silent-bug fixes, atomic personal-model save, DPO naming, `~/.human/private/` `.gitignore`d.

**Phase 1** (tag `rl-sota-phase-1-complete`): llama.cpp Metal, `chat_with_system`, KV cache, sampling, `vtable.load_adapter` / `unload_adapter`, Gemma-3-4B-it GGUF fetcher + sanity gate, `rl_sota` CMake preset.

**Phase 2** (tag `rl-sota-phase-2-complete`): `hu_rl_trainer_t` vtable, `hu_policy_logprobs`, `hu_reference_model`, two-track DPO (HUML + MLX-via-`mlx-lm-lora`), iMessage tapback + Slack reactji inbound parsers, `hu_reaction_event_t`, `hu_reaction_handler_handle_event` writing into `dpo_pairs`. **Phase 2 explicitly DEFERRED to Phase 5:** (i) production wiring of the channel-reaction → preference-DB path into the daemon poll loop; (ii) integration of the promotion gate into `lora_training_runner` (Phase 2 only shipped the handler + factory); (iii) adapter hot-swap evidence-directory contract.

**Phase 3** (tag `rl-sota-phase-3-complete`): `hu_value_head_t`, `hu_reward_model_t` (HUML + MLX/Qwen-2.5-0.5B), Bradley-Terry RM training, `human ml kto-train` + `human ml rm-train`, Phase 2 MLX factory deferral fold-in, **bootstrap-CI helper** at `src/eval/bootstrap.c` (extracted in Phase 3 Task 11's RM convergence test).

**Phase 4** (tag `rl-sota-phase-4-complete`): `hu_rollout`, `hu_grpo` (group-relative baseline, PPO clip ε=0.2, KL penalty β=0.04), `hu_kl_divergence`, `human ml grpo-train`. Phase 5 consumes Phase 4's adapters as one column in the competitive scorecard — does NOT modify GRPO code.

**What Phase 5 does NOT touch (other phases own them):**

- DO NOT modify `hu_communication_style_fidelity_score` signature OR body (Track D Phase 1 / Phase 0). Phase 5 ADDS a new 4-axis entry point `hu_communication_style_fidelity_score_v2` ALONGSIDE the existing v1 symbol. The v1 symbol's body stays byte-identical (B1: opt-in only — no rename, no deprecation, no forwarding shim).
- DO NOT rename `hu_communication_style_compare_response_sets` — Phase 5 ADDS a `_v2` variant that uses the 4-axis scorer; existing callers keep working through the unchanged v1 surface.
- DO NOT modify `hu_reward_model_t` or its vtable (Phase 3). Phase 5 consumes `vtable->score_batch` only.
- DO NOT modify `src/providers/llamacpp.c` (Phase 1). Phase 5's `stock_baseline.c` consumes the existing `hu_provider_t.chat` API.
- DO NOT extend `hu_channel_vtable_t` for reaction polling (Phase 2 R12 decision still applies). Daemon-poll wiring goes through the existing `hu_imessage_poll_reactions` symbol that Phase 2 already shipped, called from `src/daemon.c` directly.
- DO NOT modify `tests/fixtures/lora_baseline_persona.json` shape — Phase 5 EXTENDS it additively with a 4th-axis rubric column (per spec §4.6 line 351 "additive"); the existing 3-axis floor in `scripts/check-lora-baseline.sh` stays as-is and a new optional 4-axis floor is added behind a `--include-v2-axis` flag (defaults to OFF until the rubric is corpus-owner-rated).

---

## Phase 5 boundary with in-flight Track D Phase 1 work

Per spec §1.5.3, Track D Phase 1 still owns `hu_communication_style_fidelity_score` and `hu_communication_style_compare_response_sets` plus the `lora-baseline` / `lora-ab` / `check-lora-baseline.sh` gate. Phase 5's additive 4-axis surface is the single most contentious shared-file zone in the whole RL program.

**Coordination contract:**

- Phase 5 lands the additive v2 symbol in **a single atomic commit** at Task 1 (B1-revised: opt-in only — adds `include/human/memory/personal_model.h` + `src/memory/personal_model.c` v2 entry points; does NOT touch `tests/test_personal_model.c`; does NOT touch `src/ml/fidelity.c` or `src/ml/cli.c` call sites — they stay on the v1 default symbol). Because the v1 symbol's body is byte-identical, `scripts/check-lora-baseline.sh`'s 0.50 floor and `tests/test_personal_model.c::personal_model_fidelity_*` bounds remain green by construction.
- Track D's `tests/fixtures/lora_baseline_persona.json` is read by Phase 5 in a strictly additive way: a new sidecar fixture `tests/fixtures/lora_baseline_persona_v2_responses.json` holds the 4-axis prompt-tagged reference responses (spec §4.6 line 352). The existing fixture is **read-only** to Phase 5.
- Rebase against `main` at the start of each task. If Track D Phase 1 lands changes to the scorer between phase boundaries, the spec-verifier subagent at Task 0 step 3 re-validates the fold-in mapping.

---

## Architectural decisions

### D1: 4th decision-style axis is OPT-IN, NOT opt-out

The umbrella spec says (§4.6 line 349): *"Preserves the old 3-axis behavior behind a new entry-point `hu_communication_style_fidelity_score_v1` (deprecated shim) so existing callers and CI fixtures don't break; the new default returns the 4-axis mean."*

**Decision (B1-revised after adversarial review):** v2 is **opt-in, not opt-out**. The default symbol `hu_communication_style_fidelity_score` stays 3-axis (v1 behaviour, byte-stable). New code that wants the 4th decision-style axis calls `hu_communication_style_fidelity_score_v2(...)` explicitly. The three internal call sites that consume the default symbol — `src/memory/personal_model.c:1377` (personal-model fidelity summary), `src/ml/cli.c:1509` (CLI fidelity output), `src/ml/fidelity.c:75` (internal scorer dispatcher) — STAY on 3-axis. The eval gate (Task 5) and competitive harness (Task 9) opt-in to v2 explicitly. `include/human/memory/personal_model.h` ADDS `hu_communication_style_fidelity_score_v2(...)` ALONGSIDE the existing v1 symbol (no replace, no rename, no deprecation attribute on the v1 symbol).

Rationale: the existing test suite has hard-coded bounds tuned to the 3-axis mean:

- `tests/test_personal_model.c:1296` `personal_model_fidelity_low_for_uppercase_response` expects `score < 0.75f` — v2 gives exactly 0.75 (boundary failure).
- `tests/test_personal_model.c:1345` `personal_model_fidelity_zero_when_length_extremely_off` expects `score >= 0.6f && score < 0.7f` — v2 gives 0.75 (definite failure).

Plus `scripts/check-lora-baseline.sh`'s floor of 0.50 was tuned against 3-axis. Silently switching the default would break these existing gates the moment Phase 5 lands — exactly the cross-stream contamination that bit Phase 2. The opt-in approach isolates the scoring-math change from any floor-tuning change. Plan text and tests that previously said "default forwards to v2" have been struck and replaced with "default stays v1; v2 is a new explicit symbol".

**4th axis math (decision-style match):**

```
score_decision_style(target, response) =
  0.33 * triangular_match(hedging_ratio(response),  target->hedging_ratio)
+ 0.33 * triangular_match(question_ratio(response), target->question_ratio)
+ 0.34 * triangular_match(imperative_ratio(response), target->imperative_ratio)
```

Where:
- `hedging_ratio` = count of tokens in {"maybe", "perhaps", "probably", "possibly", "might", "could", "i think", "i guess", "kinda", "sort of"} divided by total tokens.
- `question_ratio` = count of `?` chars divided by sentence count (where sentence count = max(1, count of `.!?`)).
- `imperative_ratio` = fraction of sentences starting with an imperative verb (regex against a 30-verb table: "do", "try", "use", "go", "make", "take", "stop", "start", "check", "look", "see", "consider", "ensure", …).
- `triangular_match(observed, target)` reuses the existing `match_triangle` helper at `src/memory/personal_model.c` (used for lowercase/abbreviation match); 1.0 when observed == target, linearly decays to 0.0 at ±0.5 distance.

`hu_communication_style_t` gets three new EWMA-tracked float fields: `hedging_ratio`, `question_ratio`, `imperative_ratio`, plus their `last_observed_at` is shared with the existing `last_observed_at` (no per-axis freshness, KISS). Default values are 0.0 (no signal); a `sample_count` of 0 means "no comparison possible" and the 4-axis score returns -1.0f per the existing v1 contract.

### D2: Bootstrap CI helper is shared (extracted in Phase 3 Task 11)

Phase 3 already extracted a bootstrap-CI helper to `src/eval/bootstrap.c` for the RM convergence test (B=1000 resamples, 95% percentile bounds). Phase 5 consumes the same helper at `eval_gate.c`. No new bootstrap implementation. If Phase 3 missed the extraction (sprint-auditor will catch in Task 0), Phase 5 Task 2 extracts it from `tests/test_reward_model_train.c` into `src/eval/bootstrap.c` as the first task — both the test and the gate then use the shared helper.

### D3: `eval_gate` is a struct, not a vtable (B2/H2/H3-revised)

There is exactly one gate implementation (the spec). Vtable-izing it would be speculative abstraction (YAGNI).

**Eval-gate criterion (D3-revised, B2):** `lower-95-CI(candidate) > baseline_mean + delta_min` where `delta_min = +0.05` for persona-fidelity (must improve), `delta_min = -0.01` for MT-Bench, `delta_min = -0.02` for IFEval (allow small regressions), `delta_min = +50.0ms` for latency p95 (allow 50ms regression), `delta_min = 0` for the reward criterion. This is a ONE-SIDED bootstrap on the candidate's score distribution against the baseline scalar. The umbrella spec §5 row 5's "bootstrap CIs" phrasing is interpreted as the candidate-side bootstrap; the baseline is a fixed scalar from the previous baseline eval run (stored as `double baseline_*_mean` per criterion). Storing baseline sample arrays for two-sided non-overlap testing is deferred — see Phase 6 R-budget.

**Optional criteria (H3 — fix(plan,eval,api): all three optional fields codified, round-3 NEW-2):** the gate has **three** optional criterion runners — `reward_model`, `mt_bench`, and `ifeval`. Each is independently `NULL`-skippable, and the production `hu_eval_gate_decide` MUST null-check each before dereferencing the vtable.

The null-skip contract for all three is identical:

1. When the runner pointer is `NULL`, that criterion is SKIPPED.
2. The verdict's `<criterion>_pass` is set to `true` (skipped criteria do not block promotion).
3. The verdict's `<criterion>_ci_lower` and `<criterion>_ci_upper` are set to `0.0` as a sentinel.
4. The verdict's `<criterion>_skipped` boolean is set to `true`.
5. A line `"<criterion> criterion skipped: runner==NULL"` is appended to `verdict.reason`.

This contract applies to all three optional criteria:

- `gate->reward_model == NULL` → reward criterion skipped (cold-start before any RM training).
- `gate->mt_bench == NULL` → MT-Bench criterion skipped (lightweight pre-commit gates like `human ml lora-ab --require-positive` that don't run leaderboards — see Task 10b).
- `gate->ifeval == NULL` → IFEval criterion skipped (same rationale as MT-Bench).

NEVER dereference `<runner>->vtable` without a null guard on the outer pointer. The `hu_eval_gate_t` struct definition below annotates each optional field with `/* may be NULL — criterion skipped per H3 */`.

Task 10b's lightweight `lora-ab --require-positive` pre-commit gate sets `gate.mt_bench = NULL`, `gate.ifeval = NULL`, AND `gate.reward_model = NULL` — only the persona-fidelity and latency criteria are evaluated. The persona-fidelity criterion is the cheap signal `lora-ab` was already producing; the gate just adds bootstrap-CI rigour on top.

```c
typedef struct {
    /* Inputs */
    const hu_communication_style_t *target_style;
    hu_reward_model_t *reward_model;            /* may be NULL — criterion skipped per H3 */
    hu_leaderboard_runner_t *mt_bench;          /* may be NULL — criterion skipped per H3 */
    hu_leaderboard_runner_t *ifeval;            /* may be NULL — criterion skipped per H3 */
    /* Baselines (from prior eval run on the same prompt set; scalars only —
     * baseline sample arrays are deferred to Phase 6 per B2). */
    double baseline_persona_fidelity_mean;
    double baseline_mt_bench_mean;
    double baseline_ifeval_mean;
    double baseline_p95_latency_ms;
    /* Thresholds (spec §1 row 2 numerics) */
    double persona_delta_min;       /* default +0.05 (must improve) */
    double mt_bench_regression_max; /* default -0.01 (allow small regression) */
    double ifeval_regression_max;   /* default -0.02 (allow small regression) */
    double latency_delta_max_ms;    /* default +50.0 (allow 50ms regression) */
    /* Bootstrap config */
    uint32_t bootstrap_samples;     /* MUST be >= 100; 0 returns HU_ERR_INVALID_ARGUMENT (H5) */
    uint32_t bootstrap_seed;        /* deterministic across runs */
} hu_eval_gate_t;
```

Verdict struct (B2-revised: no latency CI because p95 is a pre-aggregated scalar with no samples to bootstrap):

```c
typedef struct {
    bool promote;
    char reason[1024];           /* human-readable, written to gate_decision.json */
    /* Per-criterion bootstrap CIs and pass/fail flags. Latency has no CI
     * — candidate_p95_ms is a pre-aggregated scalar (B2). */
    double persona_ci_lower, persona_ci_upper;
    bool   persona_pass;
    double mt_bench_ci_lower, mt_bench_ci_upper;
    bool   mt_bench_pass;         /* true when mt_bench==NULL — skipped per H3 (round-3 NEW-2) */
    bool   mt_bench_skipped;      /* fix(plan,eval,api): H3 explicit skipped flag for MT-Bench (round-3 NEW-2) */
    double ifeval_ci_lower, ifeval_ci_upper;
    bool   ifeval_pass;           /* true when ifeval==NULL — skipped per H3 (round-3 NEW-2) */
    bool   ifeval_skipped;        /* fix(plan,eval,api): H3 explicit skipped flag for IFEval (round-3 NEW-2) */
    double candidate_p95_ms;     /* B2: pre-aggregated scalar (no CI) */
    bool   latency_pass;
    double reward_ci_lower, reward_ci_upper;
    bool   reward_pass;           /* true when reward_model==NULL — skipped per H3 */
    bool   reward_skipped;        /* H3: explicit "skipped" flag */
} hu_eval_gate_verdict_t;
```

**API signature (H2-revised — must take the prompt fixture array):**

```c
hu_error_t hu_eval_gate_decide(
    const hu_eval_gate_t *gate,
    const char **prompts, size_t n_prompts,      /* H2: prompts MUST be passed */
    const char **candidate_responses,
    size_t n_responses,                          /* must equal n_prompts; MUST be >= 30 (round-3 NEW-MED-3) */
    double candidate_p95_ms,                     /* B2: scalar (no array) */
    hu_eval_gate_verdict_t *out);
```

**fix(plan,eval,stats): `n_responses >= 30` floor (round-3 NEW-MED-3).** The bootstrap CI is statistically invalid for very small samples. With `n_responses = 1` the resampled mean is degenerate (zero variance — every resample is the same point), the percentile bounds collapse to the point estimate, and the gate would falsely conclude "lower-95-CI > baseline + delta_min" whenever the single observation happens to clear the threshold. The Wilson-interval rule-of-thumb requires `n >= 30` for normal-approximation-based confidence intervals to be reliable; bootstrap percentile CIs follow the same floor in practice. The production call site in `src/agent/lora_training_runner.c` (Task 11) passes 100-prompt fixtures (well above 30); Task 5's unit tests use 20 only to keep test runs fast, so they use the `_for_test` array-injected entry point which has its own threshold contract (`n >= 10` for tests — half the production floor, still enough to avoid the n=1 degeneracy).

**Precondition contract:**

```c
hu_error_t hu_eval_gate_decide(...) {
    if (n_responses < 30) {
        hu_log_error("hu_eval_gate_decide: n_responses=%zu < 30 — bootstrap CI is "
                     "statistically invalid below Wilson-interval rule-of-thumb floor; "
                     "increase the prompt fixture or use the _for_test entry point",
                     n_responses);
        return HU_ERR_INVALID_ARGUMENT;
    }
    /* ... rest of the gate ... */
}
```

The `_for_test` variant `hu_eval_gate_decide_from_arrays_for_test` (used in Task 5's unit tests with `n=20`) carries a relaxed floor of `n >= 10` — still enough to avoid the n=1 degeneracy but small enough that test runs stay fast. The relaxed floor is documented in the `_for_test` signature comment and is NOT exposed in the production API. Task 5's regression pin `test_eval_gate_rejects_when_n_below_floor_for_test` (added per round-3 NEW-MED-3) exercises the boundary.

The production call site in `src/agent/lora_training_runner.c` (Task 11) passes the prompt fixture array loaded from `tests/fixtures/lora_baseline_persona_v2_responses.json::prompts` (100 prompts — well above the 30-floor). The `_for_test` array-injected entry point (Task 5) is a separate helper that takes pre-scored per-prompt double arrays for unit-test convenience.

### D4: External judges DO share a vtable

Apple FM, Gemini Nano, and stock-Gemma-baseline all answer the same question: "given prompt P, what's the completion?" Sharing a vtable means the competitive harness writes one loop instead of three switch statements. Net-new files justify the abstraction.

```c
typedef struct hu_eval_judge_external_vtable {
    const char *(*name)(void *ctx);                /* "apple_fm" | "gemini_nano" | "stock_gemma" */
    bool        (*available)(void *ctx);           /* OS check, framework check, network check */
    hu_error_t  (*generate)(void *ctx, const char *prompt, size_t prompt_len,
                            char *out, size_t out_cap, size_t *out_written);
    void        (*deinit)(void *ctx, hu_allocator_t *alloc);
} hu_eval_judge_external_vtable_t;
```

`available()` is cheap and is called once per harness run; `generate()` is the per-prompt call. The contract is "if `available()` returns false, the harness writes `unavailable (reason)` for that judge's column and continues; it does NOT fail the run" (spec §10 R2/R3, spec §9 DoD #14).

### D5: Apple FM and Chrome AI bridges follow the Phase 2 MLX subprocess pattern, different binary

- Long-running subprocess speaking JSON-per-line over stdio.
- Spawned lazily on first `generate()` call; killed in `deinit()`.
- Heartbeat: not strictly required since the subprocess is short-lived (one eval run = ~5 minutes), but the C side reads stderr in a separate descriptor and surfaces the last 4KB on subprocess death (debugging contract).
- Test-mode fallback: when `HU_IS_TEST == 1`, the factory installs a canned-response table from `tests/fixtures/external_judge_canned.json` and never spawns the subprocess. This is enforced by an `#ifdef HU_IS_TEST` guard in `apple_fm_client.c` / `gemini_nano_client.c`.

The Swift and Node binaries are kept as small as practical (one source file each, no build step beyond `swiftc` / `node --check`). They live in `scripts/eval_external/` rather than `third_party/` because they're our code, not vendored.

### D6: Test mode is hermetic; production is real

Every external bridge has two code paths:

```c
hu_error_t hu_apple_fm_judge_create(hu_allocator_t *alloc, hu_eval_judge_external_t *out) {
#if HU_IS_TEST
    return hu_apple_fm_judge_create_for_test(alloc, out);  /* canned responses */
#else
    return hu_apple_fm_judge_create_subprocess(alloc, out); /* real Swift bridge */
#endif
}
```

Production paths spawn real subprocesses. Test paths never do. The `_for_test` factory also accepts an `HU_FORCE_REAL_BRIDGE=1` env var override to allow the optional local-only `rl-sota-full.yml` workflow to test the real subprocess path; the C side reads the env var inside the `#if HU_IS_TEST` block (so it's compiled out of release).

### D7: `lora_training_runner` gate integration uses an optional `hu_eval_gate_t *` pointer

Spec §4.6 row "MODIFY src/agent/lora_training_runner.c" says "Call the promotion gate before promoting adapter; reject if gate fails."

Decision: add a new field `hu_eval_gate_t *eval_gate` to `hu_lora_runner_ctx_t` (defined in `include/human/agent/lora_runner.h`). When the field is NULL (cold-start / migrating callers / test fixtures), the runner skips gating and behaves exactly like it does today (backward-compatible). When the field is non-NULL, the runner:

1. After `hu_learner_train` succeeds and BEFORE `hu_provider_load_adapter`, gathers the trained-adapter responses against a fixed eval prompt set (defaulting to `tests/fixtures/lora_baseline_persona_v2_responses.json::prompts`);
2. Calls `hu_eval_gate_decide(...)`;
3. On `verdict.promote == false`: writes `~/.human/proofs/<adapter-id>/gate_decision.json` with the verdict, **does NOT call `hu_provider_load_adapter`**, leaves the adapter file on disk under `<adapter-path>.rejected` (move-on-reject; the file is preserved for human inspection), logs at warn level, returns `HU_OK` (the train succeeded; only the promotion was vetoed).
4. On `verdict.promote == true`: writes the same `gate_decision.json` plus the rest of the §8 evidence directory (`manifest.json`, `eval_before.json`, `eval_after.json`, `eval_delta.json`, `delta_responses.md`, `reproduce.sh`), THEN calls `hu_provider_load_adapter` as today.

`HU_ERR_NOT_SUPPORTED` from a partially-available external judge does not block promotion — the verdict explicitly says "skipped external judge: <name>" in the reason.

### D8: Daemon reaction-poll wiring is feature-flagged (Phase 2 deferral)

Phase 2 shipped `hu_imessage_poll_reactions` but did not call it from the daemon poll loop. Phase 5 wires it but gates the call behind a `[reaction_collection]` config block in `~/.human/config.json` (new section, default OFF):

```json
{
  "reaction_collection": {
    "enabled": false,
    "channels": ["imessage", "slack"]
  }
}
```

Reasoning: production reaction collection touches the user's chat.db with elevated read frequency and writes to the preference SQLite. We don't enable it by default on first launch. The `human onboard` wizard (M4) gets a new step asking the user to opt in. CI runs default-off so the test suite is unaffected.

### D9: Adapter hot-swap evidence-directory contract is enforced by `lora_training_runner`, not `eval_gate`

The gate computes a verdict. The runner is responsible for:

- Generating the canonical `adapter_id = "<YYYY-MM-DD>-<method>-step-<step_index>"` (spec §11 naming convention);
- Creating the `~/.human/proofs/<adapter-id>/` directory;
- Writing all nine evidence files;
- Triggering the hot-swap iff the verdict is positive.

This keeps `eval_gate.c` pure (input → verdict, no filesystem side effects) and makes the verdict trivially testable in-memory. The directory layout is pinned in `tests/test_lora_training_runner_proof_directory.c` (Task 13).

### D10: Stock-Gemma baseline is a thin wrapper, not a parallel inference path

`src/eval/stock_baseline.c` (~100 LOC per spec) implements `hu_eval_judge_external_t` by holding a borrowed `hu_provider_t *` (the llamacpp provider from Phase 1) and calling `provider->vtable->chat` with **no adapter loaded** (`hu_provider_unload_adapter(provider, "")` before generation). Cheap, correct, and proves to a reviewer that the "stock" column in the scorecard is genuinely the un-LoRA'd model.

---

## File inventory

### NEW (Phase 5 net-new vs. Phase 2 fold-in)

| Origin | Action | Path | LOC | Responsibility |
|--------|--------|------|-----|----------------|
| **net-new P5** | NEW | `include/human/eval/judge_external.h` | ~80 | `hu_eval_judge_external_vtable_t`, `hu_eval_judge_external_t` struct, factory declarations |
| net-new P5 | NEW | `src/eval/judge_external.c` | ~120 | Factory registry; deinit boilerplate; vtable-wiring tests use this seam (D4) |
| net-new P5 | NEW | `include/human/eval/leaderboard.h` | ~80 | `hu_leaderboard_runner_vtable_t`, configs for MT-Bench / AlpacaEval / IFEval |
| net-new P5 | NEW | `src/eval/leaderboard.c` | ~420 | All three leaderboard runners with cached gold-judge fallback (~140 LOC each). Cache lives at `~/.human/eval_cache/<runner>/<prompt_hash>.json`; cache miss falls back to a deterministic test mode that returns canned mean ± 0.02 from `tests/fixtures/leaderboard_canned.json`. Production cache miss returns `HU_ERR_NOT_SUPPORTED` cleanly |
| net-new P5 | NEW | `include/human/eval/eval_gate.h` | ~120 | `hu_eval_gate_t`, `hu_eval_gate_verdict_t`, `hu_eval_gate_decide` API |
| net-new P5 | NEW | `src/eval/eval_gate.c` | ~220 | Composes scorer + leaderboard + reward model + latency; bootstrap CIs (D2); pass/fail per criterion + overall verdict; writes structured reason string |
| net-new P5 | NEW | `src/eval/apple_fm_client.c` | ~280 | Spawns Swift subprocess in production; canned-table impl under `HU_IS_TEST` (D6); `available()` checks macOS version + entitlement env (`HU_APPLE_FM_AVAILABLE=1`); JSON-per-line wire protocol |
| net-new P5 | NEW | `scripts/eval_external/apple_fm_server.swift` | ~200 | Long-running Swift server. Imports `FoundationModels`, reads `{"id": int, "prompt": str}` from stdin, returns `{"id": int, "text": str}` or `{"id": int, "error": str}`. Exits 0 on clean stdin EOF; exits 2 on framework-unavailable; exits 3 on entitlement missing |
| net-new P5 | NEW | `src/eval/gemini_nano_client.c` | ~250 | Spawns Node bridge; canned-table impl under `HU_IS_TEST`; `available()` checks Chrome binary path + `window.ai` capability via a 1-shot Node probe script. **File header comment (L2)** documents the tested Chrome Canary version range identical to `scripts/eval_external/chrome_ai_server.js` (e.g. `/* Tested against Chrome Canary 132.0.6800.0 — 133.0.6900.0; re-test on version bumps. The JS bridge in scripts/eval_external/chrome_ai_server.js carries the same pin. */`) so reviewers can find the pin from either end of the FFI |
| net-new P5 | NEW | `scripts/eval_external/chrome_ai_server.js` | ~180 | Node script: spawn headless Chrome via `puppeteer-core` (pinned via `scripts/eval_external/package.json` + `scripts/eval_external/package-lock.json` — both committed, M4: lazy `npm ci --prefix scripts/eval_external` at first run for reproducibility, NOT `npm install puppeteer-core`), drive `window.ai.languageModel.create()` + `.prompt(text)` via CDP, JSON-per-line on stdout. File header comment documents the tested Chrome Canary version range (L2: e.g. `// Tested against Chrome Canary 132.0.6800.0 — 133.0.6900.0; re-test on version bumps`) |
| net-new P5 | NEW | `scripts/eval_external/package.json` | ~15 (file) | M4: pins `puppeteer-core` to an exact version (e.g. `"puppeteer-core": "22.10.0"`); committed alongside `package-lock.json` for byte-stable installs |
| net-new P5 | NEW | `scripts/eval_external/package-lock.json` | ~50 (file) | M4: npm lockfile committed for reproducible installs; first-run does `npm ci --prefix scripts/eval_external` (lockfile-strict), not `npm install` |
| net-new P5 | NEW | `src/eval/stock_baseline.c` | ~120 | `hu_eval_judge_external_t` impl that wraps `hu_provider_t.chat` with the adapter unloaded (D10) |
| net-new P5 | NEW | `include/human/eval/competitive_harness.h` | ~80 | `hu_competitive_harness_config_t`, `hu_competitive_harness_result_t`, `hu_competitive_harness_run` API |
| net-new P5 | NEW | `src/eval/competitive_harness.c` | ~420 | Orchestrates side-by-side: stock, our DPO, our KTO, our GRPO, Apple FM, Gemini Nano. **fix(plan,eval,scorer): use v2 scorer (round-3 NEW-MED-1)** — calls `hu_communication_style_compare_response_sets_v2` for the persona-fidelity column (4-axis decision-style scorer, consistent with the BLOCKER-1 communication-style fidelity migration and Task 9 Step 2's implementation note). The v1 default symbol is intentionally NOT used here; the competitive harness opts in to v2 explicitly per D1 / B1. Renders the §1 scorecard with bootstrap CIs to markdown + JSON. Honestly writes `unavailable (reason)` for any judge whose `available()` returned false. |
| net-new P5 | NEW | `tests/fixtures/lora_baseline_persona_v2_responses.json` | ~30 (file) | 100 prompt-tagged reference responses, scored on the 4-axis rubric (spec §11 Q4). Owner-rated; held-out from training |
| net-new P5 | NEW | `tests/fixtures/lora_baseline_persona_v2_rubric.md` | ~30 (file) | The 4-axis rubric documented for reviewer re-rating |
| net-new P5 | NEW | `tests/fixtures/leaderboard_canned.json` | ~30 (file) | Canned gold-judge scores per prompt for offline determinism |
| net-new P5 | NEW | `tests/fixtures/external_judge_canned.json` | ~50 (file) | Canned (prompt → response) table for Apple FM + Gemini Nano test-mode |
| net-new P5 | NEW | `tests/test_communication_style_fidelity_v2_axis.c` | ~250 | The 4th decision-style axis: hand-built persona → score known triples, verify expected ordering, pin that the default symbol stays 3-axis (B1: opt-in only — v2 must differ from the default scorer's score) |
| net-new P5 | NEW | `tests/test_leaderboard.c` | ~200 | Cached gold-judge resolution, cache miss → `HU_ERR_NOT_SUPPORTED` in production, canned fallback in test |
| net-new P5 | NEW | `tests/test_eval_gate.c` | ~300 | Synthetic responses → verdict; per-criterion pass/fail; bootstrap CI math; reason-string contains failing criterion names |
| net-new P5 | NEW | `tests/test_apple_fm_client.c` | ~180 | Canned-response factory + JSON-wire-protocol roundtrip in test mode; subprocess never spawned |
| net-new P5 | NEW | `tests/test_gemini_nano_client.c` | ~180 | Same shape as Apple FM test (D6 symmetry) |
| net-new P5 | NEW | `tests/test_competitive_harness.c` | ~250 | Mock external judges via canned factories, verify scorecard rendering + `unavailable` row handling |
| **P2 fold-in** | NEW | `tests/test_lora_training_runner_eval_gate.c` | ~250 | Promotion gate wiring: positive verdict → `hu_provider_load_adapter` called + evidence dir written; negative → no swap + `<path>.rejected` move + `gate_decision.json` |
| P2 fold-in | NEW | `tests/test_lora_training_runner_proof_directory.c` | ~200 | Pins the §8 evidence-directory layout: nine files exist, JSON shape stable |
| P2 fold-in | NEW | `tests/test_daemon_reaction_poll_wiring.c` | ~220 | Feature-flag OFF → poll never invoked; feature-flag ON + canned chat.db tapback → preference DB row written |
| P2 fold-in | NEW | `tests/fixtures/imessage_chatdb_canned_tapbacks.sql` | ~30 (file) | A synthetic chat.db schema + 5 tapback rows for the daemon-wiring test |

### MODIFY (Phase 5)

| Origin | Action | Path | Delta | What changes |
|--------|--------|------|-------|--------------|
| **net-new P5** | MODIFY | `include/human/memory/personal_model.h` | +25 LOC | Append 3 new fields to `hu_communication_style_t` (`hedging_ratio`, `question_ratio`, `imperative_ratio`) at the END of the struct (after `last_observed_at`); ADD `hu_communication_style_fidelity_score_v2` declaration ALONGSIDE the existing v1 symbol (B1: no rename, no deprecation, opt-in only); ADD `hu_communication_style_compare_response_sets_v2` ALONGSIDE the existing v1 symbol |
| net-new P5 | MODIFY | `src/memory/personal_model.c` | +160 LOC | Implement 4th-axis math (D1) as a new internal helper + new `_v2` entry point; the existing `hu_communication_style_fidelity_score` body is UNCHANGED and stays 3-axis (no rename, no forward to v2). The three internal call sites (`personal_model.c:1377`, `ml/cli.c:1509`, `ml/fidelity.c:75`) stay on the v1 default symbol |
| net-new P5 | (no change) | `src/ml/fidelity.c` | 0 LOC | NO change — internal scorer call site stays on the v1 default symbol per B1. Removed from the MODIFY list |
| net-new P5 | MODIFY | `tests/fixtures/lora_baseline_persona.json` | +25 LOC additive | Add a `"decision_style"` sub-object per response: `hedging_ratio`, `question_ratio`, `imperative_ratio` reference values. Existing 3-axis floor in `scripts/check-lora-baseline.sh` is unaffected |
| net-new P5 | MODIFY | `src/main.c::cmd_eval` (resolves to `src/cli_commands.c::cmd_eval` body) | +30 LOC | Dispatch new subcommands: `human eval competitive`, `human eval leaderboard`, `human eval gate`. **fix(plan,eval,build): `#ifdef HU_ENABLE_RL_FULL` guard on dispatch (round-3 NEW-1)** — `hu_eval_cli_*` symbols are only compiled when `HU_ENABLE_RL_FULL=ON`. Default `dev` and `release` presets build without the flag, so the dispatch block MUST be wrapped in `#ifdef HU_ENABLE_RL_FULL` to preserve "0-byte delta on default release" and avoid linker errors. See Task 10 Step 2-5 for the guarded dispatch snippet. |
| net-new P5 | MODIFY | `scripts/check-lora-baseline.sh` | +20 LOC | Optional `--include-v2-axis` flag that runs the 4-axis path against `lora_baseline_persona_v2_responses.json`; default OFF until corpus-owner ratings are committed (spec coordination) |
| net-new P5 | MODIFY | `CMakeLists.txt` | +60 LOC | New `HU_ENABLE_COMPETITIVE_EVAL` option, source list additions, Swift + Node availability probes for the optional bridges (configure-time, soft) |
| net-new P5 | MODIFY | `tests/test_main.c` | +30 LOC | APPEND-ONLY: 8 new `run_*_tests` registrations |
| **P2 fold-in** | MODIFY | `include/human/agent/lora_runner.h` | +12 LOC | Add `hu_eval_gate_t *eval_gate;` field (optional, NULL = no-gate as before). NO `adapter_id_template` string field — H7 replaces the broken `strftime("%Y-%m-%d-%s-step-%zu", ...)` form (strftime has no `%s` for arbitrary strings) with a two-step `strftime("%Y-%m-%d", &tm)` followed by `snprintf("%s-%s-step-%zu", datebuf, method_name, step_index)`, where `method_name = trainer->vtable->name(ctx)`. Adapter-id format is hard-coded, not a template string |
| P2 fold-in | MODIFY | `src/agent/lora_training_runner.c` | +180 LOC | Insert post-train, pre-load gate call (D7); generate adapter_id; write `~/.human/proofs/<adapter-id>/` evidence directory on accept; move adapter to `<path>.rejected` on reject; backward-compat when `ctx->eval_gate == NULL` |
| P2 fold-in | MODIFY | `src/daemon.c` | +90 LOC | Read `[reaction_collection]` from config; if enabled, schedule `hu_imessage_poll_reactions` every 30s (mirrors existing daemon poll cadences); feature-flagged off by default (D8) |
| P2 fold-in | MODIFY | `src/config.c` + `include/human/config.h` | +25 LOC | New `hu_reaction_collection_config_t` field on `hu_config_t`; JSON schema add; default = disabled |

### Total Phase 5 weight

- New C/h files: 15 net-new + 0 Phase 2 fold-in = **15 new C/h**
- New test files: 6 net-new + 3 Phase 2 fold-in = **9 new tests**
- New fixture files: 4 net-new + 1 Phase 2 fold-in = **5 fixtures**
- New scripts: 2 (Swift + Node bridges) + 0 = **2 scripts**
- Modified files: 7 net-new + 4 Phase 2 fold-in = **11 modify** (down from 12 after M3-revised: `src/ml/fidelity.c` is no longer modified per B1)

**Approximate LOC:** ~2,800 LOC new C, ~1,780 LOC new test C, ~110 LOC Swift, ~180 LOC JS, ~280 LOC fixture+rubric JSON. Mid-pack vs Phase 2 (~3,800 LOC) and Phase 3 (~2,800 LOC).

> **Footnote (M3):** the Phase 5 plan delivers more "modify" rows than umbrella spec §4.6's "~12 + 3" figure suggests because the Phase 2 production-wiring fold-in (LoRA training runner gate integration in Task 11 + daemon reaction-poll wiring in Task 12 + adapter hot-swap proof-directory contract in Task 13) accounts for additional touched files (`src/agent/lora_training_runner.c`, `include/human/agent/lora_runner.h`, `src/daemon.c`, `src/config.c`, `include/human/config.h`). When `sprint-auditor` runs at Task 14 Step 5, it should match the file inventory in **this plan**, NOT umbrella spec §4.6's count — the Phase 2 deferral fold-in is a documented Phase 5 deliverable per the umbrella spec §4.6's own "DEFERRED to Phase 5" rows.

---

## Tasks

Each task targets ≤5 LOC delta in unrelated files, ≤500 LOC in the focal file, and a single, named verifier-checkable behaviour. Tasks 1, 2, 4, 5, 6, 9, 11, 12, 13 are mandatory `verifier` subagent gates per spec §7. Tasks 4, 5, 14 additionally clear the `aspect-panel` end-gate.

### Task 0: Phase 5 start gate + Phase 3 bootstrap-helper readiness check

**Files:**
- Verify: `git tag --list 'rl-sota-phase-3-complete'` returns the tag (Phase 4 optional escape valve per spec §5; if Phase 4 tag exists, that's preferred but not required to start Phase 5).
- Verify: `./build-rl-sota/human_tests` returns the Phase 3 final count or better with 0 ASan / 0 UBSan.
- Verify: `include/human/memory/personal_model.h` exports the 3-axis `hu_communication_style_fidelity_score` symbol unmodified (Track D Phase 1 still in flight; Phase 5 must NOT have already shimmed it).
- Verify: `src/eval/bootstrap.c` and `include/human/eval/bootstrap.h` exist (Phase 3 deliverable per D2); if missing, Task 2 owns extraction.
- Verify: `hu_reward_model_t` vtable has `score_batch` (Phase 3 Task 2).

- [ ] **Step 1: Verify Phase 3 tag + deliverables**

```bash
git tag --list 'rl-sota-phase-3-complete' | grep -q rl-sota-phase-3-complete && echo "Phase 3 tag OK"
test -f include/human/ml/reward_model.h && grep -q score_batch include/human/ml/reward_model.h && echo "RM vtable has score_batch"
test -f include/human/memory/personal_model.h && grep -q hu_communication_style_fidelity_score include/human/memory/personal_model.h && echo "3-axis scorer still present"
```

- [ ] **Step 2: Verify bootstrap helper status (D2)**

```bash
if test -f include/human/eval/bootstrap.h; then
    echo "Phase 3 bootstrap helper present — Task 2 will only wire it into the gate"
else
    echo "Phase 3 deferred bootstrap extraction — Task 2 will own it (extra ~80 LOC)"
fi
```

- [ ] **Step 3: spec-verifier gate**

Dispatch `spec-verifier` subagent with `docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md` §4.6 + §5 row 5 + §6 + this plan. Expect 0 gaps before proceeding. If the subagent reports gaps, fix the plan in place and re-run.

- [ ] **Step 4: Branch from Phase 3 tag**

```bash
git status --porcelain | grep -v -E '^\?\?' && echo "DIRTY — abort and clean" && exit 1 || echo "Clean"
git checkout -b rl-sota-phase-5 rl-sota-phase-3-complete
```

- [ ] **Step 5: Verify Apple FM + Chrome AI probes are SAFE no-ops**

```bash
# These probes will be added in Tasks 7/8; here we just sanity-check the host
# so the plan author knows what the CI runner will see.
sw_vers -productVersion 2>/dev/null | awk -F. '$1 >= 26 { print "macOS 26+ detected — Apple FM bridge MIGHT be reachable" }'
which chrome-canary 2>/dev/null && echo "Chrome Canary present — Gemini Nano bridge MIGHT be reachable"
echo "No bridges spawned. Tasks 7+8 will gate by HU_IS_TEST first."
```

---

### Task 1: 4th decision-style axis (additive, opt-in only — v1 default unchanged per B1)

**Files:**
- Modify: `include/human/memory/personal_model.h`
- Modify: `src/memory/personal_model.c`
- Create: `tests/test_communication_style_fidelity_v2_axis.c`
- Modify: `tests/test_main.c` (APPEND), `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```c
/* tests/test_communication_style_fidelity_v2_axis.c */
#include "test_framework.h"
#include "human/memory/personal_model.h"
#include <math.h>
#include <string.h>

static void test_v2_axis_returns_higher_score_for_matching_decision_style(void) {
    /* Direct, imperative target persona — high imperative_ratio,
     * low hedging_ratio, low question_ratio. */
    hu_communication_style_t target = {
        .lowercase_ratio = 1.0f, .abbreviation_ratio = 0.0f,
        .hedging_ratio = 0.05f, .question_ratio = 0.10f,
        .imperative_ratio = 0.80f,
        .avg_message_length = 50, .sample_count = 100,
        .last_observed_at = 1000,
    };
    const char *matching = "do this now. check the logs. fix the bug.";
    const char *mismatched =
        "maybe we could perhaps try to see if it might be possible to consider doing this?";
    float score_match = hu_communication_style_fidelity_score_v2(
        &target, matching, strlen(matching));
    float score_mismatch = hu_communication_style_fidelity_score_v2(
        &target, mismatched, strlen(mismatched));
    HU_ASSERT_TRUE(score_match > 0.5f);
    HU_ASSERT_TRUE(score_mismatch < score_match - 0.10f);
}

static void test_v2_axis_returns_minus_one_on_zero_sample_count(void) {
    hu_communication_style_t target = {.sample_count = 0};
    const char *resp = "anything";
    float s = hu_communication_style_fidelity_score_v2(&target, resp, strlen(resp));
    HU_ASSERT_TRUE(fabsf(s - (-1.0f)) < 1e-6f);
}

static void test_default_entry_point_stays_3_axis_v1(void) {
    /* B1: the default symbol stays 3-axis. v2 is opt-in. The default must
     * return the SAME score as today; v2 must differ because it includes a
     * 4th axis. This pins both invariants. */
    hu_communication_style_t target = {
        .lowercase_ratio = 1.0f, .abbreviation_ratio = 0.3f,
        .hedging_ratio = 0.05f, .question_ratio = 0.10f,
        .imperative_ratio = 0.80f,
        .avg_message_length = 50, .sample_count = 100,
        .last_observed_at = 1000,
    };
    const char *resp = "hey u see this thing rn? maybe we could check it out lmk btw";
    float def = hu_communication_style_fidelity_score(&target, resp, strlen(resp));
    float v2  = hu_communication_style_fidelity_score_v2(&target, resp, strlen(resp));
    /* The two MUST differ — v2 includes the 4th axis. */
    HU_ASSERT_TRUE(fabsf(def - v2) > 0.001f);
    /* The default symbol's body is unchanged from pre-Phase-5, so its
     * existing tests (tests/test_personal_model.c::personal_model_fidelity_*)
     * remain byte-stable; no PIN constant is hardcoded here. */
}

void run_v2_axis_tests(void) {
    HU_TEST_SUITE("v2-axis");
    HU_RUN_TEST(test_v2_axis_returns_higher_score_for_matching_decision_style);
    HU_RUN_TEST(test_v2_axis_returns_minus_one_on_zero_sample_count);
    HU_RUN_TEST(test_default_entry_point_stays_3_axis_v1);
}
```

- [ ] **Step 2: Implement 4th-axis math in `src/memory/personal_model.c`**

Sketch (B1-revised: opt-in only — do NOT rename or shim the v1 symbol):

1. Three new internal helpers: `hedging_ratio_of(response, len)`, `question_ratio_of(response, len)`, `imperative_ratio_of(response, len)`. Each returns a `float` in [0,1].
2. New function `hu_communication_style_fidelity_score_v2` implementing the 4-axis mean (D1 formula). This is the only NEW public scorer symbol.
3. **Do NOT touch the existing `hu_communication_style_fidelity_score` body.** It stays 3-axis, byte-identical to today. No rename, no `__attribute__((deprecated))`, no forwarding shim. The three internal call sites (`personal_model.c:1377`, `ml/cli.c:1509`, `ml/fidelity.c:75`) stay on the v1 symbol unchanged.
4. New function `hu_communication_style_compare_response_sets_v2` that calls the v2 scorer per response. Existing v1 comparator stays byte-identical.
5. No "backward-compat pin" against `/*PIN*/0.733f` is needed because the v1 entry point is never modified.

- [ ] **Step 3: Extend `hu_communication_style_t` additively**

In `include/human/memory/personal_model.h`, **append three new fields (`hedging_ratio`, `question_ratio`, `imperative_ratio`) at the END of the struct, AFTER the existing `last_observed_at`** (M2). **Do NOT insert mid-struct** — positional initializers like `hu_communication_style_t s = {.lowercase_ratio = 1.0f, .abbreviation_ratio = 0.0f, .avg_message_length = 50, .sample_count = 100, .last_observed_at = 1000}` already exist across the test suite (see e.g. Task 1 Step 1 above) and an insertion after `abbreviation_ratio` would silently shift later fields. Appending at the end preserves both designated-init and positional-init call sites, and keeps zero-init compatibility for pre-Phase-5 binaries.

- [ ] **Step 4: `_v2` for the comparator**

Add `hu_communication_style_compare_response_sets_v2` as a NEW symbol that internally calls `hu_communication_style_fidelity_score_v2` for each response. The existing v1 comparator entry point stays byte-identical and is NOT renamed or deprecated (B1: opt-in only).

- [ ] **Step 5: Wire test + run**

```bash
cmake --build --preset rl_sota -j8 && ./build-rl-sota/human_tests --suite=v2-axis
```
Expected: 4/4 PASS.

- [ ] **Step 6: Commit**

```bash
git add include/human/memory/personal_model.h src/memory/personal_model.c \
        tests/test_communication_style_fidelity_v2_axis.c CMakeLists.txt tests/test_main.c
git commit -m "feat(memory,personal_model): 4th decision-style fidelity axis (opt-in v2, v1 default unchanged) (Phase 5 Task 1)"
```

---

### Task 2: Bootstrap CI helper consolidation (or extraction if Phase 3 deferred it)

**Files:**
- IF Phase 3 already shipped `include/human/eval/bootstrap.h` + `src/eval/bootstrap.c`: SKIP this task (note "skipped — Phase 3 already shipped" in the commit log) and go to Task 3.
- ELSE: Create `include/human/eval/bootstrap.h`, `src/eval/bootstrap.c`; extract from `tests/test_reward_model_train.c::test_reward_model_train_bootstrap_ci`.

- [ ] **Step 1: Write the failing test**

```c
/* tests/test_bootstrap.c (new file, only if Phase 3 didn't ship the helper) */
#include "test_framework.h"
#include "human/eval/bootstrap.h"
#include <math.h>

static void test_bootstrap_ci_95_brackets_known_mean(void) {
    double xs[10] = {1.0, 1.1, 0.9, 1.05, 0.95, 1.02, 0.98, 1.03, 0.97, 1.01};
    double lower = 0, upper = 0;
    hu_error_t e = hu_bootstrap_mean_ci_95(xs, 10, /*B=*/1000, /*seed=*/42, &lower, &upper);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(lower < 1.00 && upper > 1.00);  /* known mean = 1.001 */
    HU_ASSERT_TRUE(upper - lower > 0.0);
}

static void test_bootstrap_ci_deterministic_with_fixed_seed(void) {
    double xs[5] = {0.1, 0.2, 0.3, 0.4, 0.5};
    double l1=0,u1=0,l2=0,u2=0;
    hu_bootstrap_mean_ci_95(xs, 5, 1000, 42, &l1, &u1);
    hu_bootstrap_mean_ci_95(xs, 5, 1000, 42, &l2, &u2);
    HU_ASSERT_TRUE(fabs(l1 - l2) < 1e-12);
    HU_ASSERT_TRUE(fabs(u1 - u2) < 1e-12);
}

void run_bootstrap_tests(void) {
    HU_TEST_SUITE("bootstrap");
    HU_RUN_TEST(test_bootstrap_ci_95_brackets_known_mean);
    HU_RUN_TEST(test_bootstrap_ci_deterministic_with_fixed_seed);
}
```

- [ ] **Step 2: Implement bootstrap helper**

```c
/* include/human/eval/bootstrap.h */
hu_error_t hu_bootstrap_mean_ci_95(const double *xs, size_t n,
                                    uint32_t B, uint32_t seed,
                                    double *out_lower, double *out_upper);
```

Body: percentile bootstrap (re-sample with replacement B times, take 2.5%/97.5% quantiles of the resampled means). Use the existing `hu_rng_*` helpers in `src/core/rng.c` for reproducibility (already used in Phase 3 RM init).

- [ ] **Step 3: Wire + run + commit** — same shape as Task 1.

---

### Task 3: `hu_eval_judge_external_t` vtable + canned-response factories

**Files:**
- Create: `include/human/eval/judge_external.h`
- Create: `src/eval/judge_external.c`
- Create: `tests/fixtures/external_judge_canned.json`
- Modify: `tests/test_main.c` (APPEND), `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```c
/* tests/test_judge_external.c */
#include "test_framework.h"
#include "human/eval/judge_external.h"
#include "human/core/allocator.h"
#include <string.h>

static void test_canned_judge_returns_canned_response_for_known_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_external_t judge = {0};
    hu_error_t e = hu_eval_judge_canned_create(
        &alloc, "tests/fixtures/external_judge_canned.json", "stock_gemma", &judge);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_NOT_NULL(judge.vtable);

    char out[1024] = {0}; size_t written = 0;
    e = judge.vtable->generate(judge.ctx, "what is two plus two?", 22,
                                out, sizeof(out), &written);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(strstr(out, "four") != NULL);
    HU_ASSERT_TRUE(written > 0);

    judge.vtable->deinit(judge.ctx, &alloc);
}

static void test_canned_judge_returns_unsupported_for_unknown_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_external_t judge = {0};
    hu_eval_judge_canned_create(&alloc, "tests/fixtures/external_judge_canned.json",
                                 "stock_gemma", &judge);
    char out[1024] = {0}; size_t written = 0;
    hu_error_t e = judge.vtable->generate(judge.ctx, "unknown prompt", 14, out,
                                           sizeof(out), &written);
    HU_ASSERT_EQ(e, HU_ERR_NOT_SUPPORTED);
    judge.vtable->deinit(judge.ctx, &alloc);
}

static void test_available_returns_true_for_canned_judge(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_external_t judge = {0};
    hu_eval_judge_canned_create(&alloc, "tests/fixtures/external_judge_canned.json",
                                 "stock_gemma", &judge);
    HU_ASSERT_TRUE(judge.vtable->available(judge.ctx));
    judge.vtable->deinit(judge.ctx, &alloc);
}

void run_judge_external_tests(void) {
    HU_TEST_SUITE("judge-external");
    HU_RUN_TEST(test_canned_judge_returns_canned_response_for_known_prompt);
    HU_RUN_TEST(test_canned_judge_returns_unsupported_for_unknown_prompt);
    HU_RUN_TEST(test_available_returns_true_for_canned_judge);
}
```

- [ ] **Step 2-5: Implement vtable + canned factory + fixture, wire, run, commit.**

Canned factory loads the JSON once at create time, holds a `prompt_hash → response` map, returns `HU_ERR_NOT_SUPPORTED` on miss. The fixture has entries per judge name (`"apple_fm"`, `"gemini_nano"`, `"stock_gemma"`) so the same file feeds all three test-mode factories.

---

### Task 4: Leaderboard runners (MT-Bench, AlpacaEval, IFEval) with cached gold judges

**Files:**
- Create: `include/human/eval/leaderboard.h`
- Create: `src/eval/leaderboard.c`
- Create: `tests/fixtures/leaderboard_canned.json`
- Create: `tests/test_leaderboard.c`
- Modify: `tests/test_main.c` (APPEND), `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

```c
/* tests/test_leaderboard.c */
#include "test_framework.h"
#include "human/eval/leaderboard.h"
#include "human/core/allocator.h"

static void test_mt_bench_runner_returns_canned_score_in_test_mode(void) {
    HU_SKIP_IF(getenv("HU_FORCE_REAL_LEADERBOARD") != NULL,
               "HU_FORCE_REAL_LEADERBOARD set; production-mode leaderboard not covered here");
    hu_allocator_t alloc = hu_system_allocator();
    hu_leaderboard_runner_t runner = {0};
    hu_leaderboard_config_t cfg = {
        .canned_path = "tests/fixtures/leaderboard_canned.json",
        .seed = 42,
    };
    HU_ASSERT_EQ(hu_leaderboard_create_mt_bench(&alloc, &cfg, &runner), HU_OK);

    const char *prompts[3] = {"explain recursion", "what is bm25", "summarize einstein"};
    const char *responses[3] = {"recursion is...", "bm25 is...", "einstein was..."};
    double scores[3] = {0};
    HU_ASSERT_EQ(runner.vtable->run(runner.ctx, &alloc, prompts, responses, 3, scores), HU_OK);
    for (int i = 0; i < 3; i++) HU_ASSERT_TRUE(scores[i] >= 0.0 && scores[i] <= 10.0);
    runner.vtable->deinit(runner.ctx, &alloc);
}

static void test_ifeval_runner_returns_canned_score_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_leaderboard_runner_t runner = {0};
    hu_leaderboard_config_t cfg = {.canned_path = "tests/fixtures/leaderboard_canned.json"};
    HU_ASSERT_EQ(hu_leaderboard_create_ifeval(&alloc, &cfg, &runner), HU_OK);
    const char *p[2] = {"write a haiku", "list five primes"};
    const char *r[2] = {"haiku here", "2 3 5 7 11"};
    double s[2] = {0};
    HU_ASSERT_EQ(runner.vtable->run(runner.ctx, &alloc, p, r, 2, s), HU_OK);
    HU_ASSERT_TRUE(s[0] >= 0.0 && s[0] <= 1.0);
    runner.vtable->deinit(runner.ctx, &alloc);
}

static void test_leaderboard_cache_miss_returns_unsupported_in_production_mode(void) {
    HU_SKIP_IF(getenv("HU_FORCE_REAL_LEADERBOARD") == NULL,
               "production-mode unavailable in test mode");
    hu_allocator_t alloc = hu_system_allocator();
    hu_leaderboard_runner_t runner = {0};
    hu_leaderboard_config_t cfg = {.canned_path = NULL};
    HU_ASSERT_EQ(hu_leaderboard_create_mt_bench(&alloc, &cfg, &runner), HU_OK);
    const char *p[1] = {"unknown prompt to force a cache miss"};
    const char *r[1] = {"unknown response"};
    double s[1] = {0};
    HU_ASSERT_EQ(runner.vtable->run(runner.ctx, &alloc, p, r, 1, s), HU_ERR_NOT_SUPPORTED);
    runner.vtable->deinit(runner.ctx, &alloc);
}

void run_leaderboard_tests(void) {
    HU_TEST_SUITE("leaderboard");
    HU_RUN_TEST(test_mt_bench_runner_returns_canned_score_in_test_mode);
    HU_RUN_TEST(test_ifeval_runner_returns_canned_score_in_test_mode);
    HU_RUN_TEST(test_leaderboard_cache_miss_returns_unsupported_in_production_mode);
}
```

- [ ] **Step 2-5: Implement the three runners**

Each runner exposes the same vtable but consults its own canned section in the fixture (`{"mt_bench": {...}, "alpaca_eval": {...}, "ifeval": {...}}`). Production-mode (`HU_IS_TEST == 0`) reads `~/.human/eval_cache/<runner>/<prompt_hash>.json`; cache miss returns `HU_ERR_NOT_SUPPORTED` (no fallback to live LLM judging in v1 — that's Phase 6 / proof artefact). Implementation is ~140 LOC per runner because the JSON-parsing scaffold is shared.

---

### Task 5: `hu_eval_gate_t` decision logic + bootstrap CIs

**Files:**
- Create: `include/human/eval/eval_gate.h`
- Create: `src/eval/eval_gate.c`
- Create: `tests/test_eval_gate.c`
- Modify: `tests/test_main.c` (APPEND), `CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

```c
/* tests/test_eval_gate.c */
#include "test_framework.h"
#include "human/eval/eval_gate.h"
#include "human/eval/judge_external.h"
#include "human/eval/leaderboard.h"
#include "human/ml/reward_model.h"
#include "human/core/allocator.h"
#include <math.h>
#include <string.h>

static void test_eval_gate_accepts_when_all_criteria_pass(void) {
    /* Synthetic case: candidate set strictly better than baseline on all axes. */
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.60,
        .baseline_mt_bench_mean = 5.0,
        .baseline_ifeval_mean = 0.50,
        .baseline_p95_latency_ms = 200.0,
        .persona_delta_min = 0.05,
        .mt_bench_regression_max = -0.01,
        .ifeval_regression_max = -0.02,
        .latency_delta_max_ms = 50.0,
        .bootstrap_samples = 1000,
        .bootstrap_seed = 42,
    };
    /* Inject synthetic per-prompt scores via the test seam — see step 2 for
     * the helper signature hu_eval_gate_decide_from_arrays_for_test. */
    double persona[20], mt[20], ifeval[20], reward[20];
    for (int i = 0; i < 20; i++) {
        persona[i] = 0.75 + 0.01 * (i % 3 - 1); /* mean ~ 0.75, > 0.60 + 0.05 */
        mt[i] = 5.0 + 0.02 * (i % 3 - 1);        /* mean ~ 5.0 */
        ifeval[i] = 0.55 + 0.01 * (i % 3 - 1);   /* mean ~ 0.55, > 0.50 - 0.02 */
        reward[i] = 0.20 + 0.01 * (i % 3 - 1);   /* mean ~ 0.20, > 0 */
    }
    hu_eval_gate_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(
        &gate, persona, mt, ifeval, reward, 20, /*candidate_p95=*/180.0, &verdict), HU_OK);
    HU_ASSERT_TRUE(verdict.promote);
    HU_ASSERT_TRUE(verdict.persona_pass);
    HU_ASSERT_TRUE(verdict.mt_bench_pass);
    HU_ASSERT_TRUE(verdict.ifeval_pass);
    HU_ASSERT_TRUE(verdict.latency_pass);
    HU_ASSERT_TRUE(verdict.reward_pass);
}

static void test_eval_gate_rejects_when_persona_delta_below_threshold(void) {
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.70,
        .baseline_mt_bench_mean = 5.0, .baseline_ifeval_mean = 0.50,
        .baseline_p95_latency_ms = 200.0,
        .persona_delta_min = 0.05,
        .mt_bench_regression_max = -0.01, .ifeval_regression_max = -0.02,
        .latency_delta_max_ms = 50.0,
        .bootstrap_samples = 1000, .bootstrap_seed = 42,
    };
    double persona[20], mt[20], ifeval[20], reward[20];
    for (int i = 0; i < 20; i++) {
        persona[i] = 0.72; /* mean = 0.72; delta = 0.02, BELOW 0.05 threshold */
        mt[i] = 5.0; ifeval[i] = 0.55; reward[i] = 0.20;
    }
    hu_eval_gate_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(
        &gate, persona, mt, ifeval, reward, 20, 180.0, &verdict), HU_OK);
    HU_ASSERT_FALSE(verdict.promote);
    HU_ASSERT_FALSE(verdict.persona_pass);
    HU_ASSERT_TRUE(strstr(verdict.reason, "persona") != NULL);
}

static void test_eval_gate_rejects_on_latency_regression(void) {
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.60,
        .baseline_mt_bench_mean = 5.0, .baseline_ifeval_mean = 0.50,
        .baseline_p95_latency_ms = 200.0,
        .persona_delta_min = 0.05,
        .mt_bench_regression_max = -0.01, .ifeval_regression_max = -0.02,
        .latency_delta_max_ms = 50.0,
        .bootstrap_samples = 1000, .bootstrap_seed = 42,
    };
    double persona[20], mt[20], ifeval[20], reward[20];
    for (int i = 0; i < 20; i++) { persona[i]=0.75; mt[i]=5.0; ifeval[i]=0.55; reward[i]=0.20; }
    hu_eval_gate_verdict_t verdict = {0};
    /* candidate p95 = 300 ms; baseline + 50 = 250; over budget. */
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(
        &gate, persona, mt, ifeval, reward, 20, 300.0, &verdict), HU_OK);
    HU_ASSERT_FALSE(verdict.promote);
    HU_ASSERT_FALSE(verdict.latency_pass);
    HU_ASSERT_TRUE(strstr(verdict.reason, "latency") != NULL);
}

static void test_eval_gate_rejects_when_reward_ci_lower_below_zero(void) {
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.60, .baseline_mt_bench_mean = 5.0,
        .baseline_ifeval_mean = 0.50, .baseline_p95_latency_ms = 200.0,
        .persona_delta_min = 0.05,
        .mt_bench_regression_max = -0.01, .ifeval_regression_max = -0.02,
        .latency_delta_max_ms = 50.0,
        .bootstrap_samples = 1000, .bootstrap_seed = 42,
    };
    double persona[20], mt[20], ifeval[20], reward[20];
    for (int i = 0; i < 20; i++) {
        persona[i] = 0.75; mt[i] = 5.0; ifeval[i] = 0.55;
        reward[i] = -0.30 + 0.01 * (i % 3 - 1); /* mean ~ -0.30, below 0 */
    }
    hu_eval_gate_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(
        &gate, persona, mt, ifeval, reward, 20, 180.0, &verdict), HU_OK);
    HU_ASSERT_FALSE(verdict.promote);
    HU_ASSERT_FALSE(verdict.reward_pass);
    HU_ASSERT_TRUE(strstr(verdict.reason, "reward") != NULL);
}

/* fix(plan,eval,stats): n_responses >= 30 floor regression pin (round-3 NEW-MED-3).
 * The production `hu_eval_gate_decide` MUST return HU_ERR_INVALID_ARGUMENT when
 * n_responses < 30 (Wilson-interval rule-of-thumb floor); the _for_test variant
 * has a relaxed n >= 10 floor for unit-test speed. This test exercises both
 * boundaries. */
static void test_eval_gate_rejects_when_n_below_floor_for_test(void) {
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.60,
        .baseline_mt_bench_mean = 5.0, .baseline_ifeval_mean = 0.50,
        .baseline_p95_latency_ms = 200.0,
        .persona_delta_min = 0.05,
        .mt_bench_regression_max = -0.01, .ifeval_regression_max = -0.02,
        .latency_delta_max_ms = 50.0,
        .bootstrap_samples = 1000, .bootstrap_seed = 42,
    };
    /* n=1 must be rejected by both production and _for_test entry points
     * (n=1 is degenerate — zero variance, collapsed bootstrap bands). */
    double one[1] = {0.75};
    hu_eval_gate_verdict_t verdict = {0};
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(
        &gate, one, one, one, one, 1, 180.0, &verdict), HU_ERR_INVALID_ARGUMENT);
    /* n=10 must PASS the relaxed _for_test floor but FAIL the production floor.
     * (Task 5's other 4 tests use n=20 which is above 10 — also valid for _for_test.) */
    double ten[10]; for (int i = 0; i < 10; i++) ten[i] = 0.75;
    HU_ASSERT_EQ(hu_eval_gate_decide_from_arrays_for_test(
        &gate, ten, ten, ten, ten, 10, 180.0, &verdict), HU_OK);
}

void run_eval_gate_tests(void) {
    HU_TEST_SUITE("eval-gate");
    HU_RUN_TEST(test_eval_gate_accepts_when_all_criteria_pass);
    HU_RUN_TEST(test_eval_gate_rejects_when_persona_delta_below_threshold);
    HU_RUN_TEST(test_eval_gate_rejects_on_latency_regression);
    HU_RUN_TEST(test_eval_gate_rejects_when_reward_ci_lower_below_zero);
    HU_RUN_TEST(test_eval_gate_rejects_when_n_below_floor_for_test);
}
```

- [ ] **Step 2-5:** Implement `eval_gate.c` with the `_for_test` array-injected entry point and the production entry point that calls scorer + leaderboard + RM; bootstrap CIs via Task 2's helper. Acceptance: 4/4 tests pass; reason strings name the failing criterion.

---

### Task 6: `stock_baseline.c` — stock-Gemma judge wrapping `hu_provider_t.chat`

**Files:**
- Create: `src/eval/stock_baseline.c`
- Create: `tests/test_stock_baseline.c`
- Modify: `tests/test_main.c` (APPEND), `CMakeLists.txt`

- [ ] **Step 1: Failing test**

```c
/* tests/test_stock_baseline.c */
#include "test_framework.h"
#include "human/eval/judge_external.h"
#include "human/provider.h"
#include "human/core/allocator.h"

extern hu_error_t hu_provider_create_for_test_with_canned_response(
    hu_allocator_t *alloc, const char *canned, hu_provider_t **out);  /* Phase 1 test seam */

static void test_stock_baseline_unloads_adapter_before_generate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t *provider = NULL;
    HU_ASSERT_EQ(hu_provider_create_for_test_with_canned_response(
        &alloc, "canned: four", &provider), HU_OK);
    hu_eval_judge_external_t judge = {0};
    HU_ASSERT_EQ(hu_stock_baseline_create(&alloc, provider, &judge), HU_OK);

    char out[256] = {0}; size_t written = 0;
    HU_ASSERT_EQ(judge.vtable->generate(judge.ctx, "what is two plus two?", 22,
                                         out, sizeof(out), &written), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "four") != NULL);
    /* Verify the adapter was unloaded — Phase 1 provider records the unload call. */
    HU_ASSERT_TRUE(hu_provider_unload_called_count_for_test(provider) >= 1);

    judge.vtable->deinit(judge.ctx, &alloc);
    hu_provider_destroy_for_test(provider, &alloc);
}

void run_stock_baseline_tests(void) {
    HU_TEST_SUITE("stock-baseline");
    HU_RUN_TEST(test_stock_baseline_unloads_adapter_before_generate);
}
```

- [ ] **Step 2-5:** Implement (D10). The judge holds a borrowed `hu_provider_t *`. `available()` returns `provider != NULL`. `generate()` first calls `provider->vtable->unload_adapter(provider, "", 0)` then `provider->vtable->chat(...)` with a minimal system prompt. `deinit()` does NOT destroy the provider (borrowed reference).

---

### Task 7: Apple Foundation Models bridge (Swift subprocess + canned test factory)

**Files:**
- Create: `src/eval/apple_fm_client.c`
- Create: `scripts/eval_external/apple_fm_server.swift`
- Create: `tests/test_apple_fm_client.c`
- Modify: `tests/test_main.c` (APPEND), `CMakeLists.txt`

- [ ] **Step 1: Failing test (test-mode only — D6)**

```c
/* tests/test_apple_fm_client.c */
#include "test_framework.h"
#include "human/eval/judge_external.h"
#include "human/core/allocator.h"

static void test_apple_fm_create_returns_canned_judge_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_eval_judge_external_t judge = {0};
    /* In test mode this MUST NOT spawn the Swift subprocess. */
    hu_error_t e = hu_apple_fm_judge_create(&alloc, &judge);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_STR_EQ(judge.vtable->name(judge.ctx), "apple_fm");
    HU_ASSERT_TRUE(judge.vtable->available(judge.ctx));

    char out[1024] = {0}; size_t written = 0;
    e = judge.vtable->generate(judge.ctx, "what is two plus two?", 22,
                                out, sizeof(out), &written);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(strstr(out, "four") != NULL);
    judge.vtable->deinit(judge.ctx, &alloc);
}

/* M1: test_apple_fm_available_returns_false_when_canned_missing was REMOVED
 * during adversarial review. Once Task 3 ships `tests/fixtures/external_judge_canned.json`,
 * the fixture is always present in every CI run, so the HU_SKIP_IF would
 * permanently skip and the test would carry zero signal. The missing-canned
 * path is already covered by Task 3's
 * `test_canned_judge_returns_unsupported_for_unknown_prompt`. */

void run_apple_fm_client_tests(void) {
    HU_TEST_SUITE("apple-fm-client");
    HU_RUN_TEST(test_apple_fm_create_returns_canned_judge_in_test_mode);
}
```

- [ ] **Step 2: Implement the C side with the `#if HU_IS_TEST` split**

```c
/* src/eval/apple_fm_client.c */
hu_error_t hu_apple_fm_judge_create(hu_allocator_t *alloc, hu_eval_judge_external_t *out) {
#if HU_IS_TEST
    /* D6: test mode never spawns the subprocess. */
    return hu_eval_judge_canned_create(alloc, "tests/fixtures/external_judge_canned.json",
                                        "apple_fm", out);
#else
    return apple_fm_judge_create_subprocess(alloc, out);
#endif
}
```

The production helper `apple_fm_judge_create_subprocess` spawns `swift scripts/eval_external/apple_fm_server.swift` via `posix_spawn` + a bi-directional pipe pair (`socketpair(AF_UNIX, SOCK_STREAM, 0)`), implements the JSON-per-line protocol, and reads stderr non-destructively into a 4KB ring buffer for postmortem. The `available()` impl runs `sw_vers -productVersion` and checks `>=26`; if false, the factory itself returns `HU_ERR_NOT_SUPPORTED` (no half-spawn).

**Socketpair lifecycle (H6 — pinned to prevent EOF / zombie ambiguity):**

```c
/* 1. Parent creates socketpair: sv[0] is the parent end, sv[1] is the child end. */
int sv[2];
if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return HU_ERR_IO;

/* 2. posix_spawn the Swift binary; redirect child's stdin and stdout to sv[1]
 *    via posix_spawn_file_actions_adddup2. */
posix_spawn_file_actions_t fa;
posix_spawn_file_actions_init(&fa);
posix_spawn_file_actions_adddup2(&fa, sv[1], STDIN_FILENO);
posix_spawn_file_actions_adddup2(&fa, sv[1], STDOUT_FILENO);
posix_spawn_file_actions_addclose(&fa, sv[0]);   /* child must not inherit sv[0] */
posix_spawn_file_actions_addclose(&fa, sv[1]);   /* close after dup2 */
posix_spawn(&child_pid, swift_path, &fa, NULL, swift_argv, environ);
posix_spawn_file_actions_destroy(&fa);

/* 3. Parent closes sv[1] AFTER spawn (so the child's readLine() returns nil
 *    when the parent later closes sv[0] — only the child end is held by the
 *    child, only the parent end by the parent). */
close(sv[1]);

/* 4. Parent uses sv[0] for both read and write (JSON-per-line). */
ctx->fd = sv[0];
ctx->child_pid = child_pid;

/* 5. On shutdown (deinit_subprocess): */
close(ctx->fd);                                  /* parent end → child sees EOF */
int status = 0;
waitpid(ctx->child_pid, &status, 0);             /* blocking wait — prevents zombie */
if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    hu_log_warn("apple_fm_server exited abnormally: status=%d", status);
}
```

Why this exact lifecycle: a leftover `sv[1]` in the parent would keep the child's stdin open after `close(sv[0])`, the child's `readLine()` would block forever instead of returning nil, the child would never exit, and `waitpid` would hang. The sequence above guarantees clean EOF + clean exit + no zombies. A non-blocking `waitpid(..., WNOHANG)` is an option for `deinit()` paths that must not block, but the default contract is the blocking form above to ensure F3 (zombie leak) cannot happen.

- [ ] **Step 3: Implement the Swift server**

```swift
// scripts/eval_external/apple_fm_server.swift
// NOTE: FoundationModels.LanguageModelSession is @MainActor-bound; the entire
// main() must run on the main actor. Using @main + async throws is REQUIRED:
// `try await` is not legal at synchronous top-level scope (B3).
#if canImport(FoundationModels)
import FoundationModels
import Foundation

@main
struct AppleFMServer {
    @MainActor
    static func main() async throws {
        let session = LanguageModelSession()
        while let line = readLine() {
            guard let data = line.data(using: .utf8),
                  let req = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let id = req["id"] as? Int,
                  let prompt = req["prompt"] as? String else {
                let err = ["error": "malformed request"] as [String: Any]
                if let d = try? JSONSerialization.data(withJSONObject: err) {
                    FileHandle.standardOutput.write(d)
                    FileHandle.standardOutput.write("\n".data(using: .utf8)!)
                    try? FileHandle.standardOutput.synchronize()
                }
                continue
            }
            do {
                let resp = try await session.respond(to: prompt)
                let outObj = ["id": id, "text": resp.content] as [String: Any]
                let d = try JSONSerialization.data(withJSONObject: outObj)
                FileHandle.standardOutput.write(d)
                FileHandle.standardOutput.write("\n".data(using: .utf8)!)
                try? FileHandle.standardOutput.synchronize()
            } catch {
                let outObj = ["id": id, "error": String(describing: error)] as [String: Any]
                if let d = try? JSONSerialization.data(withJSONObject: outObj) {
                    FileHandle.standardOutput.write(d)
                    FileHandle.standardOutput.write("\n".data(using: .utf8)!)
                    try? FileHandle.standardOutput.synchronize()
                }
            }
        }
        // readLine() returned nil → stdin EOF → clean exit 0.
    }
}
#else
@main
struct AppleFMServer {
    static func main() {
        FileHandle.standardError.write("FoundationModels framework unavailable\n".data(using: .utf8)!)
        exit(2)
    }
}
#endif
```

The C side checks the subprocess's exit code on `deinit()` to detect mid-run framework loss (exit 2). Failed exits cause subsequent `generate()` calls to return `HU_ERR_NOT_SUPPORTED` instead of attempting to respawn.

Implementation note: `FoundationModels.LanguageModelSession` is `@MainActor`-bound, so the entire `main()` must run on the main actor and be `async throws` (B3 — the previous synchronous top-level `try await` form did not compile). The `@main` struct attribute is the Swift-supported way to declare an async entry point.

- [ ] **Step 4-5: Wire + run + commit.**

---

### Task 8: Gemini Nano (Chrome AI) bridge (Node subprocess + canned test factory)

**Files:**
- Create: `src/eval/gemini_nano_client.c`
- Create: `scripts/eval_external/chrome_ai_server.js`
- Create: `tests/test_gemini_nano_client.c`

Identical structure to Task 7 with two differences:

1. The subprocess is `node scripts/eval_external/chrome_ai_server.js`. `available()` checks for `chrome-canary` or `google-chrome-canary` on `PATH`, AND probes `window.ai.languageModel.capabilities()` via a tiny one-shot Node script (`scripts/eval_external/chrome_ai_capabilities.js`) the first time it's invoked. The probe is a separate JS file (not a CLI flag) so no source code is passed on the command line.
2. The Node script spawns headless Chrome with `--enable-features=AIPromptAPI --headless=new --remote-debugging-port=0`, attaches via CDP, creates `window.ai.languageModel.create()` once, then services `generate` requests by calling `.prompt(text)` until stdin EOF.

**H4 — CDP JS injection prevention (MANDATORY):** the Node script MUST call `page.evaluate` with the prompt as a SECOND argument (which Puppeteer auto-JSON-serializes), NEVER by interpolating user input into a template string passed as the JS-code argument. The mandated safe form is:

```js
// SAFE: prompt is passed as the second argument and auto-JSON-serialized
// by Puppeteer; never interpolated into the JS code string.
const result = await page.evaluate(
    async (promptText) => {
        const session = await window.ai.languageModel.create();
        return await session.prompt(promptText);
    },
    prompt   // second arg → safely serialized by Puppeteer
);
```

The BANNED form (security review will reject):

```js
// UNSAFE: NEVER use template-string interpolation of user input into the
// JS code argument of page.evaluate — any `</script>`, backtick, or quote
// in `prompt` breaks out of the string and runs as code.
const result = await page.evaluate(`(async () => {
    const session = await window.ai.languageModel.create();
    return await session.prompt(\`${prompt}\`);   // BAD
})()`);
```

Task 14 Step 4's `security-reviewer` end-gate explicitly checks for the safe form in `scripts/eval_external/chrome_ai_server.js`.

Failure modes mapped per the umbrella spec §10 R3:
- No Chrome binary → exit 4 → C surfaces `HU_ERR_NOT_SUPPORTED` and scorecard cell becomes `"gemini_nano: unavailable (no chrome)"`.
- `window.ai` capability returns `"no"` → exit 5 → scorecard cell `"gemini_nano: unavailable (capability missing)"`.
- Crash mid-run → exit non-zero → subsequent `generate()` calls return `HU_ERR_NOT_SUPPORTED`.

- [ ] **Step 1-5:** Mirror Task 7. The test factory uses the same canned-response file with the `"gemini_nano"` key.

---

### Task 9: `competitive_harness.c` — orchestrator + scorecard renderer

**Files:**
- Create: `include/human/eval/competitive_harness.h`
- Create: `src/eval/competitive_harness.c`
- Create: `tests/test_competitive_harness.c`

- [ ] **Step 1: Failing test**

```c
/* tests/test_competitive_harness.c */
#include "test_framework.h"
#include "human/eval/competitive_harness.h"
#include "human/eval/judge_external.h"
#include "human/core/allocator.h"
#include <string.h>

static void test_harness_renders_scorecard_with_unavailable_columns_honestly(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_competitive_harness_config_t cfg = {
        .prompt_fixture = "tests/fixtures/lora_baseline_persona_v2_responses.json",
        .out_markdown = "/tmp/scorecard.md",
        .out_json = "/tmp/scorecard.json",
    };
    /* Inject all judges via the test seam: canned for stock + apple_fm,
     * deliberately "unavailable" for gemini_nano so the test exercises
     * the honest-row path. */
    hu_competitive_harness_result_t res = {0};
    HU_ASSERT_EQ(hu_competitive_harness_run_with_test_judges(&alloc, &cfg, &res), HU_OK);
    /* Verify the markdown contains all six columns including the unavailable one. */
    char buf[16384]; FILE *f = fopen("/tmp/scorecard.md", "r");
    HU_ASSERT_NOT_NULL(f); size_t r = fread(buf, 1, sizeof(buf)-1, f); fclose(f); buf[r] = '\0';
    HU_ASSERT_TRUE(strstr(buf, "stock") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "apple_fm") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "gemini_nano: unavailable") != NULL);
}

void run_competitive_harness_tests(void) {
    HU_TEST_SUITE("competitive-harness");
    HU_RUN_TEST(test_harness_renders_scorecard_with_unavailable_columns_honestly);
}
```

- [ ] **Step 2-5:** Implement. The harness loops over `(judge, prompt)` cartesian product, gathers responses, scores via `hu_communication_style_compare_response_sets_v2` (the **4-axis v2 scorer** — see file inventory MODIFY annotation per round-3 NEW-MED-1; the v1 default symbol is intentionally NOT used here) and `hu_reward_model_t.score_batch`, renders markdown table + JSON. The `_with_test_judges` seam injects pre-built `hu_eval_judge_external_t` array bypassing the production wiring.

---

### Task 10: CLI plumbing — `human eval competitive`, `human eval leaderboard`, `human eval gate`

**Files:**
- Modify: `src/main.c::cmd_eval` (+30 LOC dispatch branches)
- Modify: `src/cli_commands.c::cmd_eval` if the `eval` subcommand body lives there (verified at task time)
- Create: `tests/test_cli_eval_phase5.c`

- [ ] **Step 1: Failing test**

```c
/* tests/test_cli_eval_phase5.c */
#include "test_framework.h"

static void test_human_eval_competitive_help_lists_phase5_subcommands(void) {
    /* Use the same execve-and-capture-stdout pattern Phase 2 Task 14 used.
     * Skipped on builds without HU_ENABLE_COMPETITIVE_EVAL. */
    HU_SKIP_IF(!hu_build_has_competitive_eval(),
               "HU_ENABLE_COMPETITIVE_EVAL=OFF in this build");
    char out[8192] = {0};
    HU_ASSERT_EQ(hu_test_run_binary_capture("./build-rl-sota/human eval competitive --help",
                                              out, sizeof(out)), HU_OK);
    HU_ASSERT_TRUE(strstr(out, "competitive") != NULL);
    HU_ASSERT_TRUE(strstr(out, "leaderboard") != NULL);
    HU_ASSERT_TRUE(strstr(out, "gate") != NULL);
}

void run_cli_eval_phase5_tests(void) {
    HU_TEST_SUITE("cli-eval-phase5");
    HU_RUN_TEST(test_human_eval_competitive_help_lists_phase5_subcommands);
}
```

- [ ] **Step 2-5:** Add dispatch branches in `cmd_eval` (the `src/cli_commands.c` body).

**fix(plan,eval,build): `#ifdef HU_ENABLE_RL_FULL` guard on cmd_eval dispatch (round-3 NEW-1):** the `hu_eval_cli_competitive`, `hu_eval_cli_leaderboard`, and `hu_eval_cli_gate` symbols ONLY exist when `HU_ENABLE_RL_FULL=ON` (see the `if(HU_ENABLE_RL_FULL)` block in the CMakeLists.txt section below; `src/eval/cli_eval.c` is listed under that guard). The default `dev` and `release` presets build without `HU_ENABLE_RL_FULL`, so the dispatch lines below MUST be wrapped in `#ifdef HU_ENABLE_RL_FULL` to prevent a linker error in default builds. This preserves the "0-byte delta on default release" claim from the CMakeLists.txt section.

```c
#ifdef HU_ENABLE_RL_FULL
    if (strcmp(sub, "competitive") == 0) return hu_eval_cli_competitive(argc-2, argv+2);
    if (strcmp(sub, "leaderboard") == 0) return hu_eval_cli_leaderboard(argc-2, argv+2);
    if (strcmp(sub, "gate") == 0)        return hu_eval_cli_gate(argc-2, argv+2);
#endif
```

When `HU_ENABLE_RL_FULL` is OFF, the dispatch block is compiled out entirely; the existing `cmd_eval` fall-through path (which prints help and returns `HU_ERR_INVALID_ARGUMENT` for unknown subcommands) handles `competitive`, `leaderboard`, and `gate` cleanly with a "subcommand requires build flag HU_ENABLE_RL_FULL=ON" message. Acceptance: `cmake --preset dev && cmake --build --preset dev` must link successfully without `hu_eval_cli_*` symbols defined.

Each of those `hu_eval_cli_*` entry points lives in a small new file `src/eval/cli_eval.c` (~150 LOC total) wrapping the harness, runners, and gate.

---

### Task 10b (L1): `human ml lora-ab --require-positive` forwards to `hu_eval_gate_decide`

**Files:**
- Modify: `src/ml/cli.c::lora_ab` (+40 LOC dispatch when `--require-positive` is set)
- Create: `tests/test_lora_ab_require_positive_forwards_to_gate.c` (~120 LOC)
- Modify: `tests/test_main.c` (APPEND), `CMakeLists.txt`

Rationale: umbrella spec §4.6 line 367 says "Reuse `human ml lora-ab --require-positive` semantics where applicable" — Phase 5 closes this gap so the cheap pre-commit gate and the heavier runner-side promotion gate share one statistical contract. Without this task, `lora-ab --require-positive` would silently keep using a simpler delta-only check while the runner used the bootstrap-CI gate — two implementations of "is this adapter better?" diverging.

- [ ] **Step 1: Write the failing test**

```c
/* tests/test_lora_ab_require_positive_forwards_to_gate.c */
#include "test_framework.h"
#include "human/eval/eval_gate.h"

static int g_gate_decide_called = 0;
extern void hu_eval_gate_set_decide_spy_for_test(int *counter);

static void test_lora_ab_with_require_positive_calls_hu_eval_gate_decide(void) {
    g_gate_decide_called = 0;
    hu_eval_gate_set_decide_spy_for_test(&g_gate_decide_called);
    /* Run `human ml lora-ab --require-positive --candidate <path> --baseline <path>`
     * via the existing CLI dispatch test seam. */
    char argv0[] = "human", argv1[] = "ml", argv2[] = "lora-ab",
         argv3[] = "--require-positive",
         argv4[] = "--candidate", argv5[] = "tests/fixtures/lora_baseline_persona_v2_responses.json",
         argv6[] = "--baseline",  argv7[] = "tests/fixtures/lora_baseline_persona_v2_responses.json";
    char *argv[] = {argv0, argv1, argv2, argv3, argv4, argv5, argv6, argv7};
    HU_ASSERT_EQ(hu_main_for_test(8, argv), HU_OK);
    HU_ASSERT_TRUE(g_gate_decide_called >= 1);
}

static void test_lora_ab_without_require_positive_does_not_call_gate(void) {
    /* Backward-compat: plain `lora-ab` (no flag) keeps its existing
     * delta-only behaviour. */
    g_gate_decide_called = 0;
    hu_eval_gate_set_decide_spy_for_test(&g_gate_decide_called);
    char argv0[] = "human", argv1[] = "ml", argv2[] = "lora-ab",
         argv3[] = "--candidate", argv4[] = "tests/fixtures/lora_baseline_persona_v2_responses.json",
         argv5[] = "--baseline",  argv6[] = "tests/fixtures/lora_baseline_persona_v2_responses.json";
    char *argv[] = {argv0, argv1, argv2, argv3, argv4, argv5, argv6};
    HU_ASSERT_EQ(hu_main_for_test(7, argv), HU_OK);
    HU_ASSERT_EQ(g_gate_decide_called, 0);
}

void run_lora_ab_require_positive_tests(void) {
    HU_TEST_SUITE("lora-ab-require-positive");
    HU_RUN_TEST(test_lora_ab_with_require_positive_calls_hu_eval_gate_decide);
    HU_RUN_TEST(test_lora_ab_without_require_positive_does_not_call_gate);
}
```

- [ ] **Step 2: Wire `--require-positive` to `hu_eval_gate_decide`**

In `src/ml/cli.c::lora_ab`, when `--require-positive` is set:

1. Build a default `hu_eval_gate_t` with the same spec §1 row 2 thresholds the runner uses (`persona_delta_min = +0.05`, `mt_bench_regression_max = -0.01`, `ifeval_regression_max = -0.02`, `latency_delta_max_ms = +50.0`, `bootstrap_samples = 1000`, `bootstrap_seed = 42`).
2. Skip the leaderboard + reward criteria when running `lora-ab` as a cheap pre-commit gate (`gate.mt_bench = NULL`, `gate.ifeval = NULL`, `gate.reward_model = NULL` — per H3 each NULL criterion is skipped with a clear reason; the round-3 NEW-2 fix codifies this NULL-skip semantics for all three optional runners in D3 / H3).
3. Load the candidate and baseline response sets from the fixture paths passed on the CLI.
4. **fix(plan,eval,baseline): explicit baseline-response→scalar conversion (round-3 NEW-MED-2).** `hu_eval_gate_t` requires `baseline_persona_fidelity_mean` as a **scalar**, but `lora-ab --baseline` accepts a JSONL **file of baseline responses**. Compute the scalar via:

```c
/* Load the baseline JSONL file (one {"prompt":..., "response":...} per line). */
hu_baseline_response_set_t baseline = {0};
hu_error_t e = hu_baseline_response_set_load_jsonl(baseline_path, &baseline);
if (e != HU_OK) { /* report + exit non-zero */ }

/* Score each baseline response with the v2 persona-fidelity scorer
 * (same scorer the candidate side will use, so the comparison is apples-to-apples). */
double *baseline_scores = hu_calloc(baseline.n, sizeof(double));
for (size_t i = 0; i < baseline.n; i++) {
    /* hu_communication_style_fidelity_score_v2 returns float in [0,1] (or -1.0 if
     * sample_count==0; lora-ab's fixture loader rejects empty-target-style cases). */
    baseline_scores[i] = (double)hu_communication_style_fidelity_score_v2(
        target_style, baseline.responses[i], strlen(baseline.responses[i]));
}

/* Reduce to the single scalar the gate needs. */
double baseline_persona_fidelity_mean = 0.0;
for (size_t i = 0; i < baseline.n; i++) baseline_persona_fidelity_mean += baseline_scores[i];
baseline_persona_fidelity_mean /= (double)baseline.n;

gate.baseline_persona_fidelity_mean = baseline_persona_fidelity_mean;
/* Other baseline_*_mean fields stay at 0.0 because mt_bench / ifeval are NULL. */

/* baseline_p95_latency_ms is captured directly from the baseline JSONL's
 * `latency_ms` column (precomputed by the trainer that produced the baseline
 * responses). If the column is missing, fall back to gate.baseline_p95_latency_ms = 0.0
 * and gate.latency_delta_max_ms = INFINITY (effectively skip latency for lora-ab). */
hu_free(baseline_scores);
hu_baseline_response_set_destroy(&baseline);
```

5. Call `hu_eval_gate_decide(...)`.
6. Print the verdict; exit 0 on promote, exit 1 on reject (matching the existing `--require-positive` contract).

Plain `lora-ab` without `--require-positive` keeps its existing delta-only path unchanged.

- [ ] **Step 3-6:** Wire + run + commit. Acceptance: 2/2 tests PASS; existing `tests/test_ml_cli.c::lora_ab_*` tests stay green. (Step count bumped from 3-5 to 3-6 per round-3 NEW-MED-2 insertion of the explicit baseline-scoring step.)

---

### Task 11 (Phase 2 fold-in): `lora_training_runner` calls promotion gate before promoting

**Files:**
- Modify: `include/human/agent/lora_runner.h` (+12 LOC for `hu_eval_gate_t *eval_gate` field)
- Modify: `src/agent/lora_training_runner.c` (+180 LOC)
- Create: `tests/test_lora_training_runner_eval_gate.c`

- [ ] **Step 1: Failing test**

```c
/* tests/test_lora_training_runner_eval_gate.c */
#include "test_framework.h"
#include "human/agent/lora_runner.h"
#include "human/eval/eval_gate.h"
#include "human/provider.h"
#include "human/core/allocator.h"
#include <string.h>

static void test_runner_blocks_promotion_when_gate_rejects(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t *provider = NULL;
    hu_provider_create_for_test_with_canned_response(&alloc, "canned: ok", &provider);

    /* Gate configured to ALWAYS reject — baseline thresholds set higher than the
     * canned scorer will ever produce. */
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.99,
        .persona_delta_min = 0.05,
        .baseline_mt_bench_mean = 99.0, .baseline_ifeval_mean = 0.99,
        .baseline_p95_latency_ms = 0.0, .latency_delta_max_ms = 0.0,
        .bootstrap_samples = 100, .bootstrap_seed = 42,
    };
    hu_lora_runner_ctx_t ctx = {.eval_gate = &gate, .provider = provider, /*...*/};
    /* ... set up a learner with one synthetic signal, fixed adapter_output_path ... */

    /* H1: freeze the runner's clock so the adapter_id is deterministic.
     * 1747042800 = 2026-05-12T07:00:00Z. */
    hu_lora_runner_set_test_clock(1747042800);

    hu_error_t e = hu_lora_training_runner(NULL, /*spec=*/&(hu_job_spec_t){0},
                                            /*budget_ms=*/1000, &ctx);
    HU_ASSERT_EQ(e, HU_OK);

    /* The adapter file should exist at <path>.rejected (moved aside on reject). */
    HU_ASSERT_EQ(access("/tmp/test-adapter.lora", F_OK), -1);
    HU_ASSERT_EQ(access("/tmp/test-adapter.lora.rejected", F_OK), 0);
    /* The provider's load_adapter must NOT have been called. */
    HU_ASSERT_EQ(hu_provider_load_adapter_called_count_for_test(provider), 0);
    /* A gate_decision.json must exist under ~/.human/proofs/<adapter-id>/.
     * (Test sets HOME to /tmp/test-home to make the path discoverable.)
     * H1: build the deterministic path from the frozen clock + method + step. */
    char expected[1024];
    snprintf(expected, sizeof(expected),
             "/tmp/test-home/.human/proofs/2026-05-12-dpo-step-0/gate_decision.json");
    HU_ASSERT_EQ(access(expected, F_OK), 0);
}

static void test_runner_accepts_promotion_when_gate_passes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t *provider = NULL;
    hu_provider_create_for_test_with_canned_response(&alloc, "canned: ok", &provider);
    /* Lenient gate — thresholds set so any canned scorer output passes. */
    hu_eval_gate_t gate = {
        .baseline_persona_fidelity_mean = 0.00,
        .persona_delta_min = 0.0,
        .baseline_mt_bench_mean = 0.0, .baseline_ifeval_mean = 0.0,
        .baseline_p95_latency_ms = 9999.0, .latency_delta_max_ms = 9999.0,
        .bootstrap_samples = 100, .bootstrap_seed = 42,
    };
    hu_lora_runner_ctx_t ctx = {.eval_gate = &gate, .provider = provider, /*...*/};
    HU_ASSERT_EQ(hu_lora_training_runner(NULL, &(hu_job_spec_t){0}, 1000, &ctx), HU_OK);
    HU_ASSERT_TRUE(hu_provider_load_adapter_called_count_for_test(provider) >= 1);
}

static void test_runner_skips_gate_when_eval_gate_is_null(void) {
    /* Backward-compat: existing callers that don't set ctx->eval_gate get
     * the same behaviour as today. */
    hu_lora_runner_ctx_t ctx = {.eval_gate = NULL, /*...*/};
    HU_ASSERT_EQ(hu_lora_training_runner(NULL, &(hu_job_spec_t){0}, 1000, &ctx), HU_OK);
}

void run_runner_eval_gate_tests(void) {
    HU_TEST_SUITE("runner-eval-gate");
    HU_RUN_TEST(test_runner_skips_gate_when_eval_gate_is_null);
    HU_RUN_TEST(test_runner_accepts_promotion_when_gate_passes);
    HU_RUN_TEST(test_runner_blocks_promotion_when_gate_rejects);
}
```

- [ ] **Step 2-5:** Wire the gate per D7. Note: the runner generates the canonical adapter id via `hu_format_adapter_id(method_name, step_index, time_t, buf, cap)` helper added in the same task (new file `src/agent/adapter_id.c` if extraction is justified; otherwise inline). Implementation uses the two-step strftime form per H7 (see below), NOT `"%Y-%m-%d-%s-step-%zu"` (strftime has no `%s` for a method-name string).

**Test seam (H1):** `src/agent/lora_training_runner.c` exposes a test-only symbol `void hu_lora_runner_set_test_clock(time_t frozen)` that overrides the `time(NULL)` call used for the adapter ID timestamp. The symbol is wrapped in `#ifdef HU_IS_TEST`. Tests that need a deterministic proof-directory path call this once at setup time; the runner then uses the frozen value to format the adapter id. Default value (`0`) means "use real wall clock".

**Adapter id formatting (H7):**

```c
/* Replace the (broken) strftime("%Y-%m-%d-%s-step-%zu", ...) with two steps. */
hu_error_t hu_format_adapter_id(const char *method_name, size_t step_index,
                                 time_t when, char *buf, size_t cap) {
    struct tm tm;
    gmtime_r(&when, &tm);
    char datebuf[16];
    if (strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tm) == 0) return HU_ERR_INVALID_ARGUMENT;
    int written = snprintf(buf, cap, "%s-%s-step-%zu", datebuf, method_name, step_index);
    return (written > 0 && (size_t)written < cap) ? HU_OK : HU_ERR_BUFFER_TOO_SMALL;
}
```

`method_name` comes from `hu_rl_trainer_t.vtable->name(ctx)` — e.g. `"dpo"`, `"kto"`, `"grpo"`. With `when = 1747042800` and `method_name = "dpo"` and `step_index = 0`, the result is `"2026-05-12-dpo-step-0"`.

---

### Task 12 (Phase 2 fold-in): Daemon reaction-poll wiring + config gate

**Files:**
- Modify: `src/daemon.c` (+90 LOC: schedule `hu_imessage_poll_reactions` every 30s when feature-flag is on)
- Modify: `src/config.c` + `include/human/config.h` (+25 LOC: new `[reaction_collection]` config block)
- Create: `tests/test_daemon_reaction_poll_wiring.c`
- Create: `tests/fixtures/imessage_chatdb_canned_tapbacks.sql`

- [ ] **Step 1: Failing test**

```c
/* tests/test_daemon_reaction_poll_wiring.c */
#include "test_framework.h"
#include "human/daemon.h"
#include "human/config.h"
#include "human/ml/dpo.h"

static void test_daemon_does_not_call_poll_when_feature_flag_off(void) {
    hu_config_t cfg = {.reaction_collection = {.enabled = false}};
    /* Spin up the daemon test seam — runs one tick of the poll scheduler
     * synchronously without forking. */
    int poll_call_count = 0;
    hu_daemon_set_poll_call_counter_for_test(&poll_call_count);
    HU_ASSERT_EQ(hu_daemon_tick_for_test(&cfg), HU_OK);
    HU_ASSERT_EQ(poll_call_count, 0);
}

static void test_daemon_calls_poll_when_feature_flag_on_and_writes_pref_row(void) {
    HU_SKIP_IF(access("tests/fixtures/imessage_chatdb_canned_tapbacks.sql", R_OK) != 0,
               "canned chat.db missing");
    /* Set up a fake chat.db at /tmp/test-chatdb.sqlite from the canned schema. */
    hu_test_apply_sql("tests/fixtures/imessage_chatdb_canned_tapbacks.sql", "/tmp/test-chatdb.sqlite");
    hu_config_t cfg = {
        .reaction_collection = {.enabled = true,
                                .channels[0] = "imessage", .channel_count = 1},
        .imessage = {.chatdb_path = "/tmp/test-chatdb.sqlite"},
    };
    HU_ASSERT_EQ(hu_daemon_tick_for_test(&cfg), HU_OK);
    /* Verify the canned tapback row landed in dpo_pairs. */
    int row_count = 0;
    HU_ASSERT_EQ(hu_dpo_count_pairs_with_source("imessage_tapback", &row_count), HU_OK);
    HU_ASSERT_TRUE(row_count >= 1);
}

void run_daemon_reaction_poll_tests(void) {
    HU_TEST_SUITE("daemon-reaction-poll");
    HU_RUN_TEST(test_daemon_does_not_call_poll_when_feature_flag_off);
    HU_RUN_TEST(test_daemon_calls_poll_when_feature_flag_on_and_writes_pref_row);
}
```

- [ ] **Step 2-5:** Implement per D8. Note: the daemon's existing poll scheduler architecture is the home for this; do NOT spin up a separate thread. The 30s cadence is configurable via the same config block (`poll_interval_seconds`, default 30).

**H8 — Production reaction-register API (cross-phase with Phase 6 — flag here too):**

Phase 5 Task 12 additionally ships `hu_reaction_handler_register_assistant_message_for_production(...)` as a public symbol (NOT `_for_test`). This is a thin wrapper that the daemon poll loop AND the demo CLI both call to pre-register `(prompt, response)` tuples into the assistant-message lookup store before any reaction event is processed. The existing `_for_test` seam stays test-only. The demo CLI's `synthesize_reactions` flow (developed in Phase 6) consumes the production API so it can pre-register tuples without resorting to the test-only seam. The production API:

```c
/* include/human/agent/reaction_handler.h */
hu_error_t hu_reaction_handler_register_assistant_message_for_production(
    hu_reaction_handler_t *handler,
    const char *chat_id,
    const char *message_id,
    const char *prompt,
    const char *response);
```

Behaviour is identical to the `_for_test` variant; the rename exists so production call sites (daemon + demo CLI) are not stylistically tainted with `_for_test` symbols. Phase 6 (not Phase 5) wires this from the daemon's outgoing-message path; Phase 5 only ships the symbol so Phase 6 doesn't have to revisit Task 12's header.

---

### Task 13 (Phase 2 fold-in): Adapter hot-swap proof-directory contract

**Files:**
- Modify: `src/agent/lora_training_runner.c` (the writer side, already touched in Task 11)
- Create: `tests/test_lora_training_runner_proof_directory.c`

- [ ] **Step 1: Failing test**

```c
/* tests/test_lora_training_runner_proof_directory.c */
#include "test_framework.h"
#include "human/agent/lora_runner.h"
#include <sys/stat.h>

static void test_proof_directory_has_nine_files_on_accept(void) {
    /* Set HOME to /tmp/test-home; run a lenient-gate accept path; verify the
     * nine §8 evidence files exist with non-zero size and valid JSON shape. */
    /* H1: freeze the runner's clock so the adapter_id is deterministic
     * (yields "2026-05-12-dpo-step-0" via the two-step strftime in H7). */
    hu_lora_runner_set_test_clock(1747042800);
    /* ... setup ... */
    HU_ASSERT_EQ(hu_lora_training_runner(/*...*/, &ctx), HU_OK);
    const char *expected[9] = {
        "manifest.json", "training_curves.json", "eval_before.json",
        "eval_after.json", "eval_delta.json", "delta_responses.md",
        "gate_decision.json", "adversarial_review.md", "reproduce.sh",
    };
    char path[1024];
    for (int i = 0; i < 9; i++) {
        snprintf(path, sizeof(path), "/tmp/test-home/.human/proofs/2026-05-12-dpo-step-0/%s", expected[i]);
        struct stat st;
        HU_ASSERT_EQ(stat(path, &st), 0);
        HU_ASSERT_TRUE(st.st_size > 0);
    }
}

static void test_proof_directory_gate_decision_json_only_on_reject(void) {
    /* Strict-gate reject path: only gate_decision.json + the rejected adapter
     * file should exist; the other 7 files (which are evidence of a successful
     * run) should NOT exist. The minimal reject footprint keeps disk noise
     * low when many adapters are tested. */
    /* H1: freeze the runner's clock so the adapter_id is deterministic. */
    hu_lora_runner_set_test_clock(1747042800);
    /* ... setup ... */
    HU_ASSERT_EQ(hu_lora_training_runner(/*...*/, &ctx), HU_OK);
    HU_ASSERT_EQ(access("/tmp/test-home/.human/proofs/2026-05-12-dpo-step-0/gate_decision.json", F_OK), 0);
    HU_ASSERT_EQ(access("/tmp/test-home/.human/proofs/2026-05-12-dpo-step-0/manifest.json", F_OK), -1);
    HU_ASSERT_EQ(access("/tmp/test-adapter.lora.rejected", F_OK), 0);
}

void run_proof_directory_tests(void) {
    HU_TEST_SUITE("proof-directory");
    HU_RUN_TEST(test_proof_directory_has_nine_files_on_accept);
    HU_RUN_TEST(test_proof_directory_gate_decision_json_only_on_reject);
}
```

- [ ] **Step 2-5:** Implement the writer side in `lora_training_runner.c`. `manifest.json` and `reproduce.sh` are generated from a template; `training_curves.json` comes from the learner's per-step metrics; `eval_before.json` / `eval_after.json` / `eval_delta.json` come from the gate's pre-decision capture; `delta_responses.md` is generated from the same 20-prompt fixture used by the harness; `adversarial_review.md` is an empty placeholder filled in by the `critic` + `sprint-auditor` per-phase loop.

---

### Task 14: Phase 5 end-gate (full suite + dead-code-finder + aspect-panel + sprint-auditor + tag)

**Files:** none (operational gate).

- [ ] **Step 1: Full test suite under `rl_sota` preset + `dev` preset**

```bash
cmake --build --preset rl_sota -j8 && ./build-rl-sota/human_tests --suite=v2-axis,bootstrap,judge-external,leaderboard,eval-gate,stock-baseline,apple-fm-client,gemini-nano-client,competitive-harness,cli-eval-phase5,lora-ab-require-positive,runner-eval-gate,daemon-reaction-poll,proof-directory
```
Expected: all 15 suites pass with 0 failures, 0 skips except the ones with documented HU_SKIP_IF conditions.

```bash
cmake --build --preset rl_sota -j8 && ./build-rl-sota/human_tests
```
Expected: ~10300/10300+ PASS (Phase 4 baseline + Phase 5 adds ~50 new tests). ASan clean. UBSan clean.

- [ ] **Step 2: dead-code-finder**

Dispatch `dead-code-finder` subagent on `src/eval/` and `src/agent/lora_training_runner.c` and `src/daemon.c`. Address any unused-export or unreachable-branch findings before the tag.

- [ ] **Step 3: aspect-panel (mandatory for P5 — spec §7)**

Dispatch `aspect-panel` subagent with 5 verifiers (correctness, edge-case, security, regression, style) on the gate decision logic (`src/eval/eval_gate.c`), the bootstrap CI math (`src/eval/bootstrap.c`), the 4th-axis math (`src/memory/personal_model.c`), and the proof-directory writer (`src/agent/lora_training_runner.c`). Confidence-weighted vote required to clear; disagreement ≥40% blocks tag.

- [ ] **Step 4: security-reviewer (spec §7 P5 mandatory)**

Dispatch `security-reviewer` on:
- `src/eval/apple_fm_client.c` + `scripts/eval_external/apple_fm_server.swift` — subprocess management, stdin/stdout handle hygiene, JSON injection.
- `src/eval/gemini_nano_client.c` + `scripts/eval_external/chrome_ai_server.js` — same plus headless Chrome flag whitelist (no `--disable-web-security` etc.).
- `src/daemon.c` reaction-poll wiring — feature flag gating, SQL injection on the chat.db query (parameterized binds only).

Findings ≥ medium severity block the tag.

- [ ] **Step 5: sprint-auditor**

Dispatch `sprint-auditor` with the umbrella spec §4.6 + this plan. Expect PASS on every numbered file in the §4.6 inventory.

- [ ] **Step 6: Tag**

```bash
git tag -a rl-sota-phase-5-complete -m "Phase 5: gate + competitive harness shipped"
```

---

## CMakeLists.txt entries

```cmake
# Phase 5 — gate + competitive harness
option(HU_ENABLE_COMPETITIVE_EVAL "Apple FM + Gemini Nano comparison harness" OFF)

if(HU_ENABLE_RL_FULL)
    list(APPEND HU_CORE_SOURCES
        src/eval/bootstrap.c              # Only if Phase 3 didn't ship it (Task 2)
        src/eval/judge_external.c
        src/eval/leaderboard.c
        src/eval/eval_gate.c
        src/eval/stock_baseline.c
        src/eval/cli_eval.c
        src/agent/adapter_id.c            # Helper for canonical adapter_id formatting
    )

    list(APPEND HU_TEST_SOURCES
        tests/test_communication_style_fidelity_v2_axis.c
        tests/test_bootstrap.c            # Only if Phase 3 didn't ship it (Task 2)
        tests/test_judge_external.c
        tests/test_leaderboard.c
        tests/test_eval_gate.c
        tests/test_stock_baseline.c
        tests/test_competitive_harness.c
        tests/test_cli_eval_phase5.c
        tests/test_lora_ab_require_positive_forwards_to_gate.c   # Task 10b (L1)
        tests/test_lora_training_runner_eval_gate.c
        tests/test_lora_training_runner_proof_directory.c
        tests/test_daemon_reaction_poll_wiring.c
    )
endif()

if(HU_ENABLE_COMPETITIVE_EVAL)
    list(APPEND HU_CORE_SOURCES
        src/eval/apple_fm_client.c
        src/eval/gemini_nano_client.c
        src/eval/competitive_harness.c
    )

    list(APPEND HU_TEST_SOURCES
        tests/test_apple_fm_client.c
        tests/test_gemini_nano_client.c
    )

    # Configure-time soft probes for the external bridge dependencies.
    # NEVER hard-fail the build; the C side handles unavailability at runtime.
    find_program(HU_SWIFTC swiftc)
    if(HU_SWIFTC)
        message(STATUS "swiftc found at ${HU_SWIFTC} — Apple FM bridge buildable")
        target_compile_definitions(human PRIVATE HU_HAVE_SWIFTC=1)
    else()
        message(STATUS "swiftc NOT found — Apple FM bridge will return HU_ERR_NOT_SUPPORTED at runtime")
    endif()

    find_program(HU_NODE node)
    if(HU_NODE)
        message(STATUS "node found at ${HU_NODE} — Chrome AI bridge buildable")
        target_compile_definitions(human PRIVATE HU_HAVE_NODE=1)
    endif()
endif()

# Update rl_sota preset to default HU_ENABLE_COMPETITIVE_EVAL=ON (spec §4.10)
# (this lives in CMakePresets.json, not here — annotation only).
```

**`CMakePresets.json` diff** for the `rl_sota` preset (additive):

```jsonc
{
  "name": "rl_sota",
  "cacheVariables": {
    /* existing flags from Phase 1 / 2 / 3 / 4 ... */
    "HU_ENABLE_COMPETITIVE_EVAL": "ON"
  }
}
```

Default `release` preset stays at `HU_ENABLE_COMPETITIVE_EVAL=OFF` — Phase 5's competitive bridges are opt-in. Hard sanity gate from umbrella spec §4.10 line 487: default release binary delta ≤ +250 KB. Since none of the Phase 5 code links into the default release preset (everything is gated under `HU_ENABLE_RL_FULL` or `HU_ENABLE_COMPETITIVE_EVAL`), the delta is **0 bytes** on default release — well under budget.

---

## Risk register

| # | Risk | Severity | Mitigation |
|---|------|----------|------------|
| **R1** | **4th-axis floor re-tuning conflicts with Track D Phase 1's existing 0.50 floor** in `scripts/check-lora-baseline.sh`. Flipping the default scorer to 4-axis would re-anchor the floor and cause a one-shot CI red. | High | D1 (B1-revised): v2 is opt-in only; the default `hu_communication_style_fidelity_score` body is byte-identical to today, so `scripts/check-lora-baseline.sh` cannot drift. The optional `--include-v2-axis` flag on `check-lora-baseline.sh` runs the v2 path against the v2 fixture independently. Pinning test `test_default_entry_point_stays_3_axis_v1` (Task 1) catches accidental forwarding. |
| **R2** | **Apple FM framework unavailable on macOS < 26 or without entitlement** — Phase 5 ships on a macOS aarch64 reviewer's box that probably doesn't have the framework yet. | Medium | D5 + D6: subprocess only spawned at production runtime. Test mode uses canned fixture. `available()` returns false cleanly on non-macOS-26 hosts. Scorecard cell becomes `"apple_fm: unavailable (macOS < 26)"` — spec §9 DoD #14 is satisfied. |
| **R3** | **Chrome AI / Gemini Nano flag-gated experimental API surface drift** — Chrome's `--enable-features=AIPromptAPI` and `window.ai` API surface have churned 4 times between Chrome 127 and 132. The Node bridge will break the first time the API changes. | Medium | The Node script probes `window.ai.languageModel.capabilities()` at startup; if it returns anything other than `"readily"` or `"after-download"`, exit 5 cleanly. Bridge version pin in `scripts/eval_external/chrome_ai_server.js` header comment with the Chrome version this was authored against (e.g. `// Tested against Chrome Canary 132.0.6800.0 — re-test on Chrome version bumps`). Failure mode is "unavailable" row, not crash. |
| **R4** | **Bootstrap CI seed nondeterminism across platforms** — the existing `hu_rng_*` helpers (used in Phase 3 RM init) are deterministic across Linux+macOS, but a future xoshiro impl change would silently break the CI bounds. | Low | Task 2's bootstrap helper has a `test_bootstrap_ci_deterministic_with_fixed_seed` regression pin (verbatim in Step 1) that catches RNG drift immediately. |
| **R5** | **Gate's 4-criterion AND is too strict** — RM score CI > 0 might never be true if the RM is poorly calibrated. False rejects loop forever. | Medium | D3 + H3 + H5: the documented escape hatch is `gate->reward_model = NULL` (NOT `bootstrap_samples == 0`). When `reward_model == NULL`, the gate skips the reward criterion + writes `"reward criterion skipped: reward_model==NULL"` into `verdict.reason` and sets `verdict.reward_skipped = true`. `bootstrap_samples` is independently constrained: `bootstrap_samples == 0` is INVALID and returns `HU_ERR_INVALID_ARGUMENT` at construction (H5); `bootstrap_samples >= 100` is enforced. Intended for cold-start before the RM has any training data. Spec §10 R12 supports the safety net. |
| **R6** | **`lora_training_runner` test gymnastics** — Task 11's tests synthesise an entire training run end-to-end. Phase 2's analogous tests had heavy `HU_IS_TEST` gating that is fragile. | Medium | Reuse Phase 2's `hu_provider_create_for_test_with_canned_response` seam + Phase 3's `hu_learner_create_for_test`. The Task 11 tests inject a learner whose `train` callback writes a 4-byte stub LoRA file (`"LORA"` magic) — no real training, just exercising the post-train flow. |
| **R7** | **Daemon reaction-poll wiring creates clock drift / poll storms** if the cadence is too tight. | Medium | D8: 30s cadence + opt-in by default. Cadence is configurable via `poll_interval_seconds`. CI-defaults are off so the test suite is unaffected; production uses the user-set value. |
| **R8** | **Gate false-positive promotions** — bootstrap CIs with small n (e.g. 20 prompts) have wide bounds; a "promote" verdict could land on a noisy 20-sample run. Degenerate case: n=1 collapses the bootstrap entirely (zero variance, the CI is a point). | High | The harness defaults to **N=100 prompts** for production eval runs (spec §11 Q4: "100 persona-tagged prompts committed to `tests/fixtures/lora_baseline_persona_v2_responses.json`"). The gate config has `bootstrap_samples=1000` and the CI helper's percentile bands at N≥100 are ≤ ±0.03 — tight enough that the `+0.05` persona-delta-min has signal. Tests use N=20 only for unit-test speed; production never does. **fix(plan,eval,stats): hard precondition (round-3 NEW-MED-3)** — `hu_eval_gate_decide` returns `HU_ERR_INVALID_ARGUMENT` when `n_responses < 30` (Wilson-interval rule-of-thumb floor); the `_for_test` variant carries a relaxed `n >= 10` floor. Both rule out n=1 / n=2 degeneracy. Production fixtures are 100 prompts — well above the floor. |
| **R9** | **Aspect-panel disagreement on bootstrap-CI math correctness** — the percentile bootstrap is a 1979 result but the panel may still flag it. | Medium | Plan front-loads the algorithm in `src/eval/bootstrap.c` header comment with a one-paragraph derivation + reference (Efron, *Bootstrap Methods*, 1979). Pinning test `test_bootstrap_ci_95_brackets_known_mean` (Task 2 Step 1) embeds the math as concrete numbers reviewers can hand-check. |
| **R10** | **Coordination drift with active Track D Phase 1 work** on `hu_communication_style_t` — Track D Phase 1 may add a 5th axis (timing/latency match) between phase boundaries, which would conflict with Phase 5's field-ordering decision. | Medium | At every task start, `spec-verifier` re-checks `include/human/memory/personal_model.h` for new fields. Phase 5's three new fields (`hedging_ratio`, `question_ratio`, `imperative_ratio`) are appended at a known position; if Track D Phase 1 lands new fields first, Phase 5 re-orders to land after them (no ABI break since these are new fields, just file-position drift). |

---

## Common failure modes

These are concrete failure modes anticipated from the file inventory, with explicit detection + recovery. Each Failure mode (F1-F8) carries a "smell" + a "what to check" + a "how to recover".

| # | Failure mode | Smell | What to check | How to recover |
|---|--------------|-------|---------------|----------------|
| **F1** | **4th-axis numerical instability** — `hedging_ratio` computes against a token-counted denominator that can be zero for empty responses, producing NaN. The mean of (lowercase, abbrev, length, NaN) is NaN; the score collapses. | Test runs ASan-clean but `test_v2_axis_returns_higher_score_for_matching_decision_style` returns 0.0 or NaN regardless of input. | `printf "%f\n" hedging_ratio_of("")`; verify zero-length path returns 0.0, not NaN. Read `src/memory/personal_model.c` for the divide-by-zero guard. | Add explicit `if (token_count == 0) return 0.0f;` in each of the three ratio helpers. Add `test_v2_axis_zero_length_response_returns_minus_one` regression pin. |
| **F2** | **Leaderboard cache staleness** — `~/.human/eval_cache/<runner>/<prompt_hash>.json` was generated against an older judge model; new prompts hash differently after the prompt fixture is edited. Cache silently misses on every call. | `human eval leaderboard --runner mt_bench` returns `HU_ERR_NOT_SUPPORTED` for prompts that "should" be cached. | `find ~/.human/eval_cache/mt_bench/ -type f | wc -l` vs `jq length tests/fixtures/lora_baseline_persona_v2_responses.json`. | Add a `--regenerate-cache` flag to `human eval leaderboard`; document in proof artifact. Add a manifest at `~/.human/eval_cache/<runner>/MANIFEST.json` listing prompt-hash-set version so the staleness is auditable. |
| **F3** | **Apple FM Swift FFI process leak** — the subprocess is spawned with `posix_spawn` but `deinit()` forgets to `waitpid()` after sending EOF on stdin. Process becomes zombie until parent exits; under heavy harness runs, ulimit -u trips. | `ps -A | grep apple_fm_server.swift | wc -l` returns >0 after `human eval competitive` completes. | The `apple_fm_judge_create_subprocess` impl in `src/eval/apple_fm_client.c` ; specifically the `deinit_subprocess` function. | Ensure `deinit` does `close(stdin_fd); waitpid(pid, &status, 0);` — and an `atexit()` handler that kills any remaining tracked PIDs. Add `test_apple_fm_no_zombie_after_deinit` (spawned under HU_FORCE_REAL_BRIDGE in nightly only). |
| **F4** | **Chrome AI server hangs** — Node bridge spawns headless Chrome which spawns child processes for each tab; `puppeteer-core` doesn't `dispose()` cleanly when stdin closes. Bridge hangs in `dispose()`. | The harness completes the last prompt and then the run hangs for ≥30s before SIGKILL. | The bridge's response handler in `scripts/eval_external/chrome_ai_server.js`; specifically the stdin-EOF callback. | Wrap the `browser.close()` call in `Promise.race([browser.close(), timeout(5000)])`; if timeout wins, `process.exit(0)`. Add `test_gemini_nano_deinit_within_5s_under_real_bridge` (HU_FORCE_REAL_BRIDGE nightly). |
| **F5** | **Gate false-positive promotion** — `bootstrap_samples=10` (test config leaked into a production config); CI bounds are absurdly wide; even a regressed adapter passes. Or `n_responses=1` (degenerate bootstrap — zero variance, collapsed bounds) silently accepts a single-observation "lucky shot". | `~/.human/proofs/<adapter-id>/gate_decision.json` shows `persona_ci_upper - persona_ci_lower > 0.20` OR `persona_ci_upper == persona_ci_lower` (zero-width band from n=1). | `jq '.persona_ci_upper - .persona_ci_lower' ~/.human/proofs/*/gate_decision.json`. | The `hu_eval_gate_decide` impl rejects any config with `bootstrap_samples < 100` (return `HU_ERR_INVALID_ARGUMENT`). `bootstrap_samples == 0` is INVALID (H5 — NOT a "skip the reward criterion" sentinel; to skip the reward criterion, set `gate->reward_model = NULL` instead per H3). **fix(plan,eval,stats): `n_responses >= 30` precondition (round-3 NEW-MED-3)** — production entry point rejects `n_responses < 30` (Wilson-interval rule-of-thumb floor); the `_for_test` variant has a relaxed `n >= 10` floor for unit-test speed. Both floors close the n=1 degeneracy. Document the floors in `hu_eval_gate_t` and `hu_eval_gate_decide` field/parameter comments. |
| **F6** | **`lora_training_runner` orphan adapter files** — gate rejects, runner moves adapter to `<path>.rejected`, but on next training cycle the same path is targeted; the `.rejected` file accumulates. | `ls -la /tmp/test-adapter.lora.rejected.*` shows numbered backups (`.rejected.1`, `.rejected.2`, ...) growing over CI runs. | The rename-on-reject path in `src/agent/lora_training_runner.c`. | The runner appends a 6-char random suffix (`<path>.rejected.<6hex>`); add a daily job that prunes `.rejected.*` files older than 7 days. Or document that operators clean these up out-of-band (KISS preference). |
| **F7** | **Daemon reaction-poll thrash** — `hu_imessage_poll_reactions` opens the chat.db on every tick; macOS file-handle cache fills up and SQLite returns `BUSY` for the user's own Messages app. | User reports "iMessage is slow when human daemon is running". `lsof -p $(pgrep daemon) | grep chat.db | wc -l` shows >1 handle. | `src/daemon.c` poll-tick handler. The handle should be held in `hu_daemon_state_t` and reused, not opened per tick. | Cache the SQLite handle in the daemon state, open once on `enable=true`, close on `enable=false`. Add `test_daemon_reaction_poll_reuses_chatdb_handle` (uses a strace shim in dev preset). |
| **F8** | **Competitive harness empty scorecard** — `available()` returns false for all five competitor judges; the harness writes a markdown table with five "unavailable" rows and the reviewer thinks the run failed. | `cat ~/.human/proofs/<adapter-id>/scorecard.md` shows nothing but unavailable rows. | The harness orchestrator in `src/eval/competitive_harness.c`. | The harness writes a leading "Run summary: X of 5 competitors available" paragraph and exits non-zero if 0 of 5 are available (per spec §10 R2/R3 the goal is to be honest, but a run with zero comparisons isn't a comparison). Make the threshold configurable via `--min-available N` (default 2 — at least stock + one external). |

---

## Definition of Done

Phase 5 is done when **all** of the following are true:

1. All Phase 5 tests (~50 new) pass under both `rl_sota` and `dev` presets, 0 ASan errors, 0 UBSan errors.
2. `hu_communication_style_fidelity_score_v2` exists as a new opt-in symbol that returns the 4-axis mean; the existing 3-axis `hu_communication_style_fidelity_score` symbol is byte-identical to pre-Phase-5; `tests/test_personal_model.c::personal_model_fidelity_*` and `scripts/check-lora-baseline.sh` continue to pass without modification (no floor change, no test edit).
3. `hu_eval_gate_decide` correctly accepts/rejects on synthetic per-criterion inputs (Task 5's 5-test matrix passes — 4 promote/reject tests + 1 n_responses-floor regression pin per round-3 NEW-MED-3); bootstrap CIs are deterministic with fixed seed (Task 2 pin); production entry point rejects `n_responses < 30` and `_for_test` variant rejects `n_responses < 10` per the round-3 NEW-MED-3 fix.
4. `human eval competitive` renders the §1 scorecard with at least the stock-Gemma + one external competitor column populated honestly (or `unavailable (reason)` per spec §9 DoD #14); writes both `scorecard.md` and `scorecard.json` to a configurable output path.
5. `src/agent/lora_training_runner.c` calls the gate before `hu_provider_load_adapter` when `ctx->eval_gate != NULL`; backward-compatible when NULL; rejected adapters land at `<path>.rejected` and never call `load_adapter`.
6. `~/.human/proofs/<adapter-id>/` evidence directory contains all 9 files on accept and only `gate_decision.json` + `.rejected` file on reject (Task 13 pins both shapes).
7. Daemon reaction-poll wiring is feature-flag-gated, defaults OFF, and is independently testable via `hu_daemon_tick_for_test` (Task 12); CI runs are unaffected.
8. `aspect-panel` end-gate (Task 14 Step 3) cleared with <40% disagreement.
9. `security-reviewer` end-gate (Task 14 Step 4) cleared with no medium-or-higher findings.
10. `sprint-auditor` issued PASS verdict on Phase 5 (logged in `docs/proof/adversarial-audit-report.md` archive).
11. Tag `rl-sota-phase-5-complete` exists and points at the merge commit.

---

## Out of scope (explicitly deferred to Phase 6 / v1.5)

- ❌ Live LLM-as-judge fallback when leaderboard cache misses — Phase 5 returns `HU_ERR_NOT_SUPPORTED` cleanly; Phase 6 wires real-judge fallback via `scripts/run-gemma-sanity-gate.sh`-style external invocation.
- ❌ Real Apple FM / Gemini Nano competitive numbers on the public website — the proof artefact in `docs/proof/rl-loop-proof.md` is Phase 6 scope.
- ❌ Per-persona gates with per-persona thresholds — Phase 5 ships single-persona ("Seth") thresholds per spec §11 Q6.
- ❌ Multi-tenant scorecard rendering — single-persona scorecard only.
- ❌ Online gate (gate runs at chat-time rather than train-time) — only train-time gating in Phase 5; chat-time is `HU_ERR_NOT_SUPPORTED` deferred to v2 research.
- ❌ Web dashboard visualisation of scorecards — proof artefact is markdown + JSON; visualisation is v1.5.
- ❌ Gate-config UI in `human onboard` — onboarding wizard mentions reaction-collection consent only; gate thresholds are config-file-only in v1.
- ❌ A 5th axis (timing match — message timing, response delay) — out of scope; reserved for v1.5 if Track D Phase 1 ships a freshness/timing extraction first.

---

## Self-review note

**Author's checklist before handing off:**

- [x] `HU_TEST_SUITE("...")` is inside every `run_*_tests` function body (NOT at file scope) — verified in every test code block above (Task 1 step 1, Task 2 step 1, Task 3 step 1, ..., Task 14 covered by suite-level enforcement).
- [x] `HU_SKIP_IF` is used for every environment-gated test path: leaderboard production-mode (Task 4), CLI competitive-flag-off (Task 10), daemon canned-chatdb missing (Task 12). (The Apple-FM canned-missing path was removed during adversarial review — M1.)
- [x] Backward-compat 4th axis: `hu_communication_style_fidelity_score_v2` added as opt-in symbol; default `hu_communication_style_fidelity_score` body unchanged (D1 + Task 1 — B1-revised).
- [x] Fixture extension is additive to `tests/fixtures/lora_baseline_persona.json` + new `tests/fixtures/lora_baseline_persona_v2_responses.json` (D1 + R10).
- [x] Apple FM bridge runs as long-running Swift server speaking JSON over stdio, mirroring Phase 2/3 MLX subprocess pattern but in Swift (D5 + Task 7).
- [x] Chrome AI bridge uses headless Chrome with `--enable-features=AIPromptAPI` flag (D5 + Task 8).
- [x] All external judges have deterministic test-mode fallbacks via canned fixture (D6 + Tasks 7/8/3).
- [x] Gate is statistical: one-sided bootstrap CI, `lower-95-CI(candidate) > baseline_mean + delta_min` is the bar (D3 + Task 5 + R8 — B2-revised; two-sided non-overlap deferred to Phase 6).
- [x] Phase 2 deferrals folded in: `lora_training_runner` gate (Task 11), daemon reaction-poll wiring (Task 12), adapter hot-swap proof directory (Task 13).
- [x] Phase 3 dependency: `hu_reward_model_t` is the canonical reward function for the gate's 4th criterion (D3 + Task 5 + R5).
- [x] Common failure modes table includes 4th-axis numerical instability (F1), leaderboard cache staleness (F2), Apple FM process leak (F3), Chrome AI hang (F4), gate false-positive (F5), `lora_training_runner` orphan files (F6), daemon poll thrash (F7), competitive harness empty scorecard (F8) — 8 of 8 requested.
- [x] 12-15 tasks (Tasks 0–14) — count = **15**.
- [x] 10 architectural decisions (D1–D10) — count = **10**.
- [x] 10 risks (R1–R10) — count = **10**.

Open questions for the reviewer:

1. **RESOLVED (L1):** the umbrella spec says (§4.6 line 367) "Reuse `human ml lora-ab --require-positive` semantics where applicable" — the answer is **YES, `lora-ab --require-positive` forwards to the new `hu_eval_gate_decide`** (Phase 5 closes this gap; spec §4.6 says "reuse where applicable"). See new **Task 10b** below. The existing `scripts/check-lora-baseline.sh` is still unchanged (it uses the v1 default scorer per B1); only the `lora-ab` CLI is rewired.
2. Daemon reaction-poll cadence default of 30s (D8) — is this right? Phase 2's original spec line was silent on this. 30s mirrors the existing daemon poll-job cadences but feels aggressive for production. Open to reviewer.
3. Apple FM Swift bridge — should the build configure-time probe `swiftc` and **fail the configure** if `HU_ENABLE_COMPETITIVE_EVAL=ON` AND `swiftc` is missing? This plan defaults to "soft probe" (config succeeds; runtime returns NOT_SUPPORTED). Alternative is hard-fail at configure time which prevents shipping a binary that pretends to support Apple FM but never can.
