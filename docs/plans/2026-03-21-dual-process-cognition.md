---
title: "Dual-process cognition: System 1 / System 2 mode switching"
created: 2026-03-21
status: closed
last_audit: 2026-05-25
---

# Dual-process cognition: System 1 / System 2 mode switching

## Summary

Introduce a **metacognitive dispatcher** that classifies each user turn into a **cognition mode** (`fast`, `slow`, or `emotional`) and drives **depth-gated** processing inside `hu_agent_turn()`. **System 1** minimizes retrieval, prompt mass, planning, and tool iterations for latency-sensitive turns. **System 2** preserves today’s full pipeline for hard problems. **Emotional** is a slow-path variant that prioritizes empathy, relationship context, and (when available) persona overlays without sacrificing safety.

Goals:

- Cut average turn cost (tokens, wall time) on trivial user messages.
- Preserve quality and grounding on complex, high-stakes, or emotionally loaded turns.
- Keep the implementation **vtable-friendly**, **ASan-clean**, and **test-deterministic** via `HU_IS_TEST`.

Non-goals (for this proposal):

- Replacing the security input path (`hu_input_guard_check`).
- Changing provider or tool vtables.
- Guaranteeing sub-500 ms end-to-end latency (provider/network dominates); targets are **runtime budgets** for local work and **prompt/token budgets**.

---

## Background/Research

### Dual-process theory (product metaphor)

Kahneman’s System 1 / System 2 distinction is a **useful UX metaphor**, not a literal model of human cognition. In software terms:

- **System 1** ≈ cheap heuristics, shallow context, few branches, bounded tool use.
- **System 2** ≈ deliberate reasoning: richer retrieval, planning, reflection, more tool iterations.

The runtime already contains partial analogues:

- **Self-RAG** (`hu_srag_*` in `agent_turn.c`) gates retrieval before `hu_memory_loader_load`.
- **Automatic planning** (`hu_planner_generate` / `hu_plan_executor_run`) runs for long messages with sufficient tools (`#ifndef HU_IS_TEST`).
- **Fast capture** (`hu_fast_capture`) and **superhuman observation** (`hu_superhuman_observe_all`) extract lightweight structure early in the turn.
- **`hu_prompt_config_t`** (`include/human/agent/prompt.h`) is the natural lever for “how much system prompt” each mode receives; `hu_prompt_build_static` / caching already exist for reuse.

### Related code (anchors)

| Area | Location |
|------|-----------|
| Main turn | `hu_agent_turn()` in `src/agent/agent_turn.c` |
| Prompt assembly | `hu_prompt_build_system`, `hu_prompt_config_t` in `include/human/agent/prompt.h`, implementation in `src/agent/prompt.c` |
| Streaming turn | `hu_agent_turn_stream()` in `src/agent/agent_stream.c` (must stay consistent or explicitly diverge) |
| Turn signals | `hu_fast_capture`, STM / pattern radar, `hu_detect_tone`, Self-RAG |
| Observability | `hu_observer_record_event` / `hu_observer_event_tag_t` in `include/human/observer.h` |
| Behavioral counters | `hu_bth_metrics_t` in `include/human/observability/bth_metrics.h` |

---

## Design

### Modes and semantics

| Mode | Enum | User-visible intent | Processing depth |
|------|------|---------------------|------------------|
| System 1 | `HU_COGNITION_MODE_FAST` | Acknowledgments, tiny talk, single-fact lookups | Minimal prompt blocks; retrieval skipped or shallow; **max 1** provider tool iteration; planner off |
| System 2 | `HU_COGNITION_MODE_SLOW` | Multi-step reasoning, research, coding tasks | Current full pipeline (subject to existing gates) |
| Emotional | `HU_COGNITION_MODE_EMOTIONAL` | Distress, grief, conflict, strong affect | Slow path + **mandatory** empathy-related context blocks; may still use tools but with stricter policy emphasis |

**Emotional** is not a separate “fast” lane; it is **slow-quality** with different **prompt composition** and **dispatcher triggers** (high affect, relationship-sensitive topics, escalation phrases). It may reuse the same tool-iteration cap as slow unless config overrides.

### Metacognitive dispatcher

**Inputs** (all optional where noted; missing data lowers confidence and biases toward slow):

- `msg`, `msg_len`
- Last **2–3** user/assistant turns from `agent->history` (roles + lengths)
- **Emotional state**: from `hu_fast_capture` emotion tags / intensities, `hu_superhuman` signals if exposed, or `hu_conversation_detect_emotion()`-style APIs where already integrated
- **Message length**, simple **complexity heuristics** (e.g. multistep cues — the file already has `message_looks_multistep_for_orchestrator`)
- **Channel** (`agent->active_channel`) for formality / crisis sensitivity
- **Self-RAG assessment** (once available in the turn) as a *secondary* vote, not the only gate

**Outputs**:

- `hu_cognition_mode_t mode`
- `float confidence` in **[0.0, 1.0]**
- Optional `hu_cognition_flags_t` bitset (e.g. “force retrieval”, “suppress chain-of-thought”)

**Performance target**: **under 50 ms** CPU time on native for the default **heuristic** backend on typical messages (no network, no provider call). Embedding or micro-LLM backends may exceed that; they must be opt-in and budgeted separately in config.

**Backend options** (vtable-driven):

1. **Heuristic rules** (default): keywords, length buckets, emotion thresholds, multistep detectors, “?” density, code-block detection, attachment flags from channel metadata if present.
2. **Embedding classifier**: compare message embedding to centroid prototypes (requires optional embedding path; guarded by build flags and `HU_IS_TEST` stubs).
3. **Cheap LLM call**: ultra-small model / secondary provider — **off by default**, only when explicitly configured; must use `HU_IS_TEST` short-circuit to a fixed mode in tests.

**Safety**: If confidence is below the configured threshold or the classifier errors, **default to `HU_COGNITION_MODE_SLOW`**. Injection-high-risk path remains unchanged (already returns before the main pipeline).

### System 1 prompt budget (~2K tokens target)

Not a hard C limit initially—enforce via **structured omission** of `hu_prompt_config_t` fields and **skills_context** trimming:

- Keep: identity/persona core (or shortened static prefix), **conversation_context**, minimal **tools** list (names + one-line descriptions only if needed), **safety_rules**, **autonomy** summary.
- Omit or null: **intelligence_context**, **superhuman_context**, **pattern_context**, **proactive_context**, **deep** memory_context (see retrieval gate), **outcome_context** unless tiny, **chain_of_thought** false, **reasoning_instruction** empty.
- Prefer `hu_prompt_build_static` + `hu_prompt_build_with_cache` when persona/tone do not force a full rebuild—already partially implemented for cached static prompts.

### System 1 tool loop

Align with existing policy: AI standards cite **max depth 10** for tool dispatch; System 1 should pass **`max_iterations = 1`** (or `HU_COGNITION_FAST_MAX_TOOL_ITERATIONS`) into the turn-local tool loop configuration. Slow path keeps current behavior.

### System 2+ enhancements (optional, behind flags)

- Reflection pass (`reflection.c`) only when slow/emotional and config enabled.
- Sub-agent / orchestrator hooks only on slow and when existing orchestrator triggers fire.

---

## Structs & API

New public header: `include/human/agent/cognition.h` (name bikeshed OK; must stay `hu_cognition_*`).

### Enums and constants

```c
typedef enum hu_cognition_mode {
    HU_COGNITION_MODE_FAST = 0,
    HU_COGNITION_MODE_SLOW,
    HU_COGNITION_MODE_EMOTIONAL,
} hu_cognition_mode_t;

/* Bit flags for fine-grained plans (optional) */
typedef enum hu_cognition_flag {
    HU_COGNITION_FLAG_NONE              = 0,
    HU_COGNITION_FLAG_SKIP_MEMORY_RAG   = 1u << 0,
    HU_COGNITION_FLAG_SKIP_PLANNER        = 1u << 1,
    HU_COGNITION_FLAG_LIMIT_TOOLS         = 1u << 2,
    HU_COGNITION_FLAG_SHORT_SKILLS        = 1u << 3,
    HU_COGNITION_FLAG_EMPATHY_EMPHASIS    = 1u << 4,
} hu_cognition_flag_t;

#define HU_COGNITION_CONFIDENCE_SLOW_DEFAULT     (0.55f)
#define HU_COGNITION_FAST_MAX_TOOL_ITERATIONS    (1u)
#define HU_COGNITION_TARGET_FAST_SYSTEM_TOKENS   (2048u) /* advisory for builders */
```

### Input / output bundles

```c
typedef struct hu_cognition_emotion_signal {
    int tag;           /* maps to existing emotion enum where available */
    float intensity;   /* 0..1 */
} hu_cognition_emotion_signal_t;

typedef struct hu_cognition_dispatch_input {
    const char *message;
    size_t message_len;
    /* Last few turns: parallel arrays, newest first or oldest first — pick one and document */
    const char *const *history_user_text;
    const size_t *history_user_len;
    size_t history_user_count;
    const hu_cognition_emotion_signal_t *emotions;
    size_t emotion_count;
    const char *channel;
    size_t channel_len;
    bool multistep_likely;
    bool high_injection_risk_cleared; /* must be true to run */
} hu_cognition_dispatch_input_t;

typedef struct hu_cognition_dispatch_result {
    hu_cognition_mode_t mode;
    float confidence;
    uint32_t flags;
    const char *backend_name; /* static string from classifier vtable */
} hu_cognition_dispatch_result_t;
```

### Classifier vtable

```c
typedef struct hu_cognition_classifier hu_cognition_classifier_t;

typedef struct hu_cognition_classifier_vtable {
    hu_error_t (*classify)(void *ctx, hu_allocator_t *alloc,
                           const hu_cognition_dispatch_input_t *in,
                           hu_cognition_dispatch_result_t *out);
    const char *(*name)(void *ctx);
    void (*deinit)(void *ctx);
} hu_cognition_classifier_vtable_t;

struct hu_cognition_classifier {
    void *ctx;
    const hu_cognition_classifier_vtable_t *vtable;
};

hu_error_t hu_cognition_classify(const hu_cognition_classifier_t *classifier,
                                 hu_allocator_t *alloc,
                                 const hu_cognition_dispatch_input_t *in,
                                 hu_cognition_dispatch_result_t *out);
```

**Built-ins**:

- `hu_cognition_classifier_heuristic_create(...)` — default, no extra allocations after create, or static singleton.
- Optional: `hu_cognition_classifier_embedding_create(...)` behind `#ifdef HU_ENABLE_COGNITION_EMBEDDING`.
- Optional: `hu_cognition_classifier_llm_create(...)` behind config + `!HU_IS_TEST` for real calls.

### Planner integration helper

```c
void hu_cognition_apply_to_prompt_config(hu_cognition_mode_t mode,
                                         uint32_t flags,
                                         hu_prompt_config_t *cfg /* in/out */);

uint32_t hu_cognition_max_tool_iterations(hu_cognition_mode_t mode, uint32_t configured_slow_max);
```

`hu_cognition_apply_to_prompt_config` **mutates** the struct in-place to zero out pointers / bools inappropriate for the mode (does not free strings; caller still owns buffers).

---

## Integration Points

### Primary: `hu_agent_turn()` (`src/agent/agent_turn.c`)

Recommended **single insertion point**: **after** lightweight per-turn perception that informs classification **and before** expensive retrieval:

- **After** user history append (`hu_agent_internal_append_history` succeeds).
- **After** `hu_superhuman_observe_all` and the **`hu_fast_capture` + STM / pattern radar** block (so emotion entities are available without duplicating work).
- **Before** the **Self-RAG** gate and `hu_memory_loader_load` (approximately where `hu_srag_should_retrieve` runs today).

Pseudocode sketch (illustrative):

```c
hu_cognition_dispatch_result_t cog = {0};
hu_cognition_dispatch_input_t cin = { /* msg, history slice, emotions from fc_result copy */ };
if (hu_cognition_dispatch(agent, &cin, &cog) != HU_OK || cog.confidence < threshold)
    cog.mode = HU_COGNITION_MODE_SLOW;

if (cog.mode == HU_COGNITION_MODE_FAST)
    /* set srag_skip_retrieval = true or bypass loader */;
/* else existing Self-RAG + memory loader */

/* When building hu_prompt_config_t */
hu_cognition_apply_to_prompt_config(cog.mode, cog.flags, &cfg);

/* When entering provider/tool loop */
uint32_t tool_cap = hu_cognition_max_tool_iterations(cog.mode, agent->max_tool_iterations);
```

**Interaction with existing automatic planner** (the `hu_planner_generate` block guarded by `#ifndef HU_IS_TEST`): For `HU_COGNITION_MODE_FAST`, **skip** that entire block. For slow/emotional, keep current behavior.

**Interaction with Self-RAG**: Fast mode should **short-circuit** to `HU_SRAG_NO_RETRIEVAL` equivalent behavior (or skip loader entirely) unless `HU_COGNITION_FLAG_SKIP_MEMORY_RAG` is cleared by explicit user/config override.

### Secondary: `hu_agent_turn_stream()` (`src/agent/agent_stream.c`)

Either:

- **Mirror** the same dispatch and gating (preferred for product consistency), or
- Document a deliberate **streaming = always slow** policy (not recommended).

### Observability

Extend `hu_observer_event_tag_t` with something like:

- `HU_OBSERVER_EVENT_COGNITION_DISPATCH` — payload: `mode`, `confidence`, `backend_name` (short static string).

Record once per turn after classification. Avoid logging raw user text.

### Metrics

Add counters to `hu_bth_metrics_t` (names illustrative):

- `cognition_fast_turns`, `cognition_slow_turns`, `cognition_emotional_turns`
- `cognition_low_confidence_fallbacks`

Increment in `hu_agent_turn` after dispatch. Keep struct versioning / init in `hu_bth_metrics_init`.

### `hu_agent_t` ownership

Store **optional** `hu_cognition_classifier_t classifier; bool classifier_owned;` or a **config pointer** to process-global default. Prefer **composition** over growing `hu_agent_t` without need—alternative is a single function that reads `agent->config` only.

---

## Configuration

Support **both** environment overrides (for dev) and **`config.json`** (for users).

### Environment variables (examples)

| Variable | Purpose |
|----------|---------|
| `HU_COGNITION_ENABLE` | `0` disables dispatcher (always slow) |
| `HU_COGNITION_BACKEND` | `heuristic` \| `embedding` \| `llm` |
| `HU_COGNITION_CONFIDENCE_THRESHOLD` | float string, default `0.55` |
| `HU_COGNITION_FORCE_MODE` | `fast` \| `slow` \| `emotional` (debug only) |

### `config.json` schema (proposal)

```json
{
  "agent": {
    "cognition": {
      "enabled": true,
      "backend": "heuristic",
      "confidence_threshold": 0.55,
      "fast": {
        "max_tool_iterations": 1,
        "target_system_tokens": 2048,
        "skip_memory_retrieval": true,
        "skills_catalog": "short"
      },
      "slow": {
        "max_tool_iterations": null
      },
      "emotional": {
        "min_affect_intensity": 0.6,
        "empathy_prompt_injection": true
      }
    }
  }
}
```

Parsing follows existing `config.c` patterns; **no new feature without a caller**—wire only the fields the first implementation reads.

---

## Testing

### Unit tests (`tests/test_cognition.c`)

- **Heuristic classifier**
  - Very short acknowledgments → `FAST` with high confidence.
  - Multistep / long reasoning cues → `SLOW`.
  - High-intensity negative affect → `EMOTIONAL`.
  - Ambiguous messages → low confidence → fallback to `SLOW`.
- **`hu_cognition_apply_to_prompt_config`**
  - Assert nulling/shortening of fields by mode (use a fake cfg with non-NULL pointers to stack strings; no heap).
- **`hu_cognition_max_tool_iterations`**
  - Fast returns `1`, slow returns configured max.

All classifier tests run **without** provider network: use `HU_IS_TEST` paths; embedding/LLM backends return `HU_ERR_NOT_SUPPORTED` or fixed stub modes when disabled.

### Integration tests

- **`HU_IS_TEST` agent fixture**: run `hu_agent_turn` with mock provider that records **system prompt length** and **number of tool rounds**; assert fast mode reduces both vs slow.
- **Regression**: ensure injection-high-risk still short-circuits before dispatch (or document if dispatch runs—prefer **not** to classify blocked input).

### Performance smoke (optional)

- Micro-benchmark heuristic dispatch-only on a corpus of strings; assert p99 is under 50 ms on CI hardware class (loose gate, informational first).

---

## Risks

| Risk | Mitigation |
|------|------------|
| **Wrong fast classification** → shallow answer on hard question | Low-confidence → slow; user-toggle per channel; “retry slow” tool or follow-up detection |
| **Skipped retrieval** → hallucination on factual asks | Self-RAG remains for slow; fast mode still allows **one** tool call for grounding; golden tests for factual prompts |
| **Emotional misclassification** | Bias emotional path toward **supportive** system text, not unsafe autonomy; keep policy/safety blocks identical |
| **Binary size / complexity** | Default backend is tiny heuristics; optional backends behind `#ifdef` |
| **Divergence** between stream and non-stream paths | Single shared `hu_cognition_dispatch_for_agent()` helper called from both |
| **Observer / metrics churn** | Additive enum values and BTH fields; document in `docs/error-codes.md` only if new `HU_ERR_*` introduced |

---

## References

- Kahneman, D. *Thinking, Fast and Slow* (metaphor only; not a cognitive science claim in code).
- Internal: `src/agent/agent_turn.c` — `hu_agent_turn()` pipeline ordering.
- Internal: `include/human/agent/prompt.h` — `hu_prompt_config_t`, `hu_prompt_build_system`.
- Internal: `include/human/observer.h` — `hu_observer_record_event`.
- Internal: `include/human/observability/bth_metrics.h` — behavioral telemetry struct.
- Internal: `.cursor/rules/ai-standards.mdc` — tool iteration / safety expectations for agent loops.
