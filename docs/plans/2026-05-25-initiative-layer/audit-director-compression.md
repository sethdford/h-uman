# Audit — Does the Director Compress Rich Context Before the LLM Sees It?

**Date:** 2026-05-25
**Question asked:** "The conversation feels mechanical despite 25+ context fields injected — is the director compressing this into a one-sentence directive before the model sees it?"
**Answer:** **No, but recency dominance is a real concern.**

## What the director actually does

`hu_director_result_t` ([daemon.c:231–237](src/daemon.c:231)):
```c
typedef struct {
    hu_director_action_t action;     // TEXT | TAPBACK | SILENCE
    uint32_t delay_s;
    hu_reaction_type_t reaction;
    bool burst;
    char direction[512];             // free-text meta-instruction
} hu_director_result_t;
```

The director is called by a Flash-Lite Gemini classifier that parses the inbound and emits this structured verdict ([daemon.c:309–426](src/daemon.c:309)).

## Where the director's output flows

Two distinct outputs, two different sinks:

| Output | Sink | Effect on LLM |
|---|---|---|
| `action` (TEXT/TAPBACK/SILENCE) | Daemon control flow ([daemon.c:5081+]) | Decides whether to invoke the agent at all |
| `delay_s`, `burst`, `reaction` | Daemon timing/transport | Inserts realistic latency, splits into multiple sends, fires a tapback |
| `direction` (512 char meta-instruction) | **APPENDED to `conversation_context`** ([daemon.c:10395–10421]) with header `"--- Scene Direction (this message only) ---"` | Reaches the LLM as part of conversation_context |

The director **does not modify or replace** the 25+ rich context fields. It adds a small block. The system prompt builder (`hu_prompt_build_system` at [prompt.c:109](src/agent/prompt.c:109)) injects:

```
memory_context             personal_model_context     moment_context
self_exemplars_context     world_model_context        relational_episode_context
instruction_context        stm_context                contact_context
conversation_context  ← director.direction lives here
awareness_context          superhuman_context         intelligence_context
skills_context             emotional_context          humanness_context
somatic_context            narrative_self_context     presence_context
micro_expression_context   creative_voice_context     novelty_context
attachment_context         rupture_context            commitment_context
pattern_context            adaptive_persona_context   proactive_context
outcome_context
```

Each field is appended in order. The conversation_context (which now contains the director's directive) sits AFTER most other context.

## Where the real bottleneck likely is

Not in the director's compression. Three actual suspects, in priority order:

### Suspect 1: Recency dominance
The director's directive is appended to `conversation_context`, which appears late in the system prompt. LLMs (especially Gemini 3.x with its thinking layer) exhibit strong recency bias — the LAST instruction often dominates earlier context. A directive like "Casual, slightly playful, maybe mention the cat" arriving at byte 16000 of an 18KB prompt may eclipse the rich emotional/somatic/relational context delivered earlier.

**How to verify:** capture the actual system prompt for one turn (h-uman has prompt-cache infrastructure — see [prompt_cache.c]). Inspect what fraction of bytes is rich-context vs director-directive. If director is 0.05% of bytes but 90% of model attention, recency dominates.

### Suspect 2: Context-field populator emptiness
The 25+ field NAMES are sophisticated. The CONTENT depends on populators in agent_turn.c. Some populators may return short stubs or NULL most of the time:
- `somatic_context` — does this actually reflect body state, or is it always an empty placeholder?
- `rupture_context` — fires only when there's a real relationship rupture, otherwise NULL
- `narrative_self_context` — needs a working memory consolidation pipeline to be non-trivial

**How to verify:** instrument `hu_prompt_build_system` to log per-field byte counts on every call. Look for fields that are consistently zero-length. Those are wired-but-dead.

### Suspect 3: Model can't actually use 18KB of structured context for short outputs
Gemini 3.x's thinking phase (root cause of the iMessage empty-response bug today) demonstrates the model is computing expensively even for short replies. With 18KB of rich context and `maxOutputTokens=80`, the model may use thinking to PROCESS the context but then produce a generic "Hey what's up?" because that's the natural short reply. The richness doesn't translate into output uniqueness for terse exchanges.

**How to verify:** A/B test the same inbound with (a) full 18KB context and (b) stripped-to-essentials 1KB context. If the replies are identical, the rich context isn't shaping output for short messages — which means it only helps for LONG replies where there's room to vary.

## What this means for the initiative spec

The good news: rich context DOES flow to the LLM. The bad news: that doesn't guarantee it shapes output. For the **initiative-layer**, this matters because:

- A "propose a message" LLM call has more room to vary output than "reply to 'hi'" — initiative messages are LONGER and more deliberate. The 25 context fields should pay off more here than in reactive replies.
- The initiative-layer should use the **analytical or pro tier** (more tokens, more thinking budget) — not reflexive.
- If recency bias dominates, the initiative prompt should put the MOST important context (recent commitments, today's events, current emotional state) LAST.

## Recommended follow-up audits (out of scope here)

- **Audit A:** Instrument per-field byte counts in `hu_prompt_build_system` for one week of real traffic. Identify dead fields.
- **Audit B:** Capture actual system-prompt content for 10 representative turns. Read them. Are the fields generic ("user is in casual context") or specific ("Seth mentioned bonsai 3 messages ago and last spoke to Mindy 2 days ago about Marc's job")?
- **Audit C:** A/B test full vs stripped context on the SAME inbound to a fake provider that returns the prompt verbatim. Measure output divergence.

## Verdict on the original question

"Why does it feel mechanical?" — **NOT the director's fault.** Three more likely causes: recency dominance, dead populators, or model can't use 18KB for 80-token outputs. Worth investigating but not blocking the initiative-layer work.
