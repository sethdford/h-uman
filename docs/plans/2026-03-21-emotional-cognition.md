---
title: "Emotional cognition: runtime affect tracking and adaptive response"
created: 2026-03-21
status: complete
last_audit: 2026-05-17
---

## Summary

human already implements emotion-related capture, persistence, detection, and prompt hints across STM, deep memory, conversation heuristics, SkillForge, persona, and superhuman hooks. The product gap is **architectural**: there is no single per-turn object that (a) fuses those signals, (b) exposes a stable numeric affect model, and (c) drives skill routing, tone injection, and metacognitive policy in one place.

This plan proposes an **emotional cognition layer** centered on `hu_emotional_cognition_t`, produced once per turn and consumed by prompt building, skill catalog construction, and metacognitive monitoring. It aligns implementation with a three-stage mental model (perception → understanding → interaction) inspired by recent affective-LLM work, without adding speculative subsystems beyond what existing code already approximates.

**Non-goals (for this design):** replacing clinical assessment, mandatory LLM calls on every turn, or new external dependencies. All behavior stays C11, `hu_`-prefixed, heap-safe, and testable under ASan.

---

## Background / Research

### In-repo baseline

The runtime already provides:

| Area | Representative API / behavior |
|------|-------------------------------|
| Fast affect tags | `hu_fast_capture`, `hu_stm_turn_add_emotion` (`include/human/memory/fast_capture.h`, `include/human/memory/stm.h`) |
| Mood injection | `hu_mood_build_context` (`include/human/context/mood.h`) |
| Persisted affect | `hu_emotional_residue_*`, `hu_emotional_moment_*` (`include/human/memory/deep_memory.h`, SQLite engines) |
| Topic–emotion graph | `hu_egraph_*` (`src/memory/emotional_graph.c`) |
| Conversation-level detection | `hu_conversation_detect_emotion`, `_llm`, `hu_conversation_detect_energy`, `hu_conversation_build_sentiment_momentum`, `hu_conversation_detect_escalation` (`include/human/context/conversation.h`) |
| Superhuman hook | `hu_superhuman_observe_all` (`include/human/agent/superhuman.h`) |
| Prompt surface | `hu_prompt_config_t.tone_hint`, `conversation_context`, `superhuman_context`, persona-adjacent strings (`include/human/agent/prompt.h`) |
| In-turn sentiment notes | Keyword-style augmentation in `src/agent/agent_turn.c` |
| Skill surface | `hu_skillforge_build_prompt_catalog` — keyword `top_k` over user message only (`include/human/skillforge.h`) |

The **existing** `hu_emotional_state_t` today is intentionally small:

```c
typedef struct hu_emotional_state {
    float valence;
    float intensity;
    bool concerning;
    const char *dominant_emotion; /* often static / heuristic label */
} hu_emotional_state_t;
```

That type is wired into awareness and modifier builders (e.g. `hu_conversation_build_context_modifiers`). The cognition layer **extends** the information this struct carries (or wraps it during migration) rather than inventing parallel ad-hoc state in daemon and agent paths.

### External framing (lightweight)

- **Nano-EmoX** (arXiv:2603.02123): three-level hierarchy — *perception* (signals), *understanding* (structured interpretation), *interaction* (response policy). Maps cleanly onto: raw detectors → fused `hu_emotional_state_t` + trajectory → tone + skills + metacognition.
- **EmoLLM / appraisal graphs**: motivate **explicit reasoning about why** a label was chosen (confidence, source mix), not necessarily a full ARG in v1 — we can store `source_mask` + `confidence` and leave graph structures to `hu_egraph_*` / residue.
- **AIVA-style prompt engineering**: multimodal perception is out of scope for core C runtime; channel-provided metadata (e.g. voice prosody flags) can later feed the same fusion API as optional inputs.

---

## Design

### Layering

1. **Perception (inputs)** — unchanged detectors; called from a single orchestrator:
   - Heuristic / LLM: `hu_conversation_detect_emotion` / `_llm`
   - STM / fast capture: last turn tags and intensities
   - Memory: `hu_mood_build_context`, active `hu_emotional_residue_*`, due `hu_emotional_moment_*`
   - Graph: `hu_egraph_query` / `hu_egraph_build_context` when STM session graph is populated
   - Optional: `hu_superhuman_observe_all` output merged as advisory text + numeric hints if the registry exposes them

2. **Understanding (fusion)** — new logic in a dedicated module (suggested: `src/agent/emotional_cognition.c` + `include/human/agent/emotional_cognition.h`):
   - Weighted blend of valence, intensity, arousal proxies, primary tag, confidence
   - Trajectory vector from last *N* user turns (see below)
   - Normalization and clamping to documented ranges

3. **Interaction (consumers)** — same turn, read-only view:
   - **Tone:** map state → `hu_prompt_config_t.tone_hint` and/or a dedicated `emotional_tone_directive` string (prefer reusing `tone_hint` first to avoid prompt bloat)
   - **Skills:** `hu_skillforge_build_prompt_catalog` takes emotional state to bias `top_k` and ordering
   - **Metacognition:** pass pointer into the monitor so depth, verbosity caps, and “repair” triggers can tighten under rising distress

### Unified emotional state model

**Target shape** (names follow repo conventions; exact field order TBD for cache layout):

| Field | Type | Range / notes |
|-------|------|----------------|
| `primary_emotion` | `hu_emotion_tag_t` | Canonical tag enum (already used by STM / egraph) |
| `primary_label` | `const char *` | Optional human/LLM string; may alias static table for heuristics |
| `intensity` | `float` | `0.0f`–`1.0f` |
| `valence` | `float` | `-1.0f`–`1.0f` |
| `arousal` | `float` | `0.0f`–`1.0f` (derive from energy enum + keyword spikes + intensity) |
| `confidence` | `float` | `0.0f`–`1.0f` (agreement across sources) |
| `concerning` | `bool` | Preserve existing escalation / safety semantics |
| `source_mask` | `uint32_t` | Bitfield: heuristic, LLM, STM, mood, residue, egraph, superhuman |
| `trajectory_delta` | `float` | Signed change vs prior fused state (negative = worsening valence or rising distress) |

**Relationship to today’s `hu_emotional_state_t`:** extend the struct in `conversation.h` (or move both to `emotional_cognition.h` and typedef forward in conversation) so call sites get one type. Migration steps:

- Add new fields with safe defaults (e.g. `arousal = intensity`, `confidence = 0.5f` when only heuristics run).
- Map `dominant_emotion` → `primary_label` for one release, then deprecate the redundant pointer if all producers set `primary_emotion`.

### Emotion-aware skill routing

**Policy (configurable thresholds, sane defaults):**

| Condition | Skill bias (examples from emotional / relational registry) |
|-----------|---------------------------------------------------------------|
| High `intensity` + negative valence | Boost: `emotional-awareness`, `grief-support`, `patience`, `empathy-mapping` |
| Negative valence + high `arousal` | Boost: `active-listening`, `self-regulation`, de-escalation-adjacent skills |
| Positive valence + celebration lexicon / high energy | Boost: celebration / `gratitude-practice` (exact names from `human-skills/`) |
| Low confidence | Prefer generic skills; do not over-select niche affect skills |

**Integration:** extend `hu_skillforge_build_prompt_catalog`:

```c
hu_error_t hu_skillforge_build_prompt_catalog(
    hu_allocator_t *alloc,
    hu_skillforge_t *sf,
    const char *user_msg, size_t user_msg_len,
    const hu_emotional_state_t *emotion, /* NULL = current keyword-only behavior */
    char **out, size_t *out_len);
```

Implementation sketch: compute a small integer score per skill from (a) existing keyword match, (b) emotion→skill weight table, (c) optional manifest tag `emotion_tags[]` if later added to skill JSON — **v1 can use a static internal table** keyed by skill name to avoid schema churn.

### Adaptive tone injection

Replace scattered persona suffix strings in `agent_turn.c` with **one builder**:

`size_t hu_emotional_cognition_build_tone_directive(const hu_emotional_state_t *s, const hu_emotional_trajectory_t *traj, char *buf, size_t cap);`

**Rules (examples):**

- **High distress** (`concerning` or valence < −0.5 with intensity > 0.6): *“Be brief, warm, validating. Do not problem-solve unless asked.”*
- **High excitement** (positive valence, high arousal): *“Match their energy. Celebrate before advising.”*
- **Neutral / low signal:** no directive (empty string); do not duplicate `hu_tone_hint_string` casual/formal axis — **compose**: base tone from `hu_detect_tone`, affect overlay from cognition when non-empty.

Feed result into `hu_prompt_config_t.tone_hint` (concatenate with existing tone hint only if both non-empty; prefer single string built in one place to cap size).

### Emotional trajectory tracking

**Goal:** answer “is the user cooling down or spiraling?” within the session.

- **Window:** last 5 **user** turns (align with `hu_channel_history_entry_t` filtering).
- **Per-turn scalar:** fused `valence` (or a composite distress score = `intensity * (1 - valence)` for valence < 0).
- **Trajectory:** exponentially weighted moving average (EWMA) or linear regression slope on the 5 points; expose `trajectory_delta` and `bool trajectory_improving`.
- **Reuse:** incorporate `hu_conversation_detect_escalation` and `hu_conversation_build_sentiment_momentum` as **inputs** to fusion rather than separate prompt islands — their outputs contribute to `source_mask` and narrative directives until fully absorbed.

Suggested small struct:

```c
typedef struct hu_emotional_trajectory {
    float history_valence[5];
    float history_intensity[5];
    uint8_t count;
    float slope_valence;
    bool improving;  /* valence trending up or distress score trending down */
} hu_emotional_trajectory_t;
```

### Per-turn orchestrator: `hu_emotional_cognition_t`

Container for “everything emotional this turn” so daemon/agent do not duplicate glue code:

```c
typedef struct hu_emotional_cognition {
    hu_emotional_state_t fused;
    hu_emotional_trajectory_t trajectory;
    char *mood_context;           /* owned copy or borrowed with lifetime = turn arena */
    char *residue_directive;    /* optional; may borrow from existing builder */
    char *superhuman_note;      /* optional advisory */
} hu_emotional_cognition_t;
```

**Lifecycle:** initialize at turn entry; populate via `hu_emotional_cognition_update(...)`; pass pointer through prompt config assembly and skillforge; free or arena-reset at turn end.

---

## Structs & API (proposed)

| Symbol | Responsibility |
|--------|----------------|
| `hu_emotional_state_t` | Extended unified affect snapshot (see Design) |
| `hu_emotional_trajectory_t` | Sliding-window trajectory |
| `hu_emotional_cognition_t` | Per-turn bundle: fused state + trajectory + optional context strings |
| `hu_emotional_cognition_init` / `deinit` | Setup; `deinit` frees owned strings unless arena |
| `hu_emotional_cognition_update` | Main fusion: history, memory hooks, detectors |
| `hu_emotional_cognition_build_tone_directive` | Tone text for `tone_hint` |
| `hu_skillforge_build_prompt_catalog` | Extended signature with optional `const hu_emotional_state_t *` |

**Error handling:** fusion returns `HU_OK` with low-confidence neutral state on missing data; never fails the turn solely for emotion.

---

## Consolidation map

| Existing piece | Action |
|----------------|--------|
| `hu_conversation_detect_emotion` / `_llm` | **Keep**; become primary perception inputs to fusion |
| `hu_conversation_detect_energy` | **Keep**; map to `arousal` |
| `hu_conversation_detect_escalation` | **Keep**; feed trajectory + `concerning` |
| `hu_conversation_build_sentiment_momentum` | **Refactor** consumer: either internal to fusion or one contributor to `tone_hint` (avoid double injection) |
| `hu_conversation_build_context_modifiers` | **Keep** API; pass extended `hu_emotional_state_t` |
| `hu_mood_build_context` | **Keep**; string merged into cognition bundle |
| `hu_emotional_residue_*` / `hu_emotional_residue_build_directive` | **Keep**; directive attached to cognition |
| `hu_emotional_moment_*` | **Keep**; proactive follow-up remains separate timing-wise but can set flags on cognition |
| `hu_egraph_*` | **Keep**; optional valence/tag prior by topic |
| `hu_fast_capture` / `hu_stm_turn_add_emotion` | **Keep**; STM tags as high-trust short-term signal |
| `hu_superhuman_observe_all` | **Keep**; merge advisory into cognition (do not fork parallel prompt paths) |
| Persona sentiment append in `agent_turn.c` | **Remove/replace** with `hu_emotional_cognition_build_tone_directive` |
| `hu_skillforge_build_prompt_catalog` | **Refactor** signature + scoring |

---

## Integration points

1. **Daemon / agent turn pipeline** (`src/daemon.c`, `src/agent/agent_turn.c`): after history is available, call `hu_emotional_cognition_update` once; thread result into structures that already build `hu_prompt_config_t`.
2. **`hu_prompt_build_system` callers:** set `tone_hint` from cognition; ensure `conversation_context` still receives awareness text but **deduplicate** redundant `[Emotional context: ...]` fragments.
3. **SkillForge:** pass fused state into catalog builder so relational skills surface when affect demands it, not only when keywords match.
4. **Metacognitive monitor** (wherever depth / revision is decided — e.g. quality evaluation hooks in `conversation.c`): accept `const hu_emotional_cognition_t *` to tighten brevity or validation when `trajectory_delta` is strongly negative.
5. **Tests:** new `tests/test_emotional_cognition.c` for fusion math, threshold routing, and “no leak / no double free” under ASan; extend SkillForge tests for emotional `top_k` bias.

---

## Configuration

Proposed **optional** `config.json` block (names illustrative; schema work in implementation PR):

```json
"emotional_cognition": {
  "enabled": true,
  "trajectory_window": 5,
  "high_intensity_threshold": 0.65,
  "distress_valence_threshold": -0.45,
  "llm_detection": "auto",
  "skill_bias_strength": 0.35
}
```

- **`llm_detection`:** reuse existing daemon policy for when `_llm` is invoked; cognition layer only consumes result.
- **`skill_bias_strength`:** scale added score from emotion table (0 = legacy keyword-only).

Defaults must preserve today’s behavior when `enabled` is false.

---

## Testing

| Test type | Coverage |
|-----------|----------|
| Unit | Clamping, mask flags, trajectory slope on synthetic histories |
| Unit | Tone directive strings for boundary thresholds |
| Unit | Skill ordering changes when `hu_emotional_state_t` simulates grief vs neutral |
| Integration | Single turn through prompt builder: assert one emotional directive channel |
| ASan / leak | All allocated strings in `hu_emotional_cognition_t` freed or arena-scoped |
| Regression | Existing `test_stm.c`, `test_mood.c`, `test_emotional_moments.c`, `test_emotional_graph.c`, conversation emotion tests still pass |

Determinism: no new network; LLM paths remain behind existing `HU_IS_TEST` / provider guards.

---

## Risks

| Risk | Mitigation |
|------|------------|
| Prompt ballooning from merged directives | Single tone builder; max length cap; dedupe with awareness |
| False positives route “therapy” skills in technical threads | Require combined score (keyword + affect + intensity); cap bias |
| Struct growth / ABI | All in-tree; single migration PR; default-initialize new fields |
| LLM detector latency | Keep heuristic default; LLM optional per config |
| Overfitting to English heuristics | Document limitation; leave hooks for channel-provided affect metadata |

---

## References

- Nano-EmoX: arXiv:2603.02123 (perception / understanding / interaction hierarchy).
- EmoLLM / appraisal-style EQ reasoning (IQ–EQ co-reasoning, appraisal graphs) — conceptual reference for confidence + structured affect.
- AIVA (multimodal sentiment + emotion-aware prompts) — conceptual reference for prompt-side adaptation; multimodal inputs optional later.
- In-repo: `include/human/context/conversation.h`, `include/human/agent/prompt.h`, `include/human/skillforge.h`, `src/agent/agent_turn.c`, `src/memory/emotional_graph.c`, `docs/plans/2026-03-10-human-fidelity-phase2-emotional-intelligence.md` (prior phase alignment, if still current).
