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

- **Scope.** Add `hu_support_strategy_t` enum (validate, normalize, reframe, question, plan, ground, refer, boundary). Consumed by B1's `hu_behavior_decision_t.evidence` and surfaced to the prompt builder.
- **First commit.** Enum + name table + classifier from `hu_dialog_act_t`.
- **Success.** Behavior policy emits a strategy label on every distress turn; tests check label monotonicity.

## B11 — Trust calibration policy

- **Scope.** Add `hu_trust_policy_t` choosing answer / ask / abstain / cite memory / refuse. Sycophancy regression suite drawn from BASIL/MARC.
- **First commit.** Header + heuristic policy that abstains under user pressure when memory disagrees.
- **Success.** Sycophancy regression: ≥80 % abstention or push-back on memory-disagree pressure prompts.
- **Depends on.** B1 + memory contradiction signal.

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

- **Scope.** Extend `src/persona/circadian.c` to expose chronotype phase (`morning_lark`, `evening_owl`, `flexible`) for B4 to consume.
- **First commit.** Add chronotype enum to overlay + persona JSON schema; wire into `hu_behavior_change_input_t` defaults via callers.
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
