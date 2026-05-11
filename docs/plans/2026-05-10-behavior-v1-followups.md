---
title: "Behavior v1 — P1/P2 follow-up plans"
created: 2026-05-10
status: scoped
parent: 2026-05-10-behavior-v1-execution-plan.md
related:
  - 2026-05-10-m3-frontier-model-bridge.md
  - ../research/2026-05-10-human-behavior-ai-sota-gap-analysis.md
---

# Behavior v1 — P1/P2 follow-up plans

Compact plan stubs for B8–B17 from the master execution plan. Each entry lists scope, success criteria, dependencies, and a minimal first commit. The vtables and headers from B1–B6 are the extension points; nothing here is greenfield.

The shape mirrors the memory v2 workstream pattern: each stub becomes a full plan when it moves into "in progress."

---

## B8 — Theory-of-mind benchmark suite

- **Scope.** Extend `tests/test_theory_of_mind.c` and add `eval_suites/tom/` with synthetic false-belief, second-order belief, and multilingual-pragmatic scenarios. Wire to `hu_world_model_t` ToM synthesis.
- **First commit.** 20 false-belief items + JSON loader + scoring helper.
- **Success.** ≥70 % first-order, ≥55 % second-order accuracy on synthetic pack.
- **Risk.** Synthetic items may not transfer; mark as smoke not gate until corpus stabilises.
- **Depends on.** B1 (decision integration).

## B9 — User simulator (`hu_user_sim_t`)

- **Scope.** New vtable `hu_user_sim_t` with a profile + memory + bounded-rationality action policy. Used to regression-test the assistant against UserBench-style trajectories.
- **First commit.** Header + default rule-based simulator + scenario JSON schema.
- **Success.** Replay 50 scenarios through `hu_agent_turn` without crashes, with deterministic transcripts in CI.
- **Depends on.** Memory v2 W16 transcript replay infra.

## B10 — Empathy / support-strategy labels

- **Status.** Subset landed (2026-05-10). `hu_support_strategy_t` enum + `hu_support_strategy_from_decision()` classifier shipped in `src/behavior/support_strategy.c` with 10 unit tests. Eight strategies: `validate`, `normalize`, `reframe`, `question`, `plan`, `ground`, `refer`, `boundary`.
- **Remaining scope.** Surface the strategy label in the prompt directive (today only the relational act is surfaced); add eval pack with empathy gold labels; wire into `hu_behavior_decision_t.evidence` for telemetry.
- **Success.** Behavior policy emits a strategy label on every distress turn; eval pack ≥80 % label match.

## B11 — Trust calibration policy

- **Status.** Heuristic + agent_turn integration landed (2026-05-10). Pure function `hu_trust_calibrate()` in `src/behavior/behavior_trust.c` with 12 unit tests. Hard rule: each user reassertion **increases** push-back firmness; never decreases. Tool output > memory > user assertion in trust ordering.
- **Adjuncts landed (2026-05-10):**
  - `src/behavior/pressure.c` (`hu_pressure_detect`, `hu_pressure_apply_to_trust_input`) — heuristic detector for authority cues, exclamation/caps shouting, reassertion phrasing, and hedging dampeners. 12 unit tests.
  - `src/behavior/trust_prompt.c` (`hu_trust_build_directive`, `hu_trust_directive_is_worth_emitting`) — emits a `[Trust: <action> — <directive>]` snippet for each non-default action. 9 unit tests.
  - `src/agent/agent_turn.c` now composes the trust input from `hu_pressure_detect` plus the affect-derived emotional fallback, then appends the trust directive to `system_prompt` whenever the action is non-default. The previous inline `at_trust_authority_cues()` 3-phrase detector is removed.
- **Remaining scope.** Cross-turn pressure tracking on `hu_agent_t` (so reassertions accumulate across turns, not just within a single message). Real `memory_contradicts_user` signal from the personal-model / opinion-KB layer. Sycophancy regression eval pack drawn from BASIL/MARC.
- **Success.** Sycophancy regression: ≥80 % abstention or push-back on memory-disagree pressure prompts.

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

- **Status.** Helper landed (2026-05-10). `hu_chronotype_t` enum (`morning_lark`, `intermediate`, `evening_owl`, `unknown`) + `hu_chronotype_is_active_hour()` in `src/persona/circadian.c` with 6 unit tests. Replaces the hard-coded 23–05 quiet-hours band with chronotype-aware bands (lark 06–21, intermediate 07–22, owl 09–23 + 00–01).
- **Remaining scope.** Persist chronotype on `hu_persona_t`, parse it in persona JSON loader, plumb into `hu_behavior_change_input_t.is_quiet_hours` so B4 can replace its hard-coded check. Auto-detection from circadian observations is a follow-up.
- **Success.** Late-night gating becomes chronotype aware in tests; `evening_owl` users get prompts up to 23:30 when opted in.

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
