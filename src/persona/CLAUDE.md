# src/persona/ — Persona System

The persona system defines and manages the agent's identity, communication style, and behavioral adaptation. Persona profiles are JSON files in `~/.human/personas/`.

## Architecture

```
persona.c            Loading, parsing, and validating persona JSON profiles
creator.c            Interactive persona creation workflow
analyzer.c           Analyzes communication samples to extract persona traits
sampler.c            Sampling and variation in persona expression
examples.c           Example conversation selection for few-shot prompting
feedback.c           Feedback loop for persona refinement
cli.c                CLI subcommands (list, show, create, analyze, validate)
```

## Adaptation & Context

```
mood.c               Mood tracking and emotional state modeling
circadian.c          Circadian rhythm — time-of-day behavioral shifts
relationship.c       Relationship stage tracking (new, familiar, close)
style_clone.c        Style cloning from communication samples
auto_profile.c       Auto-profile generation from conversation data
auto_tune.c          Auto-tuning persona parameters over time
training.c           Training data preparation for fine-tuning
life_sim.c           Life simulation context (daily routine, life chapter)
voice_maturity.c     Voice maturity tracking over time
replay.c             Conversation replay for persona testing
```

## Key Types

```c
hu_persona_t               — full persona profile (identity, traits, vocab, rules, values, overlays, examples, contacts, motivation, humor, voice, etc.)
hu_persona_overlay_t       — per-channel overrides (formality, length, emoji, typing quirks, message splitting)
hu_persona_example_bank_t  — example conversations grouped by channel
hu_persona_example_t       — single example (context, incoming, response)
hu_contact_profile_t       — per-contact relationship data
```

## Key Functions

- `hu_persona_load` — load persona from `~/.human/personas/<name>.json`
- `hu_persona_load_json` — parse persona from raw JSON
- `hu_persona_build_prompt` — compose system prompt from persona + channel overlay + topic
- `hu_persona_select_examples` — pick relevant few-shot examples for a channel + topic
- `hu_persona_find_overlay` — look up channel-specific style overrides
- `hu_persona_find_contact` — look up per-contact relationship data
- `hu_persona_feedback_record` / `hu_persona_feedback_apply` — feedback loop

## Prompt Assembly Flow

```
hu_persona_build_prompt
  → identity + traits + communication rules
  → channel overlay (formality, length, emoji for this channel)
  → selected examples (few-shot from example bank)
  → contact context (if known contact)
  → mood + circadian context
  → final system prompt string
```

## Rules

- Persona JSON parsing must handle missing/empty fields gracefully
- Overlay lookup is by channel name — must be case-insensitive
- Example selection should prefer topic-relevant examples, fallback to general
- Never hardcode persona data — always load from profile
- Use neutral test data in tests — no real personal information
- Free all allocations from `hu_persona_load` via `hu_persona_deinit`

## Contacts JSON shape (drives relational length / brevity)

The relational response-length and brevity-cap logic in `src/context/conversation.c`
reads `hu_contact_profile_t.prefers_short_texts`, `texts_in_bursts`, etc.
These fields are populated from JSON in **two accepted shapes**:

```jsonc
"contacts": {
  "+15555550123": {                            // key becomes contact_id
    "name": "Casey",
    "relationship_type": "friend",
    "warmth_level": "high",
    "relationship_stage": "close",            // unlocks deeper rapport behaviors
    // Canonical (preferred) — nested under communication_patterns:
    "communication_patterns": {
      "prefers_short_texts": true,
      "texts_in_bursts": true,
      "sends_links_often": false,
      "uses_emoji": true
    }
    // Forgiving fallback — same booleans at the top level when no nested
    // object exists. Useful for hand-edited persona files. Nested wins
    // when both are present.
  }
}
```

Lookup: `hu_persona_find_contact` matches the query string against
`contact_id` (the key) → `email` → `name` (case-insensitive). The runtime
passes the channel-specific `batch_key` (e.g. `+1…` for iMessage), so
contact keys must match the format the channel emits.
