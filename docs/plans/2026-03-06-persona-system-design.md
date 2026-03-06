# Persona System Design

**Date**: 2026-03-06
**Status**: Approved
**Scope**: New `src/persona/` subsystem, CLI command, tool, runtime integration

## Goal

Give seaclaw the ability to adopt a user's real personality — writing style, vocabulary, decision-making patterns, communication habits — by analyzing their actual message history from iMessage, Gmail, Facebook, etc. Each agent can have its own persona. Personas adapt per channel (casual on iMessage, professional on Gmail) while maintaining a unified core identity.

## Requirements

1. **Full personality clone**: writing style, decision-making, communication habits, vocabulary, tone
2. **Data-driven creation**: ingest real messages from iMessage, Facebook, Gmail to extract personality patterns
3. **CLI + tool interface**: CLI command for initial creation, `sc_tool_t` for in-conversation refinement
4. **Unified core + channel overlays**: base personality with per-channel style adjustments (formality, message length, emoji usage)
5. **Per-agent personas**: each agent can have its own persona; spawned agents inherit parent's persona unless overridden
6. **Example bank**: curated message pairs per channel for few-shot prompting
7. **Provider-assisted analysis**: messages sent to AI provider for personality extraction (HTTPS)

## Architecture: Approach C (Structured Profile + Example Bank)

A structured persona profile (`persona.json`) captures the core personality and per-channel overlays. A curated example bank stores representative message pairs per channel. At runtime, the profile goes into the system prompt and 2-3 relevant examples are injected as few-shot context.

This balances personality fidelity against seaclaw's constraints (430 KB binary, 5-6 MB RSS). No embeddings infrastructure or vector search required.

## Data Model

### Persona Profile (`~/.seaclaw/personas/<name>.json`)

```json
{
  "version": 1,
  "name": "seth",
  "core": {
    "identity": "Short bio/description of who this person is",
    "traits": ["direct", "technically curious", "dry humor"],
    "vocabulary": {
      "preferred": ["definitely", "solid", "let's go"],
      "avoided": ["synergy", "leverage", "circle back"],
      "slang": ["ngl", "lowkey"]
    },
    "communication_rules": [
      "Keeps messages short unless explaining something technical",
      "Uses lowercase in casual contexts",
      "Rarely uses exclamation marks"
    ],
    "values": ["efficiency", "honesty", "privacy"],
    "decision_style": "Decides quickly, iterates after"
  },
  "channel_overlays": {
    "imessage": {
      "formality": "casual",
      "avg_length": "short",
      "emoji_usage": "minimal",
      "style_notes": ["drops punctuation", "uses 'lol' sparingly"]
    },
    "gmail": {
      "formality": "professional",
      "avg_length": "medium",
      "emoji_usage": "none",
      "style_notes": ["signs off with 'Best,'", "uses bullet points"]
    }
  }
}
```

### File Layout

```
~/.seaclaw/personas/
  default.json              # core persona, used when no agent-specific persona is set
  seth-professional.json    # variant persona for specific agents
  examples/
    seth/
      imessage/
        examples.json       # 5-10 message pairs
      gmail/
        examples.json
```

### Example Bank Format (`examples.json`)

```json
{
  "examples": [
    {
      "context": "Friend asking to hang out",
      "incoming": "Hey want to grab dinner tonight?",
      "response": "down. where at"
    },
    {
      "context": "Coordinating a meeting time",
      "incoming": "When works for you this week?",
      "response": "thursday afternoon is open, or friday morning"
    }
  ]
}
```

### Config Integration

```json
{
  "agent": {
    "persona": "seth",
    "profiles": {
      "email-agent": { "persona": "seth-professional" },
      "chat-agent": { "persona": "seth" }
    }
  }
}
```

## Persona Creator

### CLI Interface

```bash
seaclaw persona create seth --from-imessage
seaclaw persona create seth --from-imessage --from-gmail --from-facebook
seaclaw persona create seth --from-imessage --interactive
seaclaw persona update seth --from-gmail
```

### Creation Pipeline

```
1. INGEST    → Read messages from source (iMessage DB, Gmail API, Facebook export)
2. SAMPLE    → Select diverse message samples (recent, varied contacts, different contexts)
3. ANALYZE   → Send batches to AI provider with extraction prompt
4. SYNTHESIZE → Merge per-batch analyses into unified persona profile
5. OVERLAY   → Generate channel-specific overlays from per-source patterns
6. CURATE    → Select 5-10 representative message pairs per channel for example bank
7. INTERVIEW → (if --interactive) Ask user to confirm/correct extracted traits
8. WRITE     → Save persona.json + example bank to ~/.seaclaw/personas/
```

### Provider Analysis Prompt (Step 3)

The analyzer sends batches of messages to the AI provider with a structured extraction prompt requesting: personality traits, vocabulary patterns, sentence structure preferences, formality level, emoji/slang usage, sign-off habits, humor style, response length patterns, and decision-making patterns.

### Data Sources

| Source   | Ingestion Method                                                      |
| -------- | --------------------------------------------------------------------- |
| iMessage | Read from `~/Library/Messages/chat.db` (SQLite, macOS)                |
| Gmail    | Existing `src/channels/gmail.c` infrastructure, OAuth                 |
| Facebook | JSON export file (`Download Your Information` from Facebook settings) |

## New Files

| File                           | Purpose                                                                      |
| ------------------------------ | ---------------------------------------------------------------------------- |
| `src/persona/persona.c`        | Persona loading, merging, serialization                                      |
| `src/persona/creator.c`        | Creation pipeline orchestration                                              |
| `src/persona/analyzer.c`       | Message analysis via provider (batched)                                      |
| `src/persona/sampler.c`        | Message sampling strategies                                                  |
| `src/persona/examples.c`       | Example bank read/write/selection                                            |
| `include/seaclaw/persona.h`    | Public types: `sc_persona_t`, `sc_persona_overlay_t`, `sc_persona_example_t` |
| `src/tools/persona_tool.c`     | `sc_tool_t` for in-conversation persona management                           |
| `tests/test_persona.c`         | Tests for persona loading, building, overlay merging                         |
| `tests/test_persona_creator.c` | Tests for creation pipeline with mock data                                   |

## C Types

```c
typedef struct sc_persona_overlay {
    const char *channel;
    const char *formality;
    const char *avg_length;
    const char *emoji_usage;
    const char **style_notes;
    size_t style_notes_count;
} sc_persona_overlay_t;

typedef struct sc_persona_example {
    const char *context;
    const char *incoming;
    const char *response;
} sc_persona_example_t;

typedef struct sc_persona {
    const char *name;
    size_t name_len;
    const char *identity;
    const char **traits;
    size_t traits_count;
    const char **preferred_vocab;
    size_t preferred_vocab_count;
    const char **avoided_vocab;
    size_t avoided_vocab_count;
    const char **slang;
    size_t slang_count;
    const char **communication_rules;
    size_t communication_rules_count;
    const char **values;
    size_t values_count;
    const char *decision_style;
    sc_persona_overlay_t *overlays;
    size_t overlays_count;
    sc_persona_example_t **examples;   /* per-channel example arrays */
    size_t *examples_counts;
    size_t channels_count;
} sc_persona_t;
```

## Runtime Integration

### System Prompt Assembly

In `sc_prompt_build_system()`, persona replaces the default identity:

```
WITHOUT PERSONA:
  "You are SeaClaw, an AI assistant. Respond helpfully and concisely."

WITH PERSONA:
  "You are acting as Seth. Here is Seth's personality profile:
   [core traits, vocabulary, communication rules, values, decision style]
   When communicating, match Seth's style naturally. Don't exaggerate
   traits — aim for authenticity, not caricature."

WITH PERSONA + CHANNEL:
  [above, plus:]
  "You are currently communicating via iMessage. Seth's iMessage style:
   [overlay: casual, short messages, minimal emoji, drops punctuation]
   Here are examples of how Seth actually texts:
   [2-3 example pairs from the example bank]"
```

### Runtime Flow

```
sc_agent_turn()
  → sc_persona_load(agent->persona_name)        // load persona.json
  → sc_persona_build_prompt(persona, channel)    // build persona_prompt string
  → sc_persona_select_examples(persona, channel) // pick 2-3 examples
  → config.persona_prompt = built_prompt          // wire into prompt config
  → sc_prompt_build_system(&config)              // existing function handles the rest
```

### Agent Struct Changes

```c
typedef struct sc_agent {
    /* ... existing fields ... */
    const char *persona_name;
    size_t persona_name_len;
    sc_persona_t *persona;
} sc_agent_t;
```

### Spawn Inheritance

```c
typedef struct sc_spawn_config {
    /* ... existing fields ... */
    const char *persona_name;       /* NULL = inherit parent's persona */
    size_t persona_name_len;
} sc_spawn_config_t;
```

## Per-Channel Overlay System

Overlays are keyed by channel name (matching `sc_channel_t.name`). When the agent sends through a channel, the runtime checks `persona->overlays` for a match. If found, the overlay modifies the prompt. If not, the core persona is used as-is.

The creator auto-generates overlays from source data. Users can manually add overlays for channels without source data via the persona tool.

## Agent Persona Inheritance

```
Default persona ("seth")
    │
    ├── email-agent → persona: "seth-professional" (override)
    ├── chat-agent  → persona: "seth" (inherited)
    └── spawned sub-agent → inherits parent's persona
```

Agents inherit the default persona unless overridden in config or at spawn time.

## Example Selection

At runtime, `sc_persona_select_examples()` picks 2-3 examples from the matching channel's bank. Selection: random sampling biased toward examples whose `context` keywords overlap with the current conversation topic. Keyword matching, not semantic search — stays lightweight.

## Persona Tool (`persona_tool`)

Registered as `sc_tool_t` with actions:

| Action           | Description                     |
| ---------------- | ------------------------------- |
| `create`         | Create persona from source data |
| `update`         | Patch persona fields            |
| `update_overlay` | Modify a channel overlay        |
| `show`           | Display current persona         |
| `list`           | List available personas         |
| `delete`         | Remove a persona                |

## Risk Assessment

- **Medium risk**: New subsystem with config/file I/O, provider interaction, and prompt injection
- **Low risk**: No changes to security, gateway, or runtime boundaries
- **Privacy note**: Raw messages are sent to the AI provider for analysis (user-consented, HTTPS-only)
- **Size impact**: Estimated ~8-12 KB additional binary size for the persona subsystem (within budget)

## Testing Strategy

- Unit tests for persona loading/serialization (`test_persona.c`)
- Unit tests for prompt building with persona + overlay + examples
- Unit tests for creation pipeline with mock provider responses (`test_persona_creator.c`)
- Mock iMessage DB for ingestion tests (SQLite fixture)
- `SC_IS_TEST` guards for provider calls and file I/O in tests
