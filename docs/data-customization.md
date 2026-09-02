---
title: Data Customization Guide
date: 2026-03-10
---

# Data Customization Guide

The runtime includes configurable data files that control prompt behavior, conversation patterns, memory analysis, security rules, and more. Users can customize any data file by placing an override in `~/.human/data/`.

## How Overrides Work

At startup, `hu_data_load()` checks for user customizations in this order:

1. **User override**: `~/.human/data/<relative-path>`
2. **Embedded default**: Compiled into the binary from `data/` directory

If a user-provided file exists, it takes precedence. Otherwise, the embedded default is used.

**File size limit**: 1 MB per file

## Data Directory Structure

```
data/
├── agent/              # Agent commitment patterns
├── channels/           # Channel-specific configs
├── conversation/       # Conversation analysis & tone
├── memory/             # Memory consolidation & emotion
├── persona/            # Persona and circadian phases
├── prompts/            # System prompts and hints
└── security/           # Command allowlists
```

## File Reference

### Prompts (`prompts/`)

System prompts that guide the agent's behavior.

#### `autonomy_full.txt`

Full autonomy mode — agent can act without explicit user approval.

#### `autonomy_supervised.txt`

Supervised mode — agent must request approval before acting.

#### `autonomy_readonly.txt`

Read-only mode — agent can analyze but not modify.

#### `default_identity.txt`

Core identity and role definition.

#### `safety_rules.txt`

Safety guardrails (data protection, command safety, prompt injection defense).

#### `group_chat_hint.txt`

Behavior guidance for group conversations. Controls response length and energy matching.

#### `persona_reinforcement.txt`

Instructions to maintain consistent persona during long conversations.

#### `reasoning_instruction.txt`

Guidance on how the agent should reason through complex problems.

#### `tone_hints.json`

Tone matching rules by communication style.

```json
{
  "casual": "The user communicates casually. Match their tone.",
  "technical": "The user is discussing technical details. Be precise and specific.",
  "formal": "The user communicates formally. Use clear, professional language."
}
```

### Conversation (`conversation/`)

Lexicons and patterns that shape conversational behavior.

#### `filler_words.json`

Casual particles to inject naturalness.

```json
{
  "fillers": [
    "haha ",
    "lol ",
    "yeah ",
    "honestly ",
    "tbh ",
    "ngl ",
    "hmm ",
    "oh ",
    "ah ",
    "like "
  ]
}
```

#### `starters.json`

Common conversation starters/bridges between thoughts.

```json
{
  "starters": [
    "oh ",
    "but ",
    "and ",
    "like ",
    "also ",
    "wait ",
    "haha",
    "lol",
    "omg",
    "ngl ",
    "tbh ",
    "btw ",
    "anyway ",
    "ok ",
    "so ",
    "yeah",
    "nah"
  ]
}
```

#### `ai_disclosure_patterns.json`

Patterns that indicate explicit AI self-disclosure to filter or flag.

```json
{
  "patterns": [
    "i'm an ai",
    "i am an ai",
    "as an ai",
    "i'm a language",
    "language model",
    "as a chatbot",
    "i'm a chatbot",
    "i am a chatbot",
    "i'm not human",
    "i am not human",
    "i don't have feelings",
    "i do not have feelings",
    "i can't feel",
    "as an assistant",
    "i'm an assistant",
    "i am an assistant",
    "i was programmed",
    "my training data",
    "i don't have personal",
    "i do not have personal",
    "i'm not a real person",
    "i am not a real person",
    "i'm just a bot",
    "i am just a bot"
  ]
}
```

#### `emotional_words.json`

Emotional vocabulary for sentiment tracking.

```json
{
  "emotional": [
    "miss",
    "love",
    "hurt",
    "stress",
    "depress",
    "lonely",
    "scared",
    "worried",
    "sorry",
    "afraid",
    "giving up",
    "feel like",
    "don't know",
    "can't",
    "help me",
    "need you",
    "cry",
    "sad"
  ],
  "positive": [
    "happy",
    "great",
    "awesome",
    "love",
    "good",
    "nice",
    "excited",
    "glad",
    "amazing",
    "wonderful",
    "lol",
    "haha",
    "yay",
    "sweet",
    "perfect",
    "thanks"
  ],
  "negative": [
    "sad",
    "angry",
    "frustrated",
    "annoyed",
    "terrible",
    "awful",
    "hate",
    "worried",
    "stressed",
    "upset",
    "disappointed",
    "ugh",
    "sucks",
    "rough",
    "hard",
    "sorry"
  ]
}
```

#### `crisis_keywords.json`

Keywords that trigger crisis response protocol.

```json
{
  "keywords": [
    "died",
    "passed away",
    "passed",
    "cancer",
    "divorce",
    "fired",
    "laid off",
    "diagnosis",
    "funeral",
    "breakup",
    "lost my job",
    "terminal",
    "hospital"
  ]
}
```

#### `contractions.json`

Natural language contraction patterns (is → 's, do not → don't, etc.).

```json
{
  "contractions": [
    { "from": "I am ", "to": "I'm " },
    { "from": "it is ", "to": "it's " },
    { "from": "do not ", "to": "don't " },
    { "from": "does not ", "to": "doesn't " }
  ]
}
```

#### `engagement_words.json`

High-engagement vocabulary to boost interaction quality.

#### `personal_sharing.json`

Patterns that indicate vulnerable personal sharing (triggers deeper empathy).

#### `conversation_intros.json`

Natural conversation opening phrases.

#### `backchannel_phrases.json`

Listening signals ("I see", "oh I get it", etc.).

#### `time_gap_phrases.json`

Phrases for acknowledging time between messages.

#### `vulnerability_keywords.json`

Keywords indicating emotional vulnerability.

### Agent (`agent/`)

Agent commitment and pattern tracking.

#### `commitment_patterns.json`

Linguistic patterns for detecting user commitments and goals.

```json
{
  "promise": ["I will ", "I'll ", "I promise "],
  "intention": ["I'm going to ", "I am going to ", "I plan to "],
  "reminder": ["remind me ", "don't let me forget ", "don't forget to "],
  "goal": ["I want to ", "my goal is ", "I hope to "],
  "negation": ["not ", "n't ", "never "]
}
```

### Memory (`memory/`)

Memory consolidation and emotion tracking.

#### `emotion_adjectives.json`

Adjective-to-emotion mapping for sentiment analysis.

```json
{
  "adjectives": [
    { "word": "great", "emotion": "joy" },
    { "word": "happy", "emotion": "joy" },
    { "word": "excited", "emotion": "excitement" },
    { "word": "sad", "emotion": "sadness" },
    { "word": "depressed", "emotion": "sadness" },
    { "word": "angry", "emotion": "anger" },
    { "word": "scared", "emotion": "fear" },
    { "word": "anxious", "emotion": "anxiety" }
  ]
}
```

#### `emotion_prefixes.json`

Sentence starters that precede emotional statements.

```json
{
  "prefixes": ["I feel ", "I'm feeling ", "I am ", "I'm "]
}
```

#### `relationship_words.json`

Relationship entity types for memory anchoring.

```json
{
  "words": [
    "mom",
    "dad",
    "wife",
    "husband",
    "friend",
    "sister",
    "brother",
    "boss",
    "coworker",
    "partner",
    "son",
    "daughter",
    "child",
    "family",
    "manager",
    "mommy",
    "daddy",
    "kid",
    "kids",
    "spouse",
    "colleague"
  ]
}
```

#### `topic_patterns.json`

Topic extraction patterns for memory indexing.

#### `commitment_prefixes.json`

Sentence starters for user commitments.

### Persona (`persona/`)

Persona adaptation and circadian guidance.

#### `relationship_stages.json`

Relationship stage definitions that control agent warmth and proactivity.

```json
{
  "stages": [
    {
      "name": "new",
      "guidance": "This is a newer relationship. Be helpful, clear, and professional. Build trust through reliability."
    },
    {
      "name": "familiar",
      "guidance": "You know this user moderately well. Reference past conversations when relevant. Be warmer."
    },
    {
      "name": "trusted",
      "guidance": "This is a trusted relationship. Be candid and proactive. Share observations and insights freely."
    },
    {
      "name": "deep",
      "guidance": "This is a deep, long-standing relationship. Be genuinely present. Anticipate needs. Celebrate growth."
    }
  ]
}
```

#### `circadian_phases.json`

Time-of-day persona adjustments.

```json
{
  "phases": [
    {
      "name": "early morning",
      "guidance": "Be gentle and warm. The user is starting their day. Keep responses calm and encouraging."
    },
    {
      "name": "morning",
      "guidance": "Be energetic and productive. The user is at peak mental clarity. Be direct and efficient."
    },
    {
      "name": "afternoon",
      "guidance": "Be steady and focused. Energy may be dipping. Keep things clear and structured."
    },
    {
      "name": "evening",
      "guidance": "Be relaxed and reflective. The day is winding down. Allow for deeper conversation."
    },
    {
      "name": "night",
      "guidance": "Be calm and intimate. The user is in a quieter headspace. Slow your pace, be thoughtful."
    },
    {
      "name": "late night",
      "guidance": "Be present and unhurried. Late night conversations often carry more weight. Be a quiet companion."
    }
  ]
}
```

### Security (`security/`)

Security and command allowlists.

#### `command_lists.json`

High-risk command blacklist and default allowlist.

```json
{
  "high_risk": [
    "rm",
    "mkfs",
    "dd",
    "shutdown",
    "reboot",
    "halt",
    "poweroff",
    "sudo",
    "su",
    "chown",
    "chmod",
    "useradd",
    "userdel",
    "usermod",
    "passwd",
    "mount",
    "umount",
    "iptables",
    "ufw",
    "firewall-cmd",
    "curl",
    "wget",
    "nc",
    "ncat",
    "netcat",
    "scp",
    "ssh",
    "ftp",
    "telnet"
  ],
  "default_allowed": [
    "git",
    "npm",
    "cargo",
    "ls",
    "cat",
    "grep",
    "find",
    "echo",
    "pwd",
    "wc",
    "head",
    "tail"
  ]
}
```

### Channels (`channels/`)

Channel-specific configurations.

#### `telegram_commands.txt`

Telegram bot commands and their descriptions.

```
/start - Start a conversation
/help - Show available commands
/status - Show model and stats
/model - Switch model
/think - Set thinking level
/verbose - Set verbose level
/tts - Set TTS mode
/memory - Memory tools and diagnostics
/stop - Stop active background task
/restart - Restart current session
/compact - Compact context now
```

## Behavioral Thresholds

The `behavior` config section in the runtime configuration controls seven key thresholds. These can be customized by creating a config file at `~/.human/config.json` (separate from data files).

| Threshold                     | Key                        | Default | Range    | Purpose                                                       |
| ----------------------------- | -------------------------- | ------- | -------- | ------------------------------------------------------------- |
| Group chat consecutive limit  | `consecutive_limit`        | 3       | 1–100    | Max consecutive messages in group chat before stepping back   |
| Group participation threshold | `participation_pct`        | 40      | 1–100    | Minimum percentage of total messages before agent joins group |
| Max response length           | `max_response_chars`       | 300     | 1–100000 | Character limit for responses                                 |
| Min response length           | `min_response_chars`       | 15      | 1–1000   | Minimum characters to send (avoid single-word responses)      |
| Memory decay window           | `decay_days`               | 30      | 1–365    | Days before memories fade in consolidation                    |
| Deduplication threshold       | `dedup_threshold`          | 70      | 1–100    | Similarity percentage (0–100) to consider memories duplicates |
| Missed message threshold      | `missed_msg_threshold_sec` | 1800    | 60–86400 | Seconds gap to trigger "missed message" acknowledgment        |
| Missed message ceiling        | `missed_msg_max_age_sec`   | 86400   | >threshold–604800 | Above this gap no acknowledgment is sent at all (a "just saw this" on a days-old message is a tell) |

### iMessage replay guards

Three guards keep a stale poll cursor from replaying old inbound messages as
if they were new (incident 2026-09-01: a reboot resumed from a two-week-old
cursor and re-answered threads the human had already closed). They are
environment overrides in the daemon's launchd plist, not `config.json` keys:

| Guard | Env var | Default | Behavior |
| ----- | ------- | ------- | -------- |
| Resume cap | `HU_IMESSAGE_MAX_REPLAY` | 50 rows | If the persisted cursor is more than this many rows behind chat.db, the backlog is skipped and the daemon resumes from the current max (logged as a warning). |
| Stale inbound | `HU_IMESSAGE_MAX_INBOUND_AGE_SEC` | 86400 | Inbound rows older than this are dropped at poll time (`0` disables). |
| Already answered | (always on) | — | An inbound row with a later human-authored text bubble in the same conversation is skipped; the daemon's own sends do not count. |

## How to Customize

### 1. Create Override Directory

```bash
mkdir -p ~/.human/data
```

### 2. Copy and Edit Data File

```bash
# Example: customize filler words
cp data/conversation/filler_words.json ~/.human/data/conversation/filler_words.json
# Edit the file
nano ~/.human/data/conversation/filler_words.json
```

### 3. Restart Daemon

Changes are loaded at startup:

```bash
hu_daemon restart
```

## Examples

### Example 1: Make Responses Shorter

**File**: `~/.human/data/behavior.json`

```json
{
  "behavior": {
    "max_response_chars": 150,
    "min_response_chars": 10
  }
}
```

Restart to apply.

### Example 2: Customize AI Disclosure Detection

**File**: `~/.human/data/conversation/ai_disclosure_patterns.json`

Add or remove patterns the agent should flag:

```json
{
  "patterns": [
    "i'm an ai",
    "i am an ai",
    "as an ai",
    "my custom pattern here",
    "another phrase to filter"
  ]
}
```

### Example 3: Add New Filler Words

**File**: `~/.human/data/conversation/filler_words.json`

```json
{
  "fillers": [
    "haha ",
    "lol ",
    "yeah ",
    "honestly ",
    "tbh ",
    "ngl ",
    "hmm ",
    "oh ",
    "ah ",
    "like ",
    "yo ",
    "dude "
  ]
}
```

### Example 4: Tune Group Chat Behavior

**File**: `~/.human/data/behavior.json`

```json
{
  "behavior": {
    "consecutive_limit": 2,
    "participation_pct": 50
  }
}
```

- Agent steps back after 2 consecutive messages
- Only joins groups where it has >50% participation rate

### Example 5: Adjust Memory Consolidation

**File**: `~/.human/data/behavior.json`

```json
{
  "behavior": {
    "decay_days": 60,
    "dedup_threshold": 80
  }
}
```

- Memories fade after 60 days (instead of 30)
- Consolidation considers memories duplicates at 80% similarity (more aggressive dedup)

### Example 6: Customize Circadian Phases

**File**: `~/.human/data/persona/circadian_phases.json`

Modify the guidance for specific times:

```json
{
  "phases": [
    {
      "name": "early morning",
      "guidance": "The user is just waking up. Be extremely gentle, supportive, and don't ask questions. Just listen."
    }
  ]
}
```

## Validation

Data files are validated at parse time. Common errors:

- **Invalid JSON**: Check syntax with `jq` or a JSON validator
- **File too large**: Max 1 MB per file
- **Missing required fields**: Some files have required keys (check examples above)

Validation errors log to stderr and fall back to the embedded default.

## Debugging

Check if your override is being loaded:

```bash
# Run in debug mode (requires debug build)
HU_DEBUG_DATA=1 hu_daemon start

# Look for logs indicating which path was loaded
```

To verify file ownership and permissions:

```bash
ls -la ~/.human/data/conversation/filler_words.json
```

Ensure the file is readable by the user running the daemon.

## Files Guaranteed to Override

All files in `data/` can be overridden. The system never hardcodes behavior — every tunable aspect is in a data file.

**Do not edit these files in the binary** — always place customizations in `~/.human/data/`.
