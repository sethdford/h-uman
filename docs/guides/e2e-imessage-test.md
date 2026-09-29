---
title: "E2E iMessage Agent-to-Agent Test"
description: "End-to-end test with two human instances conversing over real iMessage."
---

# E2E iMessage Agent-to-Agent Test

Two `human` instances talking to each other over real iMessage, proving the full pipeline works end-to-end: poll → classify → agent turn → send → repeat.

> **Note:** The `human channel e2e-test` subcommand is planned but not yet implemented in `cli_commands.c`. For now, use the manual two-machine E2E procedure described below, or the synthetic test harness at `tests/synthetic/channel_imessage_real.c`.

## Quick Humanness E2E (Single Machine)

For a fast single-machine proof of every iMessage capability (text, images, audio, video, tapbacks, typing indicators, read receipts, watch events):

```bash
./scripts/e2e-imessage-humanness.sh --to "+1234567890"          # all phases
./scripts/e2e-imessage-humanness.sh --to "+1234567890" --phase=tapback  # one phase
./scripts/e2e-imessage-humanness.sh --to "+1234567890" --verbose --pause=5  # slow + logs
```

Requires: macOS, Messages.app signed in, `imsg` CLI, Full Disk Access. Optional: `ffmpeg` (video), `CARTESIA_API_KEY` (voice messages).

## Agent-to-Agent E2E (Two Machines)

## Prerequisites

- **Two Macs** (or one Mac + one Mac VM), each with its own Apple ID
- **Full Disk Access** granted to the terminal app on both machines (for `chat.db` read access)
- `human` built and configured on both machines
- Both Apple IDs must have iMessage enabled
- Optional: `brew install steipete/tap/imsg` for faster sends via imsg CLI

## Quick Start

Until `human channel e2e-test` exists, run E2E manually: on **Mac A**, send an opening message to Mac B’s handle (Messages.app or any method), then start `human service-loop` on both machines with the config below. **Mac B** runs the service loop with E2E-friendly daemon settings; **Mac A** does the same so both sides poll and reply. Configure `e2e_max_turns`, `max_consecutive_replies`, and `response_mode` on both sides as in the Config section.

### Mac A (initiator) — when `e2e-test` is implemented

```bash
# Planned: one command sends seed, starts listening, auto-stops after 5 turns (not in cli_commands.c yet)
human channel e2e-test --target +1BBBBBBBBB --turns 5
```

### Mac B (responder)

```bash
# Run the service loop with E2E-friendly config (see Config section below)
human service-loop
```

With the manual procedure, Mac A’s first outbound message kicks off the loop; Mac B picks it up and responds, Mac A picks that up and responds, and so on — back and forth over real iMessage until you stop the daemons or hit `e2e_max_turns` on the responder.

## CLI Reference (planned)

The following documents the intended `human channel e2e-test` interface once it is implemented:

```
human channel e2e-test [options]

Options:
  --target <phone/email>   iMessage handle of the other agent (defaults to
                           channels.imessage.default_target from config)
  --seed <message>         Opening message to kick off the conversation
                           (default: "Hey! Just testing our connection —
                           how's everything going?")
  --turns <N>              Stop after N agent responses (default: 5)
```

When implemented, the command would automatically override daemon settings for E2E mode:
- `max_consecutive_replies` → 0 (unlimited)
- `e2e_max_turns` → N (from `--turns`)
- `response_mode` → "eager" (respond to everything)

## Config

### Mac A (initiator)

Mac A would use `human channel e2e-test` for automatic config overrides once that subcommand exists. For the manual procedure, apply the same daemon overrides as Mac B (or only on the side where you want turn limiting). Minimal config needed:

```json
{
  "channels": {
    "imessage": {
      "default_target": "+1BBBBBBBBB",
      "allow_from": ["+1BBBBBBBBB"],
      "use_imsg_cli": true
    }
  }
}
```

`use_imsg_cli` is the recommended production config. When `true` and `imsg` is on `$PATH`, enables: faster text send (<1s via `imsg send`), attachment send via `imsg send --file`, tapback via `imsg react`, event-driven polling via `imsg watch` subprocess, and target validation via `imsg chats` at startup. All paths have graceful AppleScript/JXA fallback.

### Mac B (responder)

Mac B runs `human service-loop`, so it needs explicit daemon overrides in `~/.human/config.json`:

```json
{
  "channels": {
    "imessage": {
      "default_target": "+1AAAAAAAAA",
      "allow_from": ["+1AAAAAAAAA"],
      "use_imsg_cli": true,
      "daemon": {
        "max_consecutive_replies": 0,
        "e2e_max_turns": 5,
        "response_mode": "eager"
      }
    }
  }
}
```

### Config Fields

| Field | Where | Default | Description |
|-------|-------|---------|-------------|
| `daemon.max_consecutive_replies` | Any channel's daemon block | 3 | Max agent replies before going silent. `0` = unlimited. |
| `daemon.e2e_max_turns` | Any channel's daemon block | 0 | Auto-stop daemon after N total agent responses. `0` = no limit. |
| `daemon.response_mode` | Any channel's daemon block | `"selective"` | `"eager"` responds to everything, recommended for E2E. |

These fields work on **any channel**, not just iMessage — you could run the same test over Telegram, Discord, etc.

### Humanness Config (Production)

For natural, human-like behavior in real conversations (not E2E testing), use `response_mode: "normal"` and enable `llm_decides`. This activates the full intelligence pipeline: the daemon uses a fast classify LLM to decide text, tapback, or silence for each message — with natural delay.

```json
{
  "channels": {
    "imessage": {
      "default_target": "+1XXXXXXXXXX",
      "use_imsg_cli": true,
      "daemon": {
        "llm_decides": true,
        "voice_enabled": true,
        "response_mode": "normal"
      }
    }
  }
}
```

| Feature | What happens | Requires |
|---------|-------------|----------|
| **`llm_decides: true`** | Fast Flash model classifies each message: text, tapback, or silence + natural delay. Overrides the *heuristic* gates (response_mode, drop-off, tapback-skip, leave-on-read, constitutional critique). The *safety* gates always run regardless: consecutive-reply limiter, AI-tell retry, quality retry, and the reply budget (`behavior.reply_budget_*`). See `include/human/daemon/reactive_gates.h`. | Configured provider (Gemini Flash recommended) |
| **`use_imsg_cli: true`** | Text via `imsg send`, attachments via `imsg send --file`, tapbacks via `imsg react`, event-driven polling via `imsg watch`, target validation via `imsg chats`. | `imsg` on `$PATH` |
| **`voice_enabled: true`** | Agent occasionally sends voice notes via Cartesia TTS when context warrants it. | `CARTESIA_API_KEY`, persona with `voice.voice_id` |
| **`response_mode: "normal"`** | Heuristic classifier output used as-is (selective downgrades without `?`, eager always responds). `llm_decides` overrides this per-message. | — |

With this config, the full humanness pipeline activates:

- **Emotional pacing** — heavier messages get longer read delays (+3s heavy, +6s grief)
- **Typing indicators** — "..." bubble appears before responses, between choreography segments, and briefly before backchannels
- **Tapback intelligence** — director chooses heart/like/haha contextually (not randomly)
- **Leave on read** — occasionally skips response entirely for low-signal messages
- **Backchannel** — sends "mhm" or "haha" instead of a full LLM response when appropriate
- **Silence intuition** — detects when no response is the most human response
- **Choreography** — splits long responses into natural multi-message segments with inter-segment typing
- **Voice notes** — probabilistically sends TTS audio for emotional or casual contexts
- **GIFs/stickers** — contextually sends media when conversation tone matches
- **Late-night gating** — higher skip rates and briefer responses between 2-6 AM

## What Gets Tested

A successful run proves every layer of the stack works in production conditions:

1. **Inbound poll** — `chat.db` SQLite queries, `last_rowid` tracking, `allow_from` filtering
2. **Message batching** — consecutive messages from the same sender merged into one prompt
3. **Response classifier** — eager mode overrides selective gating
4. **Agent turn** — full LLM round-trip (provider, tools, context, history)
5. **Outbound send** — `imsg` CLI or AppleScript delivery
6. **Echo suppression** — sent-message ring prevents processing our own outbound as inbound
7. **Turn limiting** — daemon auto-stops after the configured number of responses

## Troubleshooting

### Agent goes silent after 3 replies

The consecutive reply limiter is active. Ensure `daemon.max_consecutive_replies` is set to `0` in both machines' config (the planned `human channel e2e-test` would override this automatically when it exists).

### Messages not being picked up

- Check `allow_from` includes the other agent's handle (exact match on phone number or email)
- Verify Full Disk Access is granted for the terminal app
- Check poll interval: default is 30s, which means up to 30s latency per turn
- Run `human doctor` to verify iMessage health

### Send failures

- If using `imsg` CLI: verify `which imsg` returns a path
- If using AppleScript: Messages.app must be running
- Check the console for `[e2e]` prefixed log lines

### Persona blocking

If you have a persona configured, the other agent's handle must appear as a contact in the persona file. Unknown senders are dropped when persona is active.

## Architecture Notes

The E2E test reuses the production daemon loop unchanged — there is no special "test mode" bypass. The only differences from normal operation are:

- **Consecutive reply limiter disabled** (`max_consecutive_replies: 0`) — normally caps at 3 to prevent runaway loops with real humans
- **Turn budget enforced** (`e2e_max_turns: N`) — ensures the test terminates
- **Eager response mode** — responds to everything instead of selectively gating

This means a successful E2E run is a genuine proof that the production pipeline works.
