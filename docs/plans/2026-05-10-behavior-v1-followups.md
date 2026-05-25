---
title: "Behavior v1 — P1/P2 follow-up plans"
created: 2026-05-10
status: deferred
parent: 2026-05-10-behavior-v1-execution-plan.md
related:
  - 2026-05-10-m3-frontier-model-bridge.md
  - ../research/2026-05-10-human-behavior-ai-sota-gap-analysis.md
last_audit: 2026-05-25
---

# Behavior v1 — P1/P2 follow-up plans

Compact plan stubs for B8–B17 from the master execution plan. Each entry lists scope, success criteria, dependencies, and a minimal first commit. The vtables and headers from B1–B6 are the extension points; nothing here is greenfield.

The shape mirrors the memory v2 workstream pattern: each stub becomes a full plan when it moves into "in progress."

---

## B8 — Theory-of-mind benchmark suite

- **Status.** Landed (2026-05-10).
  - `eval_suites/tom/tom_synthetic.json` — 10 items spanning false-belief, second-order, pragmatic implicature, multilingual stub, and common-knowledge categories with `gold_answer` rubric strings.
  - `src/agent/tom_scenario.c` + `include/human/agent/tom_scenario.h` ship `hu_tom_scenario_synthesize` (premise/question/category → `hu_theory_of_mind_t` with `[ToM:<tag>]` planner hints), `hu_world_model_merge_tom_scenario` (merges synthesized ToM into a loaded `hu_world_model_t`), `hu_tom_scenario_gold_matches_response` (case-insensitive, underscore-tokenised rubric matcher), `hu_tom_b8_synthetic_pack_run_smoke` (category-tag self-test), `hu_tom_b8_synthetic_pack_score_gold` (CI smoke against premise+question+stub overlap), and **`hu_tom_b8_synthetic_pack_score_responses`** (CLI / model-eval hook that scores an `hu_tom_b8_response_t[]` array against the JSON pack, with a `count_unanswered_as_failed` policy flag).
  - `hu_w7_render_world_model` now accepts optional trailing `tom_premise` / `tom_question` / `tom_category` parameters; when all three are non-empty the bridge calls `hu_world_model_merge_tom_scenario` after `hu_world_model_load` and before formatting.
  - `hu_agent_t` carries `tom_scenario_premise/question/category` fixed buffers; **`hu_agent_set_tom_scenario`** copies (or clears) those, and `agent_turn.c` / `agent_stream.c` thread them into the bridge call. Production turns leave the fields empty (no behavior change); eval / benchmark drivers call the setter before `hu_agent_turn`.
  - Tests in `tests/test_tom_scenario_b8.c` + the `bridge_render_optional_tom_scenario_merges_into_output` case in `tests/test_world_model_bridge.c` cover synthesis, gold matching (synthetic + response-array), unanswered-policy semantics, agent setter truncation/clearing, and the bridge merge path.
- **Remaining scope.**
  - First-order ≥70 % / second-order ≥55 % gates against an external model run (today the pack passes smoke + ≥3/10 partial coverage; gates will land once we point a frontier provider at the response-array CLI).
  - Optional eval CLI subcommand (`human eval tom run …`) that pulls model responses through `hu_tom_b8_synthetic_pack_score_responses` and prints pass/total.
- **Success.** ≥70 % first-order, ≥55 % second-order accuracy on synthetic pack (gate, blocked on the eval CLI hook above).
- **Risk.** Synthetic items may not transfer; pack stays smoke until the gated metric is stable across at least two frontier model versions.
- **Depends on.** B1 (decision integration).

## B9 — User simulator (`hu_user_sim_t`)

- **Scope.** New vtable `hu_user_sim_t` with a profile + memory + bounded-rationality action policy. Used to regression-test the assistant against UserBench-style trajectories.
- **First commit.** Header + default rule-based simulator + scenario JSON schema.
- **Success.** Replay 50 scenarios through `hu_agent_turn` without crashes, with deterministic transcripts in CI.
- **Depends on.** Memory v2 W16 transcript replay infra.

## B10 — Empathy / support-strategy labels

- **Status.** Landed (2026-05-10). `hu_support_strategy_t` enum + `hu_support_strategy_from_decision()` classifier shipped in `src/behavior/support_strategy.c` with 10 unit tests. Eight strategies: `validate`, `normalize`, `reframe`, `question`, `plan`, `ground`, `refer`, `boundary`. `src/behavior/prompt.c::hu_behavior_build_directive` now appends `Support strategy: <name>.` to the directive whenever the classifier returns a non-`NONE` strategy, so the model sees the empathy frame alongside the relational act.
- **Remaining scope.** Empathy eval pack with gold labels; wire into `hu_behavior_decision_t.evidence` for telemetry beyond the prompt-side surfacing.
- **Success.** Behavior policy emits a strategy label on every distress turn (done); eval pack ≥80 % label match (pending).

## B11 — Trust calibration policy

- **Status.** Landed (2026-05-10), including cross-turn pressure tracking. Pure function `hu_trust_calibrate()` in `src/behavior/behavior_trust.c` with 12 unit tests. Hard rule: each user reassertion **increases** push-back firmness; never decreases. Tool output > memory > user assertion in trust ordering.
- **Adjuncts landed (2026-05-10):**
  - `src/behavior/pressure.c` (`hu_pressure_detect`, `hu_pressure_apply_to_trust_input`) — heuristic detector for authority cues, exclamation/caps shouting, reassertion phrasing, and hedging dampeners. 12 unit tests.
  - `src/behavior/pressure_history.c` (`hu_pressure_history_observe`, `hu_pressure_history_apply_to_trust_input`) — fixed-size ring buffer of recent user messages + last assistant trust action, so reassertions accumulate across turns instead of resetting per-message.
  - `src/behavior/trust_prompt.c` (`hu_trust_build_directive`, `hu_trust_directive_is_worth_emitting`) — emits a `[Trust: <action> — <directive>]` snippet for each non-default action. 9 unit tests.
  - `hu_agent_t` now carries `hu_pressure_history_t pressure_history` (~1.2 KB POD field, zero-init on `memset`).
  - `src/agent/agent_turn.c` reads from `agent->pressure_history` *before* `hu_trust_calibrate` (so cross-turn reassertions flip `user_reasserted_after_pushback`) and writes to it *after* (so subsequent turns see the action that was actually taken). The E2E wire is pinned by `tests/test_b11_pressure_history_e2e.c` (3 tests covering observe-on-write, no inflation on unrelated messages, and ring-buffer wraparound safety).
- **Remaining scope.** Real `memory_contradicts_user` signal from the personal-model / opinion-KB layer. Sycophancy regression eval pack drawn from BASIL/MARC (the `eval_suites/sycophancy/` corpus is in the repo; gating it in CI is the next step).
- **Success.** Sycophancy regression: ≥80 % abstention or push-back on memory-disagree pressure prompts (pending eval gate).

## B12 — Multimodal affect

- **Scope.** Implement `hu_affect_estimate_audio()` (prosody features) and a fusion path through `hu_affect_fuse`. Produce/consume in `src/voice/`.
- **First commit.** Stub estimator returning neutral with high uncertainty + integration test against the existing duplex pipeline.
- **Success.** Voice turns can carry an affect snapshot; B1 routing changes between text-only and audio-fused inputs in tests.
- **Depends on.** B3 (already shipped).

## B13 — Other-initiated repair eval pack

- **Scope.** Corpus of repair scenarios (text + voice) with expected `hu_dialog_act_t` and `hu_relational_act_t`. Includes "what did you mean?", anaphora confusion, mishearing.
- **First commit.** 30-item JSON pack + `tests/test_repair_pack.c` smoke runner.
- **Success.** ≥85 % expected dialog-act match, ≥80 % expected relational-act match.

## B14 — Learned persona control (DPO/LoRA)

- **Scope.** Train a persona LoRA against persona example banks + DPO on collected accept/reject pairs. Activate at chat time on the frontier model.
- **First commit.** Wire B6 eval harness as the offline reward signal; ship a no-op runner script.
- **Success.** LoRA improves B6 retest stability by ≥10 points on a held-out persona.
- **Depends on.** [`2026-05-10-m3-frontier-model-bridge.md`](2026-05-10-m3-frontier-model-bridge.md).

---

## B15 — Cultural pragmatics overlay

- **Scope.** Extend `hu_persona_overlay_t` with `directness`, `face_saving`, `disagreement_style`, `silence_tolerance`. Respect explicit user preference only — never inferred from name/locale.
- **First commit.** Schema + JSON parsing + prompt builder edit.
- **Success.** Per-channel overlays compose with persona; tests cover three contrasting profiles.
- **Risk.** Stereotyping. Mitigation: opt-in only, no inference, documented in `docs/standards/ai/responsible-ai.md`.

## B16 — Chronotype-aligned JITAI

- **Status.** Production-wired (2026-05-10).
  - `hu_chronotype_t` enum (`morning_lark`, `intermediate`, `evening_owl`, `unknown`) + `hu_chronotype_is_active_hour()` in `src/persona/circadian.c` with 6 unit tests. Replaces the hard-coded 23–05 quiet-hours band with chronotype-aware bands (lark 06–21, intermediate 07–22, owl 09–23 + 00–01).
  - `hu_persona_t.chronotype` field shipped in `include/human/persona.h`.
  - JSON loader parses the `chronotype` field in `src/persona/persona.c:1675` (string → enum, unknown values fall through to `HU_CHRONO_UNKNOWN`).
  - `hu_behavior_change_input_t.jitai_chronotype` is consumed by `src/behavior/change.c::bct_outside_jitai_hours`, replacing the hard-coded 23–05 check.
  - **Production wire.** `hu_scheduler_probe_quiet_hours` (`src/agent/scheduler_probes.c:310`) — the function that actually gates proactive messages from `src/agent/scheduler.c:442` and `:573` — now consults `persona->chronotype` first, falling back to the legacy 01–06 band when the field is `HU_CHRONO_UNKNOWN`. A new `HU_TEST_HOUR` env override drives a portable hour for tests so the wire is provable without TZ flakiness. 7 new tests in `tests/test_w14_scheduler.c::run_w14_scheduler_tests` (`test_b16_quiet_hours_*`) pin lark/intermediate/owl/unknown bands and the NULL-persona case.
- **Remaining scope.**
  - Auto-detection of chronotype from circadian observations (lower priority — the persona-overlay path now works end-to-end).
  - `hu_behavior_change_select` (BCT recommender) is still not called from `agent_turn.c`; that's a separate B14/B5 surface, not the B16 quiet-hours gate.
- **Success.** Late-night gating is chronotype aware end-to-end. Larks see proactive messages suppressed at 22:00; owls see them up to 23:00 + 00:00–01:00 (`test_b16_quiet_hours_owl_at_23_is_active`).

## B17 — On-device frontier behavior

- **Scope.** Folds into [`2026-05-10-m3-frontier-model-bridge.md`](2026-05-10-m3-frontier-model-bridge.md). Behavior v1 is the consumer: once the bridge ships, B14 trains against it; B12 receives audio embeddings from it.
- **Success.** Same B6 + B11 + B13 evals run against on-device frontier without code changes.

---

## How these stubs become plans

Promote a stub by:

1. Copying it to `docs/plans/2026-05-10-b<N>-<slug>.md`.
2. Adding a YAML frontmatter parent pointing back here.
3. Filling out `## Architecture`, `## Validation`, `## Risks`, `## Out of scope` sections (mirroring the memory v2 plans).
4. Updating the master `2026-05-10-behavior-v1-execution-plan.md` table from `scoped` → `in progress`.

The vtables and headers from B1–B6 are stable enough that none of these promotions should break ABI.
