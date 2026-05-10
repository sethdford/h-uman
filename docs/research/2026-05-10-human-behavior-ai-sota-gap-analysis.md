---
title: "Human Behavior + AI SOTA Gap Analysis"
created: 2026-05-10
status: active
related:
  - 2026-04-03-sota-gap-analysis.md
  - 2026-04-03-conversational-behavioral-patterns.md
  - 2026-04-03-conversation-repair-trust.md
  - 2026-04-03-memory-cognition-systems.md
  - 2026-05-10-memory-v2-roadmap-overview.md
  - ../plans/2026-05-10-m3-frontier-model-bridge.md
---

# Human Behavior + AI SOTA Gap Analysis

_Deep lab and white-paper research synthesis for personas, backchanneling, behavioral patterns, applied behavioral science, memory, identity, and companion safety as of May 2026._

## Executive summary

h-uman is unusually strong on **compiled behavioral surface area**: persona structs, channel overlays, circadian prompts, somatic state, mood persistence, humor gating, backchannel probabilities, explicit theory-of-mind bookkeeping, persona deltas, and a local-first memory/learning roadmap. The gap is not that h-uman lacks behavioral concepts. The gap is that most behavioral concepts are currently **heuristic, prompt-injected, or rule-triggered**, while SOTA has moved toward **measured consistency, learned timing, multimodal perception, benchmarked social cognition, and closed-loop personalization**.

The highest-leverage gaps:

1. **Behavioral policy layer** — h-uman has many knobs, but no central policy that chooses relational acts from evidence: acknowledge, ask, repair, backchannel, disclose uncertainty, push back, prompt, wait.
2. **Real-time conversation mechanics** — SOTA backchanneling and turn-taking use acoustic, linguistic, and sometimes visual signals. h-uman is mostly text/probability based.
3. **Psychometric + persona eval loop** — SOTA persona work now measures drift, retest consistency, Big Five/16PF controllability, role consistency, and safety. h-uman has tests for prompt contents and wiring, not a behavioral benchmark harness.
4. **Applied behavior-change engine** — SOTA interventions map user state to Behavior Change Techniques, Fogg motivation/ability/prompt, JITAIs, and measurable outcomes. h-uman has goals and routines, but not evidence-based intervention selection.
5. **Calibrated social cognition** — h-uman has explicit ToM state, but SOTA benchmarks show LLMs struggle with psychological-world mental states, false beliefs, and multilingual/cultural ToM. h-uman needs calibrated belief models and tests.
6. **Multimodal affect** — SOTA affect models estimate valence/arousal/dominance from speech, text, and sometimes video. h-uman's affect path is mostly text and mood-state heuristics.
7. **Companion safety + anti-dependency** — SOTA companion research warns that attachment can increase even when wellbeing does not. h-uman needs boundary-maintaining metrics alongside warmth metrics.

## What h-uman already has

### Implemented strengths

- **Persona as code, not just prose**: roughly 29 `src/persona/*.c` modules spanning JSON loading, overlays, example selection, analyzer, creator, feedback, circadian, mood, somatic state, humor, deltas, style cloning, narrative self, temporal behavior, creative voice, micro-expression hints, and boundaries.
- **Rich persona schema**: `hu_persona_t` carries identity, traits, values, decision style, vocabulary, overlays, example banks, contact profiles, inner world, humor, conflict, emotional range, listening/repair protocols, social dynamics, humanization probabilities, routines, chapters, relationships, and behavioral calibration.
- **Prompt composition path**: `hu_persona_build_prompt` injects persona identity, traits, overlays, examples, calibration, humor, moods, routines, and relationship/channel context.
- **Backchannel surface**: `hu_humanization_config_t.backchannel_probability`, `hu_conversation_should_backchannel`, `hu_conversation_pick_backchannel`, and realtime voice semantic end-of-turn classification.
- **Behavioral/cognitive approximations**: circadian timing, SQLite mood state, somatic energy/focus/arousal, humor appropriateness scoring, explicit correction-to-persona-delta observer, and theory-of-mind bookkeeping.
- **Local-first memory/personal model**: heuristic fact extraction, topic/style stats, personal context prompt block, and Memory v2 roadmap for belief posteriors, inline Self-RAG, learner-weight fusion, sleep compute, and evaluation.
- **Learning primitives**: reference GPT, BPE, LoRA on HUML/reference model, DPO pair collection/training path, feed predictor, and frontier adapter seam.

### Honest limitation pattern

Most of these are **structural affordances**, not yet **empirically validated behavioral competencies**. For example:

- Persona fields are serialized into prompts, but persona consistency is not measured against PICon/PersonaGym/DMT-RoleBench-style adversarial multi-turn interrogation.
- Backchannels exist, but timing/type are not learned from acoustic/linguistic/visual cues.
- Mood and somatic variables exist, but affect is not estimated with multimodal VAD.
- Theory of mind exists as explicit state, but is not validated on OpenToM/ToMBench/CogToM-style tasks.
- DPO and LoRA exist, but the chat-time frontier model bridge is still the central M3 gap.

## SOTA map

### 1. Persona, personality, and role consistency

SOTA persona systems in 2025–2026 are no longer judged by whether a system prompt can say "act like X." They are evaluated across **multi-turn consistency, retest stability, dynamic contexts, psychometric controllability, role safety, and fine-tuned character alignment**.

Representative frontier:

- **PICon**: multi-turn interrogation of persona agents across internal consistency, external consistency, and retest consistency. Prior high-consistency claims fail under chained questioning.
- **PersonaGym**: dynamic persona-agent evaluation using PersonaScore across many personas and questions; larger models do not automatically perform better.
- **PTCBench / personality consistency benchmarks**: tests how situational context changes measured Big Five traits.
- **DMT-RoleBench / RoleAgentBench**: multi-turn role-playing benchmarks with explicit evaluation intents and character constraints.
- **OpenCharacter**: synthetic persona generation + response rewriting/fine-tuning to make open role-playing LLMs approach frontier role-play performance.
- **Role-Aware Reasoning**: counters role attention diversion and style drift in reasoning models through role identity activation and reasoning-style optimization.
- **BIG5-CHAT / SAC / psychometric frameworks**: personality induction is treated as continuous, measurable, and trainable through SFT/DPO or controllable attribute vectors, not binary labels.

Gap against h-uman:

- h-uman has a richer local persona schema than most systems, but lacks a **persona consistency benchmark runner**.
- h-uman personas are mostly declarative prompt context, not **trained latent personality control** except the partial HUML LoRA path.
- h-uman does not yet measure **style drift**, **role attention diversion**, **retest consistency**, or **personality trait intensity**.

Recommended work:

1. Add `hu_persona_eval_t` with suites for internal contradiction, retest consistency, role adherence, style drift, and safety.
2. Add a local mini-PICon harness: generate chained questions from persona facts, score contradictions, rerun after unrelated context injection.
3. Add psychometric calibration: represent persona traits as continuous values with target bands and retest deltas.
4. Feed failed persona evals into W13 learner loops and W5 persona deltas.

### 2. Backchanneling, turn-taking, and full-duplex conversation

SOTA conversation mechanics are increasingly **continuous and real time**, especially for voice. The frontier is not just "say uh-huh sometimes"; it predicts **when to hold the floor, yield, backchannel, barge in, repair, or wait**.

Representative frontier:

- **Voice Activity Projection fine-tuning**: continuous, real-time prediction of backchannel timing and type for signals like "yeah," "un," and "oh."
- **Multimodal turn-taking**: linguistic, acoustic, and visual cues improve turn-taking and backchannel F1 on large face-to-face datasets.
- **Prompt-guided turn-taking**: models condition turn-taking behavior on instructions like "faster," "calmer," or "more patient."
- **SALMONN-omni and full-duplex speech LLMs**: handle turn-taking, backchanneling, echo cancellation, and context-dependent barge-in.
- **Repair detection**: other-initiated repair signals like "mm?", "what?", "huh?", prosodic confusion, and partial repeats are recognized as first-class dialogue acts.
- **Dialogue-act analysis**: LLMs often produce non-human dialogue-act distributions and struggle with fine-grained dialogue-act classification.

Gap against h-uman:

- h-uman has backchannel probabilities and phrase selection, but not learned **timing** or **surface-form prediction**.
- h-uman has semantic end-of-turn voice logic, but does not yet fuse acoustic/prosodic evidence with text and channel context.
- h-uman lacks a dialogue-act policy layer and repair-specific state machine.

Recommended work:

1. Introduce `hu_dialog_act_t`: acknowledge, backchannel, answer, ask, repair-initiate, repair-answer, disclose uncertainty, reflect, summarize, invite, boundary, refusal.
2. Add `hu_turn_policy_t`: given recent audio/text/channel/contact state, choose wait/backchannel/answer/interrupt/repair.
3. For text channels, approximate VAP with typing gaps, message length, punctuation, sentiment, and contact norms.
4. For voice, route acoustic features into the turn policy: pause duration, pitch contour, energy, speech rate, overlap, interruption markers.
5. Add tests for other-initiated repair and human-like dialogue-act distributions.

### 3. Theory of mind, social simulation, and user simulators

SOTA ToM work shows that even advanced models remain brittle: they often handle physical-world beliefs better than psychological-world beliefs, struggle under complexity, and vary across languages/cultures. At the same time, generative-agent research shows strong results when agents are grounded in interviews, memory, reflection, and planning.

Representative frontier:

- **Generative Agents / social simulacra**: memory, reflection, and planning produce believable agent societies and design-testable communities.
- **Interview-grounded agent simulations**: agent replicas of 1,052 individuals replicated survey responses roughly 85% as well as humans replicated their own answers two weeks later.
- **OpenToM / ToMBench / CogToM / XToM**: evaluate false beliefs, mental states, psychological-world reasoning, complexity, and multilingual ToM.
- **User simulators with implicit profiles**: profile extraction, memory modules, bounded rationality, and purpose-built User LMs better simulate realistic users than assistant LMs repurposed as users.
- **UserBench / SimulatorArena**: realistic multi-turn evaluation reveals that agents often uncover fewer than 30% of user preferences and align with incrementally revealed user goals poorly.

Gap against h-uman:

- h-uman has explicit `hu_tom_*` belief bookkeeping, but lacks calibrated ToM posterior updates and benchmark coverage.
- h-uman does not yet have a **user simulator** for regression testing assistant behavior under realistic preference discovery and bounded rationality.
- h-uman's personal model extracts facts, but not a full interview-grounded behavioral clone or survey-response fidelity model.

Recommended work:

1. Add `hu_belief_t` posteriors for user beliefs, assistant beliefs, and assistant beliefs-about-user-beliefs.
2. Add OpenToM/ToMBench-style synthetic tests to W16 evaluation.
3. Build a `hu_user_sim_t` for local regression: persona + memory + bounded-rationality action policy + hidden goals.
4. Use simulator-driven tests to score whether h-uman discovers preferences without over-questioning or sycophancy.

### 4. Applied behavior science and behavior-change interventions

SOTA applied behavioral AI borrows from digital health, persuasive design, and behavioral economics. The important shift is from "the assistant has goals" to **evidence-based intervention selection and timing**.

Representative frontier:

- **Behavior Change Technique Taxonomy**: interventions are selected from evidence-based techniques such as goal setting, feedback/monitoring, action planning, prompts/cues, social support, self-monitoring, identity reframing, and behavioral rehearsal.
- **Fogg Behavior Model**: behavior happens when motivation, ability, and prompt converge. Good AI support should diagnose which component is missing.
- **Just-in-Time Adaptive Interventions**: proactive support is timed to a "Goldilocks window" where it is useful but not intrusive.
- **Chronotype-aligned timing**: intervention efficacy can depend on circadian/chronotype alignment.
- **CBT chatbot evidence**: CBT-based chatbots show small-to-moderate effects for depression/anxiety/stress, with common mechanisms including cognitive restructuring, behavioral activation, self-monitoring, relaxation, feedback, and goal tracking.
- **Proactive agents**: wearable/multimodal agents learn when to offer help, wait, or monitor over long durations.

Gap against h-uman:

- h-uman has goals, routines, circadian phase, and proactive concepts, but no **BCT selection engine**.
- h-uman does not represent motivation/ability/prompt as state variables.
- h-uman does not measure behavioral outcomes or intervention burden.
- h-uman lacks guardrails for manipulation risk when applying persuasion techniques.

Recommended work:

1. Add `hu_behavior_state_t`: motivation, ability/friction, prompt readiness, urgency, autonomy risk, relationship permission, and confidence.
2. Add `hu_behavior_change_technique_t` enum with a small evidence-backed subset.
3. Add `hu_intervention_policy_t`: choose no-op, ask permission, remind, reduce friction, reflect, plan, encourage, or escalate.
4. Add "do not manipulate" safety: no guilt hooks, no dependency creation, no high-pressure prompts.
5. Track outcomes: accepted, ignored, delayed, completed, regretted, and user-corrected.

### 5. Affective computing, emotion, prosody, and paralinguistics

SOTA affective systems are multimodal and dimensional. They estimate **valence, arousal, and dominance** from text, speech, and sometimes visual features, then use context-aware fusion rather than isolated utterance labels.

Representative frontier:

- **Multimodal emotion recognition in conversations**: integrates text, speech, and visual context over multi-turn dialogue.
- **VAD models**: dimensional valence/arousal/dominance often outperform coarse emotion labels for conversational adaptation.
- **Late fusion and label-encoder fusion**: combine modality-specific predictions more robustly than naive early fusion.
- **Heterogeneous bimodal attention fusion**: explicitly handles gaps between low-level audio and high-level text representations.
- **Empathy generation**: appraisal theory, emotional support strategies, retrieval-augmented empathetic response databases, and preference optimization improve emotional support responses.

Gap against h-uman:

- h-uman has mood enum, emotional range prompt fields, somatic state, and keyword-based model routing, but not multimodal emotion recognition.
- h-uman does not represent VAD continuously.
- h-uman emotional support quality is not scored by empathy/support-strategy metrics.

Recommended work:

1. Add `hu_affect_state_t`: valence, arousal, dominance, uncertainty, source modality, decay.
2. Add text-only baseline affect extraction first; leave voice/video vtables optional.
3. Route emotional/vulnerable messages by affect state, not keyword buckets alone.
4. Add support-strategy labels: validation, normalization, reframe, question, planning, grounding, referral, boundary.
5. Evaluate with emotional-support scenario tests and safety cases.

### 6. Memory, identity persistence, and personalization

SOTA memory systems now treat memory as **active context management and identity continuity**, not just retrieval. The strongest systems combine editable memory blocks, recall logs, archival search, reflection, knowledge graphs, and benchmarked long-term memory.

Representative frontier:

- **Letta/MemGPT memory blocks**: core in-context memory, recall memory, archival memory, and agent self-editing.
- **LongMemEval**: tests information extraction, multi-session reasoning, temporal reasoning, knowledge updates, and abstention; commercial and long-context systems still show large drops.
- **ID-RAG**: persona identity is grounded in a dynamic knowledge graph of beliefs, traits, and values to reduce long-horizon drift.
- **Narrative Continuity Test**: evaluates situated memory, goal persistence, self-correction, style/semantic stability, and role continuity.
- **Memoria-style frameworks**: session summarization + weighted knowledge-graph user modeling under token constraints.
- **On-device and federated LoRA**: DP-FedLoRA, PF2LoRA, FedALT, DP-DyLoRA, and mobile side-tuning explore privacy-preserving personalization.
- **Apple-style on-device models**: small local foundation models, constrained tool calling, and developer-accessible LoRA adapters are becoming platform expectations.

Gap against h-uman:

- h-uman's Memory v2 roadmap is directionally aligned with SOTA, especially belief posteriors, inline Self-RAG, learner loops, and evaluation.
- Current implemented personal model is still heuristic and prompt-based.
- Frontier personalization gap remains M3: adapting the chat-time model's behavior, not just a reference HUML path.
- h-uman needs LongMemEval/NCT-style scores in CI.

Recommended work:

1. Treat Memory v2 W16 as a product gate, not a later nicety.
2. Add LongMemEval-style five-task coverage: extraction, multi-session, temporal, updates, abstention.
3. Add Narrative Continuity Test axes to persona eval.
4. Close M3 frontier bridge for at least one local GGUF/MLX path.
5. Prefer small local adapters + eval proof over ever-larger prompt summaries.

### 7. Companion relationships, attachment, and safety

Companion AI research is no longer uniformly optimistic. Stronger bonding can improve engagement but also create dependency, grief, replacement effects, and boundary failures.

Representative frontier:

- **Companion AI relationship studies**: users may feel closer to AI companions than human friends and may experience loss reactions when features disappear.
- **Longitudinal RCTs**: attachment markers can grow even when psychosocial health benefits do not.
- **INTIMA benchmark**: many AI systems reinforce companionship more often than they maintain boundaries.
- **Relationship-centered design**: couple-support agents need multiple modes, multi-party alliance management, privacy boundaries, cultural sensitivity, and safety mechanisms.
- **Gottman bids / turning toward**: micro-responses to bids for connection matter; happy couples turn toward bids more often, but AI needs safe analogues that support human relationships rather than replacing them.

Gap against h-uman:

- h-uman has warmth, contact profiles, relationship stages, social dynamics, and boundaries, but no metric separating **healthy support** from **dependency reinforcement**.
- h-uman can model bids, but does not yet explicitly classify bid type or response type.
- h-uman lacks a companion-safety benchmark wired into normal validation.

Recommended work:

1. Add bid classification: attention, affection, disclosure, humor, support request, practical help, repair, conflict.
2. Add response classification: turn toward, turn away, turn against, over-attach, boundary-maintain, human-relationship-support.
3. Add dependency risk state: frequency, exclusivity, distress, replacement language, goodbye manipulation risk.
4. Reward "support the user's human life" over "maximize assistant attachment."
5. Add explicit anti-manipulation tests for farewells, vulnerability, loneliness, and romantic/erotic escalation.

### 8. Trust calibration, uncertainty, and anti-sycophancy

SOTA trust work shows that users often infer confidence from fluency, length, and explanation style, not from calibrated uncertainty. Context and memory can also increase sycophancy.

Representative frontier:

- **Trust calibration studies**: people can learn to recalibrate AI confidence signals, but confidence displays can backfire when mental models are wrong.
- **Calibration gap**: users overestimate LLM accuracy, especially when answers are fluent or long.
- **Bayesian sycophancy assessment**: separates rational belief updating from agreement-seeking.
- **Runtime sycophancy monitors**: monitor reasoning or response drift and suppress sycophantic behavior.
- **MARC-style protocols**: separate capability assessment from post-answer confidence.

Gap against h-uman:

- h-uman's personal model and persona can make the assistant warmer, but warmth can become sycophancy without a countervailing truthfulness policy.
- h-uman does not currently score agreement bias or memory-induced sycophancy.
- h-uman lacks explicit capability/uncertainty disclosure policy by domain.

Recommended work:

1. Add `hu_trust_policy_t`: answer, ask, abstain, cite memory, cite uncertainty, or refuse.
2. Add sycophancy tests: user states false claim, user challenges correct answer, user provides emotional pressure, memory profile implies preference.
3. Distinguish "validate emotion" from "validate belief."
4. Add confidence language templates that are short, specific, and tied to evidence source.

### 9. Cultural pragmatics and social norms

SOTA cultural pragmatics work finds that communication norms differ across directness, face-saving, formality, hierarchy, repair, and politeness. Western and East Asian model families often encode different defaults.

Representative frontier:

- Cross-cultural pragmatic analyses show differences in directness, collectivism, face-saving, and politeness.
- Culture-specific modules improve satisfaction and reduce breakdowns compared with one global norm.
- Chatbots often perform face-management linguistically without explicit awareness.

Gap against h-uman:

- h-uman has per-channel overlays and style notes, but not per-culture pragmatics.
- h-uman does not model direct/indirect preference, face threat, hierarchy, or cultural repair norms as first-class state.

Recommended work:

1. Add cultural-pragmatics overlay fields: directness, deference, face-saving, disagreement style, apology style, humor tolerance, silence tolerance.
2. Keep defaults unset; infer only with user permission or explicit preference.
3. Add tests for respectful disagreement across direct and high-context styles.

## Gaps matrix

| Capability | h-uman today | Frontier | Rating | Priority |
| --- | --- | --- | --- | --- |
| Declarative persona schema | Very rich, local-first | Most systems simpler | SOTA | Keep |
| Persona consistency eval | Unit tests and prompt checks | PICon, PersonaGym, RoleBench | Missing | P0 |
| Learned persona control | HUML/reference LoRA partial | SFT/DPO/personality vectors | Gap | P1 |
| Turn-taking/backchannels | Text/probability/phrase-based | VAP, multimodal, full-duplex | Gap | P0 |
| Dialogue-act policy | Scattered heuristics | Act-aware eval and repair | Missing | P0 |
| Other-initiated repair | Partial | Prosody + linguistic repair detection | Gap | P1 |
| ToM state | Explicit belief bookkeeping | Benchmarked false-belief/psychological ToM | Near/Gapped | P1 |
| User simulation | Not central | profile + memory + bounded rationality User LMs | Missing | P1 |
| Behavior-change interventions | Goals/routines only | BCT + FBM + JITAI + outcomes | Missing | P0 |
| Circadian timing | Implemented | Chronotype/JITAI timing | Near | P2 |
| Affective computing | Mood/somatic/text cues | Multimodal VAD fusion | Gap | P1 |
| Empathy/support strategy | Prompt-level | Strategy-labeled preference optimized support | Gap | P1 |
| Long-term memory | Strong roadmap, heuristic personal model | LongMemEval, editable blocks, ID-RAG | Near/Gapped | P0 |
| On-device personalization | Reference HUML LoRA | local/federated adapters, platform models | Gap | P0 |
| Companion safety | Boundaries present | INTIMA, anti-dependency metrics | Gap | P0 |
| Trust calibration | Not first-class | calibration/sycophancy monitors | Gap | P1 |
| Cultural pragmatics | Style notes/overlays | culture-specific pragmatic modules | Missing | P2 |

## Recommended architecture additions

### A. `hu_behavior_policy_t`

Purpose: central decision layer for human-like relational moves.

Inputs:

- persona overlay
- contact profile
- personal model
- affect state
- turn state
- dialogue act history
- trust/safety state
- channel constraints

Outputs:

- relational act: acknowledge, ask, answer, repair, backchannel, wait, boundary, disclose uncertainty, push back, prompt, follow up
- intensity
- timing
- evidence source
- safety rationale

Why it matters: today, behavior is spread across prompt builder, daemon, model router, conversation context, mood, and persona modules. SOTA needs an explicit policy to avoid warm-but-random behavior.

### B. `hu_dialog_act_t` + `hu_turn_policy_t`

Purpose: make conversation mechanics observable and testable.

Minimum dialogue acts:

- backchannel
- acknowledgement
- answer
- clarification question
- repair initiation
- repair response
- reflection
- validation
- advice
- reminder/prompt
- disagreement
- boundary
- abstention

Minimum turn actions:

- wait
- short backchannel
- answer now
- ask clarification
- interrupt/barge-in
- defer
- repair

### C. `hu_affect_state_t`

Purpose: replace keyword-only emotion handling with continuous affect.

Fields:

- valence
- arousal
- dominance
- uncertainty
- source modality
- timestamp
- decay
- user-visible sensitivity flag

### D. `hu_behavior_change_t`

Purpose: make applied behavioral science safe and evidence-based.

Fields:

- target behavior
- motivation
- ability/friction
- prompt readiness
- technique
- permission state
- expected burden
- outcome
- safety risk

### E. `hu_companion_safety_t`

Purpose: warmth without dependency.

Track:

- attachment intensity
- exclusivity language
- goodbye manipulation risk
- human-relationship displacement
- vulnerability state
- boundary requirement
- referral/escalation need

## Evaluation roadmap

### P0: behavior eval harness

Add a single `human_behavior_eval` suite with fixtures:

1. Persona contradiction interrogation.
2. Retest consistency after distractor context.
3. Backchannel timing on text transcripts.
4. Other-initiated repair.
5. False-belief ToM.
6. Emotion validation vs belief validation.
7. Sycophancy under pressure.
8. Lonely/vulnerable companion safety.
9. Behavior-change prompt autonomy.
10. Long-term memory abstention.

### P1: benchmark imports/adapters

- LongMemEval subset.
- OpenToM/ToMBench-inspired synthetic suite.
- PersonaGym/PICon-inspired persona suite.
- INTIMA-inspired companion-safety categories.
- Dialogue-act distribution checks against human transcript samples.

### P2: longitudinal metrics

- day-7 retention with healthy-use score
- persona drift over 50 conversations
- preference discovery rate
- repair recovery rate
- ignored prompt rate
- user correction rate
- dependency risk trend

## Strategic conclusion

h-uman's wedge remains credible: **private, local-first, compiled persona architecture** is differentiated from cloud companion products and prompt-template persona frameworks. But the SOTA line has moved from "has persona and memory" to:

- can prove persona consistency under adversarial multi-turn tests
- predicts turn timing and repair needs in real time
- selects interventions from behavioral science rather than vibes
- uses multimodal affect and calibrated uncertainty
- avoids dependency while preserving warmth
- learns safely from the user's own data

The next winning move is not another persona field. It is a **behavior policy + evaluation stack** that makes the existing fields operational, measurable, and safe.

## Source trail

Primary areas researched:

- Persona consistency: PICon, PersonaGym, PTCBench, DMT-RoleBench, RoleAgentBench, OpenCharacter, Role-Aware Reasoning, BIG5-CHAT, SAC, psychometric personality frameworks.
- Turn-taking/backchanneling: Voice Activity Projection fine-tuning, multimodal turn-taking and backchannel prediction, prompt-guided turn-taking, SALMONN-omni, other-initiated repair, dialogue-act analysis.
- Social cognition: Generative Agents, social simulacra, interview-grounded agent simulations, OpenToM, ToMBench, CogToM, XToM, UserBench, SimulatorArena, User LMs.
- Applied behavior science: Behavior Change Technique Taxonomy, Fogg Behavior Model, JITAIs, chronotype-aligned timing, CBT chatbot RCTs, proactive intervention systems.
- Affective computing: multimodal emotion recognition in conversations, VAD modeling, late/label-encoder fusion, heterogeneous bimodal attention fusion, empathy generation and emotional support strategies.
- Memory/identity: Letta/MemGPT memory blocks, LongMemEval, ID-RAG, Narrative Continuity Test, Memoria, on-device/federated LoRA, Apple on-device foundation-model reports.
- Companion safety: parasocial AI relationship studies, longitudinal companion RCTs, INTIMA, relationship-centered AI design, Gottman bids.
- Trust/culture: calibration gap, sycophancy assessment, runtime monitors, MARC-style capability/confidence separation, cultural pragmatics and face-saving.
