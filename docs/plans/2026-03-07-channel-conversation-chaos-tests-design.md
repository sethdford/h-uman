# Channel Conversation & Chaos Tests Design

**Date:** 2026-03-07
**Status:** Approved
**Scope:** Dynamic synthetic conversations, pressure testing, and chaos testing across all 33 human channels

## Motivation

The existing synthetic test harness validates CLI, Gateway HTTP, WebSocket, Agent, and Pressure paths. It does not exercise channel implementations — the poll→agent→send pipeline that powers every messaging integration. With 33 channels, we need automated proof that conversations flow correctly through each one, survive chaos conditions, and perform under load.

## Architecture

Two layers: **loopback simulation** (all channels, in-process) and **real iMessage** (macOS, opt-in). A chaos layer composes with both.

### Layer 1: Mock Inject Infrastructure

Add `hu_<channel>_test_inject_mock` and `hu_<channel>_test_get_last_message` APIs to all channels missing them. Four channels already have this pattern: MQTT, IMAP, Nostr, Email.

**Pattern (per channel, ~30 LOC each):**

```c
#if HU_IS_TEST
typedef struct hu_<name>_mock_msg {
    char session_key[128];
    char content[4096];
} hu_<name>_mock_msg_t;

// In context struct:
hu_<name>_mock_msg_t mock_msgs[8];
size_t mock_count;
char last_message[4096];
size_t last_message_len;
#endif
```

- `test_inject_mock`: appends to ring buffer
- Poll function: under `HU_IS_TEST`, returns mocks instead of doing real I/O
- `test_get_last_message`: returns last message passed to `send()`

**Channels to add (all tiers):**

| Tier          | Channels                                                                                                                   |
| ------------- | -------------------------------------------------------------------------------------------------------------------------- |
| 1 (Core)      | iMessage, Telegram, Discord, Slack, Signal                                                                                 |
| 2 (Extended)  | WhatsApp, Teams, Matrix, IRC, Line                                                                                         |
| 3 (Remaining) | Facebook, Instagram, Twitter, Google Chat, Google RCS, Lark, DingTalk, Mattermost, OneBot, QQ, Twilio, Web, Voice, MaixCam |

### Layer 2: Conversation Engine

Drives multi-turn synthetic dialogues through the full channel pipeline.

**Flow per conversation:**

```
Gemini generates scenario
  → inject user message via test_inject_mock
  → channel poll picks it up
  → agent dispatches to Gemini provider
  → response sent via channel vtable send
  → capture output via test_get_last_message
  → regex-match against expected pattern
  → inject next turn
  → repeat
```

**Scenario structure (Gemini-generated):**

```json
[
  {
    "channel": "imessage",
    "session_key": "+15551234567",
    "turns": [
      {
        "user": "What's the weather?",
        "expect_contains": "weather|temperature|forecast"
      },
      {
        "user": "Thanks, what about tomorrow?",
        "expect_contains": "tomorrow|forecast"
      }
    ]
  }
]
```

**Execution:**

1. Create channel via `hu_<name>_create` with test config
2. For each turn: inject → poll → dispatch → send → capture → verify
3. Record latency, verdict, save failures to regression dir

### Layer 3: Chaos Testing

Two chaos dimensions, composable with conversations. Chaos types are hardcoded (deterministic). Gemini generates the conversations that chaos is applied to.

**Message Chaos:**

| Type          | Description                                            |
| ------------- | ------------------------------------------------------ |
| Unicode bomb  | Zalgo text, RTL overrides, emoji sequences, null bytes |
| Size extremes | Empty, 1-char, 4095 bytes (boundary), 100KB overflow   |
| Rapid fire    | 50 messages in <100ms on same session_key              |
| Interleave    | 5 conversations on 5 session_keys through one channel  |
| Malformed     | Unmatched quotes, broken JSON, SQL injection attempts  |
| Echo storm    | Messages matching channel's own sent messages          |

**Infrastructure Chaos:**

| Type                 | Description                                      |
| -------------------- | ------------------------------------------------ |
| Gateway kill/restart | SIGKILL mid-conversation, restart, verify resume |
| State corruption     | Zero context fields between poll cycles          |
| Memory pressure      | Failing allocator after N allocations            |
| Poll timeout         | Sleep in poll cycle to simulate delay            |
| Double start/stop    | Call start/stop twice, verify idempotent         |
| Concurrent send      | Fork and send from 2 processes simultaneously    |

**CLI flag:** `--chaos=message`, `--chaos=infra`, `--chaos=all`, `--chaos=none` (default).

### Layer 4: Real iMessage (macOS, opt-in)

Activated via `--real-imessage <phone_or_email>`:

- Verifies Messages.app is running
- Gemini generates conversation starter
- Sends via real AppleScript path
- Polls `~/Library/Messages/chat.db` for response
- 30-second timeout per turn
- Reports send success, poll success, round-trip latency
- Guarded behind `__APPLE__` and `HU_ENABLE_SQLITE`

### Layer 5: Pressure Testing

Fork-based concurrent conversation load:

- N workers, each driving a conversation on a different channel
- All channels use loopback/mock
- Gateway running with Gemini provider
- Metrics: conversations/sec, avg turn latency, error rate
- Duration-based with configurable concurrency

## File Structure

```
tests/synthetic/
  channel_harness.h          — types, config, declarations
  channel_main.c             — CLI parsing, orchestrator, report
  channel_conversation.c     — conversation engine
  channel_chaos.c            — chaos scenarios
  channel_pressure.c         — fork-based pressure
  channel_imessage_real.c    — real iMessage e2e (macOS)

src/channels/
  imessage.c                 — +mock inject, +get_last_message
  telegram.c                 — +mock inject, +get_last_message
  discord.c                  — +mock inject, +get_last_message
  slack.c                    — +mock inject, +get_last_message
  signal.c                   — +mock inject, +get_last_message
  whatsapp.c                 — +mock inject, +get_last_message
  teams.c                    — +mock inject, +get_last_message
  matrix.c                   — +mock inject, +get_last_message
  irc.c                      — +mock inject, +get_last_message
  line.c                     — +mock inject, +get_last_message
  (... remaining channels)

CMakeLists.txt               — new human_channel_tests target
```

## Build

```cmake
option(HU_ENABLE_CHANNEL_TESTS "Build channel conversation+chaos test harness" OFF)
```

```bash
cmake -B build -DHU_ENABLE_CHANNEL_TESTS=ON -DHU_ENABLE_SYNTHETIC=ON \
      -DHU_ENABLE_ALL_CHANNELS=ON -DHU_ENABLE_SQLITE=ON
cmake --build build -j$(nproc)
```

## Usage

```bash
# All channels, full chaos, pressure
./build/human_channel_tests \
  --binary ./build/human \
  --channels all \
  --count 5 \
  --chaos all \
  --concurrency 4 \
  --duration 10 \
  --regression-dir /tmp/hu_channel_regressions \
  --verbose

# Single channel
./build/human_channel_tests --channels imessage --count 3

# Real iMessage
./build/human_channel_tests --real-imessage +15551234567 --count 2

# Chaos only
./build/human_channel_tests --channels telegram,slack --chaos message --count 10
```

## Metrics Output

```
[channel] ========== Final Report ==========
[channel] iMessage     5/5 passed  (avg 1.2s/turn, p99 2.1s)
[channel] Telegram     5/5 passed  (avg 0.9s/turn, p99 1.5s)
[channel] Discord      4/5 passed, 1 FAILED  (avg 1.1s/turn)
[channel] Chaos-Msg    12/12 passed
[channel] Chaos-Infra  8/10 passed, 2 FAILED
[channel] Pressure     120 convos, 24/s, 0 errors
[channel] Real-iMsg    1/1 passed  (avg 8.3s/turn)
[channel] Total: 155/158 passed, 3 failed, 0 errors
```

## Security Considerations

- No real API keys, tokens, or phone numbers in test code
- Real iMessage requires explicit opt-in flag
- All mock data uses neutral placeholders
- Chaos tests never weaken security policy — they test resilience
- `HU_IS_TEST` guards prevent accidental network/process side effects
