---
title: "Behavior v1 Execution Plan"
created: 2026-05-10
status: in_progress
related:
  - 2026-05-10-master-follow-through-program.md
  - 2026-05-10-memory-v2-roadmap-overview.md
  - ../research/2026-05-10-human-behavior-ai-sota-gap-analysis.md
---

# Behavior v1 Execution Plan

_Closing the P0/P1/P2 gaps from `2026-05-10-human-behavior-ai-sota-gap-analysis.md` with a single, vtable-friendly behavior layer that composes existing persona, memory, voice, and security modules instead of duplicating them._

## Why this exists

The May 2026 SOTA gap analysis listed 17 capabilities across persona consistency, real-time conversation mechanics, applied behavior science, theory of mind, affective computing, memory eval, identity persistence, companion safety, trust calibration, and cultural pragmatics. h-uman already has rich primitives in `src/persona/`, `src/memory/`, `src/voice/`, and `src/security/companion_safety.c`. The gap is not raw building blocks — it is a **central behavior layer** that:

1. Decides relational acts (acknowledge, ask, repair, backchannel, push back, boundary, defer) from evidence, not from scattered prompt fragments.
2. Classifies user dialogue acts and detects other-initiated repair.
3. Carries continuous valence/arousal/dominance affect, replacing keyword-only emotional routing.
4. Selects evidence-based behavior-change techniques with autonomy and burden gating.
5. Composes existing companion-safety + vulnerability detection into actionable assistant responses.
6. Runs PICon-style persona consistency evaluations.

No layer reaches around another. Each workstream ships behind a small vtable or pure-C helper.

## Workstream index

The 17 items from the gap analysis map onto 17 workstreams. P0 lands in this PR; P1/P2 ship as follow-up work behind the same vtables.

| ID | Workstream | Source/Header | Priority | Status |
| --- | --- | --- | --- | --- |
| B1 | Central behavior policy | `src/behavior/policy.c`, `include/human/behavior/policy.h` | P0 | landed |
| B2 | Dialog acts + repair detection | `src/behavior/dialog_act.c`, `include/human/behavior/dialog_act.h` | P0 | landed |
| B3 | Continuous affect (VAD) | `src/behavior/affect.c`, `include/human/behavior/affect.h` | P0 | landed |
| B4 | Behavior-change engine (BCT + Fogg + JITAI) | `src/behavior/change.c`, `include/human/behavior/change.h` | P0 | landed |
| B5 | Companion safety integration (anti-dependency) | `src/behavior/safety.c`, `include/human/behavior/safety.h` | P0 | landed |
| B6 | Persona consistency evaluation harness | `src/persona/eval.c`, `include/human/persona/eval.h` | P0 | landed |
| B7 | LongMemEval scaffold for W16 | tests/eval scaffold (planned) | P0 | scoped |
| B8 | Theory-of-mind benchmark suite | extends `src/agent/theory_of_mind.c` (planned) | P1 | scoped |
| B9 | User simulator (`hu_user_sim_t`) | new vtable (planned) | P1 | scoped |
| B10 | Empathy / support-strategy labeling | extends behavior policy (planned) | P1 | scoped |
| B11 | Trust calibration policy | extends behavior policy (planned) | P1 | scoped |
| B12 | Multimodal affect (voice prosody hooks) | extends `hu_affect_t` (planned) | P1 | scoped |
| B13 | Other-initiated repair eval pack | extends B2 + voice (planned) | P1 | scoped |
| B14 | Learned persona control (DPO/LoRA loop) | extends `src/ml/`, depends on M3 | P1 | depends-m3 |
| B15 | Cultural pragmatics overlay | extends `hu_persona_overlay_t` (planned) | P2 | scoped |
| B16 | Chronotype-aligned JITAI | extends `hu_circadian_*` and B4 (planned) | P2 | scoped |
| B17 | On-device frontier bridge for behavior | folds into M3 frontier plan | P2 | depends-m3 |

"landed" = code + tests in this PR. "scoped" = covered by extension points + design notes below. "depends-m3" = waiting on `2026-05-10-m3-frontier-model-bridge.md`.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    hu_behavior_policy_t                     │  B1
│ decide(input) → hu_behavior_decision_t {act, intensity, …}  │
└──────────┬─────────────┬─────────────┬─────────────┬───────┘
           │             │             │             │
           ▼             ▼             ▼             ▼
   hu_dialog_act_t  hu_affect_t   hu_behavior_  hu_behavior_
   classify+repair  VAD+decay     change_t      safety_t
        B2              B3            B4              B5
                                                       │
                                                       ▼
                                          src/security/companion_safety.c
                                          (SHIELD-001, vulnerability)

    hu_persona_eval_t      ← runs interrogation suites against any responder
         B6                  (PICon-style: contradictions + retest)
```

The behavior policy is the only module the agent loop talks to. Dialog acts, affect, change, and safety are pure helpers it composes. The eval harness is offline and runs against a `hu_persona_response_fn` so it never touches the runtime path.

## Per-workstream notes

### B1 — Central behavior policy

- Public type `hu_behavior_policy_t` with vtable `decide`, `deinit`, `name`.
- Default heuristic policy `hu_behavior_default_policy()` exposed as `hu_behavior_policy_t`.
- `hu_relational_act_t` enum: ANSWER, ACKNOWLEDGE, BACKCHANNEL, ASK_CLARIFY, REPAIR, REFLECT, VALIDATE, DISCLOSE_UNCERTAINTY, PUSH_BACK, BOUNDARY, PROMPT, WAIT, FOLLOW_UP, REFER_OUT, ABSTAIN.
- Inputs include affect, recent dialog acts, channel class, trust score, dependency risk, distress, memory contradictions.
- Defaults: safety wins over warmth, validate before answer when distressed, push back with calibrated uncertainty when memory contradicts, never advise when consent missing.

### B2 — Dialog acts + other-initiated repair

- `hu_dialog_act_t` enum: BACKCHANNEL, ACKNOWLEDGE, ANSWER, QUESTION, CLARIFY_QUESTION, REPAIR_INITIATE, REPAIR_ANSWER, REFLECTION, VALIDATION, ADVICE, REMINDER, DISAGREEMENT, BOUNDARY, ABSTENTION, GREETING, FAREWELL.
- `hu_dialog_act_classify(text)` heuristic (substring + punctuation + length).
- `hu_dialog_act_is_repair_initiation(text)` covers "huh?", "what?", "wait", "i don't follow", "say that again", "you mean…", short rising-intonation acknowledgements.
- B13 layers in prosodic features when wired through the voice pipeline.

### B3 — Continuous affect (VAD)

- `hu_affect_state_t {valence, arousal, dominance, uncertainty, modality, ts}`.
- `hu_affect_estimate_text(text, len, &state)` — small lexicon-driven baseline.
- `hu_affect_decay(state, now_ts, half_life_s)`.
- `hu_affect_fuse(prior, update, out)` — uncertainty-weighted average.
- `hu_affect_route_tier_score(state)` returns a model-router-friendly integer that replaces today's keyword-only routing.
- B12 will add audio/video producers behind the same `hu_affect_state_t`.

### B4 — Behavior-change engine

- `hu_bct_t` enum: GOAL_SETTING, ACTION_PLANNING, FEEDBACK_MONITORING, SELF_MONITORING, PROMPTS_CUES, REWARD, SOCIAL_SUPPORT, REDUCE_FRICTION, REFRAMING, BEHAVIORAL_REHEARSAL, HABIT_REVERSAL, VERBAL_PERSUASION (gated).
- `hu_fogg_state_t {motivation, ability, prompt_readiness}`.
- `hu_behavior_change_select(input, &decision)` returns a `hu_bct_t` plus `act_now` and `ask_permission_first` flags.
- Hard rules: never persuade when autonomy_risk > 0.3, never prompt outside reasonable hours unless explicitly opted in, never suggest behavior change while distress is escalating.

### B5 — Companion safety integration

- `hu_behavior_safety_input_t` carries `hu_companion_safety_result_t` (existing SHIELD-001), `hu_vulnerability_result_t` (existing), and an attachment-trajectory snapshot.
- `hu_behavior_safety_assess(input, out)` returns whether the policy must move to BOUNDARY, REFER_OUT, encourage human relationships, or pause behavior-change interventions.
- No duplication — the existing security module remains the source of truth.

### B6 — Persona evaluation harness

- `hu_persona_eval_question_t` array.
- `hu_persona_eval_run(persona, questions, n, responder, ud, &result)` calls a generic responder pointer (works in tests against a mock; works in CLI against a real provider).
- `hu_persona_eval_generate_baseline(persona, …)` builds questions from persona traits, vocab preferences, values, and decision style.
- Result captures contradictions, retest drifts, total/passed/failed, and the first failure for fast triage.
- Wired into `human persona eval` CLI in a follow-up commit; this PR ships the library + tests.

### B7 — LongMemEval scaffold

- Reuse W16 evaluation harness. Add `tests/test_eval_longmemeval.c` and `tools/eval_longmemeval/` skeleton (planned). Five tasks: information extraction, multi-session reasoning, temporal reasoning, knowledge updates, abstention.

### B8–B17 — design notes (scoped, not yet shipped)

- **B8** Theory of mind: extend `tests/test_theory_of_mind.c` with synthetic false-belief scenarios; baseline target ≥70% pass.
- **B9** User simulator: vtable `hu_user_sim_t` driven by a profile + memory + bounded-rationality action policy. Used to regression-test the assistant.
- **B10** Empathy/support-strategy labels: add a `hu_support_strategy_t` enum (validate, normalize, reframe, question, plan, ground, refer, boundary) consumed by B1.
- **B11** Trust calibration: add `hu_trust_policy_t` choosing answer / ask / abstain / cite memory / refuse, with sycophancy regression tests under user pressure and memory-implied preferences.
- **B12** Multimodal affect: implement `hu_affect_estimate_audio()` and a fusion path; wire into voice pipeline.
- **B13** Repair eval: corpus of repair scenarios (text + voice) with expected dialog act + relational act.
- **B14** Learned persona control: DPO/LoRA on local model — depends on M3 frontier bridge.
- **B15** Cultural pragmatics: add `directness`, `face_saving`, `disagreement_style`, `silence_tolerance` to overlays; respect explicit user preference only.
- **B16** Chronotype-aligned JITAI: extend `hu_circadian_*` to expose chronotype phase used by B4 decisions.
- **B17** On-device frontier behavior: hosted by M3 plan; the behavior layer is the consumer.

## Validation

Per-workstream tests live in `tests/test_behavior_*.c` and `tests/test_persona_eval.c`. Suites:

- `behavior_policy` — decision priority, safety override, distress validation, contradiction push-back, abstention.
- `behavior_dialog_act` — classification, repair detection, false-positive filter on narrative content.
- `behavior_affect` — VAD baseline, decay, fusion, routing score monotonicity.
- `behavior_change` — autonomy gating, Fogg ordering, late-night defer, manipulation refusal.
- `behavior_safety` — composition with SHIELD output, attachment trajectory escalation, referral path.
- `persona_eval` — contradiction detection, retest stability, baseline question generation.

All run inside `human_tests` and inherit ASan from the dev preset.

## Out of scope for this PR

- Wiring `hu_behavior_policy_t` into `agent_turn.c` and `daemon.c`. The wiring lands in a follow-up that swaps prompt-side heuristics for the policy; this PR delivers the building blocks, headers, and tests behind them.
- **Bridge helper (2026-05-10):** `hu_behavior_input_from_user_message()` in `policy.h` / `policy.c` fills `hu_behavior_input_t` from the latest user text (dialog act + affect + distress + `user_asked_question`) so the agent loop can call `hu_behavior_decide` in one shot when wiring lands.
- Real ML in B3 affect estimation. The lexicon baseline is intentionally small.
- Cultural pragmatic content libraries — only the schema lands later.

## Risks

- **Heuristic baselines look weak**. They are starting points; the behavior eval suite makes regressions visible before any frontier integration.
- **Overlap with existing modules**. We deliberately compose `companion_safety.c`, `mood.c`, `circadian.c`, and the voice duplex turn FSM rather than reimplement them. New names use `hu_relational_act_t`, `hu_dialog_act_t`, `hu_affect_state_t`, `hu_behavior_change_t`, `hu_behavior_safety_t`, and `hu_persona_eval_t` to avoid collision with `hu_turn_signal_t` / `hu_turn_action_t` / `hu_affect_mirror_*` already in the tree.
- **Behavior + manipulation risk**. B4 explicitly gates persuasion behind autonomy_risk and consent flags; B5 escalates dependency rather than reinforcing it.

## Follow-through

This plan is a child of `2026-05-10-master-follow-through-program.md`; phases there should be updated to reference Behavior v1 once landed.
