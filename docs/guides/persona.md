---
title: Persona Guide
description: Create and use persona profiles for personality cloning in seaclaw
updated: 2026-03-06
---

# Persona Guide

Personas let seaclaw adopt your real communication style by analyzing your message history and synthesizing a profile. The agent then uses this profile to match your voice, vocabulary, and tone across channels.

## Overview

A persona is a data-driven personality profile stored as JSON. It includes:

- **Core identity** — who you are, traits, vocabulary preferences, communication rules, values, decision style
- **Channel overlays** — per-channel adjustments (formality, message length, emoji usage, style notes)
- **Example banks** — curated message examples per channel for few-shot style matching

The persona system samples messages from iMessage, Gmail, or Facebook exports, sends batches to an AI provider for extraction, and writes a synthesized profile. You can also create and edit personas manually.

## Quick Start

Create your first persona in three commands:

```bash
# Create from iMessage (macOS)
seaclaw persona create myname --from-imessage

# Show the generated profile
seaclaw persona show myname

# Enable in config
# Add to ~/.seaclaw/config.json: { "agent": { "persona": "myname" } }
```

## Creating a Persona

### From iMessage

Requires macOS and access to `~/Library/Messages/chat.db`:

```bash
seaclaw persona create myname --from-imessage
```

The tool queries your sent messages (excluding group chats by default), sends them to the configured AI provider for analysis, and writes `~/.seaclaw/personas/myname.json`.

### From Facebook

Export your Facebook data (Settings > Your Facebook Information > Download Your Information), then:

```bash
seaclaw persona create myname --from-facebook /path/to/messages/inbox/your_thread/message_1.json
```

You can also pass a directory; the tool will find and parse message JSON files.

### From Gmail

Export Gmail via Google Takeout (MBOX or JSON). Then:

```bash
seaclaw persona create myname --from-gmail /path/to/Takeout/Mail/your_label.mbox
```

Or with a JSON export:

```bash
seaclaw persona create myname --from-gmail /path/to/messages.json
```

### Manual Creation

Create the file directly:

```bash
mkdir -p ~/.seaclaw/personas
# Edit ~/.seaclaw/personas/myname.json
```

See [Persona JSON format](#persona-json-format) for the schema.

## Persona JSON Format

All persona files live in `~/.seaclaw/personas/<name>.json`. Required structure:

```json
{
  "version": 1,
  "name": "myname",
  "core": {
    "identity": "Short description of who you are",
    "traits": ["direct", "curious", "casual"],
    "vocabulary": {
      "preferred": ["solid", "cool", "works"],
      "avoided": ["synergy", "leverage"],
      "slang": ["ngl", "imo", "tbh"]
    },
    "communication_rules": ["Keep it short", "No exclamation overload"],
    "values": ["honesty", "speed"],
    "decision_style": "Decides fast, iterates"
  },
  "channel_overlays": {
    "imessage": {
      "formality": "casual",
      "avg_length": "short",
      "emoji_usage": "minimal",
      "style_notes": ["drops punctuation", "no caps"]
    },
    "telegram": {
      "formality": "casual",
      "avg_length": "medium",
      "emoji_usage": "moderate",
      "style_notes": []
    }
  }
}
```

### Fields

| Field                       | Type     | Required | Description                                              |
| --------------------------- | -------- | -------- | -------------------------------------------------------- |
| `version`                   | number   | yes      | Schema version (currently 1)                             |
| `name`                      | string   | yes      | Persona identifier (must match filename without `.json`) |
| `core`                      | object   | yes      | Core personality data                                    |
| `core.identity`             | string   | yes      | One-line identity summary                                |
| `core.traits`               | string[] | no       | Personality traits (e.g. direct, curious)                |
| `core.vocabulary.preferred` | string[] | no       | Words/phrases to favor                                   |
| `core.vocabulary.avoided`   | string[] | no       | Words/phrases to avoid                                   |
| `core.vocabulary.slang`     | string[] | no       | Slang or informal terms                                  |
| `core.communication_rules`  | string[] | no       | Rules for how to communicate                             |
| `core.values`               | string[] | no       | Values that guide behavior                               |
| `core.decision_style`       | string   | no       | How decisions are made                                   |
| `channel_overlays`          | object   | no       | Per-channel overrides                                    |

### Channel Overlay Fields

Each key in `channel_overlays` is a channel name (e.g. `imessage`, `telegram`, `cli`, `discord`). Values:

| Field         | Type     | Description                                |
| ------------- | -------- | ------------------------------------------ |
| `formality`   | string   | e.g. casual, formal, mixed                 |
| `avg_length`  | string   | e.g. short, medium, long                   |
| `emoji_usage` | string   | e.g. none, minimal, moderate, heavy        |
| `style_notes` | string[] | Free-form notes (e.g. "drops punctuation") |

### Example: seth Persona

A minimal but complete persona (like the `seth` example):

```json
{
  "version": 1,
  "name": "seth",
  "core": {
    "identity": "Developer, direct and pragmatic",
    "traits": ["direct", "curious", "terse"],
    "vocabulary": {
      "preferred": ["solid", "works", "cool"],
      "avoided": ["synergy", "leverage"],
      "slang": ["ngl", "imo"]
    },
    "communication_rules": ["Keep it short", "Be precise"],
    "values": ["honesty", "speed"],
    "decision_style": "Decides fast, iterates"
  },
  "channel_overlays": {
    "imessage": {
      "formality": "casual",
      "avg_length": "short",
      "emoji_usage": "minimal",
      "style_notes": ["no caps"]
    }
  }
}
```

## Using a Persona

### Config

Set the default persona in `~/.seaclaw/config.json`:

```json
{
  "agent": {
    "persona": "myname"
  }
}
```

The agent will load this persona and inject its prompt into the system message.

### Per-Channel Persona Config

Use different personas per channel. Set `agent.persona` as the default, then override specific channels with `agent.persona_channels`:

```json
{
  "agent": {
    "persona": "seth",
    "persona_channels": {
      "imessage": "seth_casual",
      "gmail": "seth_professional",
      "slack": "seth_work"
    }
  }
}
```

When the agent responds on `imessage`, it loads `seth_casual`; on `gmail`, `seth_professional`. Channels not listed use the default `seth`.

### Channel Overlays (within a persona)

Channel overlays are applied automatically when the agent responds on a given channel. No extra config is needed; the persona file defines overlays per channel (formality, length, emoji usage).

### CLI Commands

```bash
# List all personas
seaclaw persona list

# Show persona profile (rendered system prompt)
seaclaw persona show myname

# Validate persona JSON
seaclaw persona validate myname

# Delete a persona
seaclaw persona delete myname

# Export persona JSON to stdout
seaclaw persona export myname

# Merge multiple personas into one
seaclaw persona merge output_name persona1 persona2 [persona3 ...]

# Import persona from file or stdin
seaclaw persona import myname --from-file /path/to/persona.json
seaclaw persona import myname --from-stdin   # read JSON from stdin

# Diff two personas (identity, traits, overlays)
seaclaw persona diff persona1 persona2

# Apply recorded feedback to persona
seaclaw persona feedback apply myname
```

## Example Banks

Example banks provide few-shot style examples per channel. The prompt builder selects relevant examples by topic and injects them into the system prompt.

### File Location

```
~/.seaclaw/personas/examples/<persona_name>/<channel>/examples.json
```

Example: `~/.seaclaw/personas/seth/examples/imessage/examples.json`

### Example JSON Format

```json
{
  "examples": [
    {
      "context": "casual greeting",
      "incoming": "hey whats up",
      "response": "not much, you?"
    },
    {
      "context": "making plans",
      "incoming": "dinner thursday?",
      "response": "down, 7pm"
    }
  ]
}
```

| Field      | Type   | Description                         |
| ---------- | ------ | ----------------------------------- |
| `context`  | string | Optional context/topic for matching |
| `incoming` | string | What the user said                  |
| `response` | string | How the persona would reply         |

Add examples manually or via feedback (see below).

## Feedback and Improvement

### Recording Corrections

When the agent responds in a way that does not match your style, record a correction via the persona tool in-conversation:

```json
{
  "action": "feedback",
  "name": "myname",
  "original": "Hey there!",
  "corrected": "Hey what's up",
  "context": "greeting"
}
```

Feedback is stored in `~/.seaclaw/personas/feedback/<name>.jsonl`.

### Applying Feedback

Periodically apply recorded feedback to update the persona and example banks:

```bash
seaclaw persona feedback apply myname
```

This reads the feedback file, merges corrections into example banks, and rewrites `examples.json` for affected channels.

## Channel Overlays

When the agent responds on a channel (e.g. `imessage`, `telegram`, `cli`), the prompt builder:

1. Uses the core persona (identity, traits, vocabulary, rules, values, decision_style)
2. Looks up `channel_overlays[channel]`
3. Appends overlay instructions (formality, avg_length, emoji_usage, style_notes)
4. Optionally injects example bank entries for that channel

Overlays let the same persona adapt: casual on iMessage, more formal on Slack, terse on CLI.

## Environment Override

For tests or custom layouts, set `SC_PERSONA_DIR` to override the persona base directory (default `~/.seaclaw/personas`):

```bash
export SC_PERSONA_DIR=/tmp/my-personas
seaclaw persona list   # lists /tmp/my-personas/*.json
```

## CLI Reference

| Command                                                                                                        | Description                                       |
| -------------------------------------------------------------------------------------------------------------- | ------------------------------------------------- |
| `seaclaw persona list`                                                                                         | List all persona profiles                         |
| `seaclaw persona show <name>`                                                                                  | Display persona (rendered prompt)                 |
| `seaclaw persona validate <name>`                                                                              | Validate persona JSON                             |
| `seaclaw persona create <name> [--from-imessage \| --from-gmail \| --from-facebook \| --from-response <path>]` | Create persona from message history               |
| `seaclaw persona update <name> [--from-imessage \| ...]`                                                       | Update existing persona from sources              |
| `seaclaw persona delete <name>`                                                                                | Remove persona file                               |
| `seaclaw persona export <name>`                                                                                | Export persona JSON to stdout                     |
| `seaclaw persona merge <output> <name1> <name2> [...]`                                                         | Merge multiple personas into one                  |
| `seaclaw persona import <name> [--from-file <path> \| --from-stdin]`                                           | Import persona from file or stdin                 |
| `seaclaw persona diff <name1> <name2>`                                                                         | Compare two personas (identity, traits, overlays) |
| `seaclaw persona feedback apply <name>`                                                                        | Apply recorded feedback to persona                |

## Persona for Agents

When you spawn subagents (e.g. via the delegate tool or agent teams), the spawn config can include a persona:

- `sc_spawn_config_t.persona_name` — name of the persona to load
- The spawned agent loads the persona from `~/.seaclaw/personas/<name>.json` and injects it into its system prompt

Config-driven agents use `agent.persona` from `config.json`. Spawned agents use `sc_spawn_config_t.persona_name` when provided.
