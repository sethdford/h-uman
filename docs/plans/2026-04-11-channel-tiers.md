---
title: "M6: Channel Tiering — Depth Over Breadth"
created: 2026-04-11
status: closed
scope: channel prioritization
parent: 2026-04-11-strategic-missions.md
last_audit: 2026-05-25
---

# M6: Channel Tiering — Depth Over Breadth

31 channels. 43 `.c` files. One persona. Focus wins.

## Tier 1 — Full Persona Experience

Channels where persona *shines*: reactions, typing indicators, attachments, conversation history, response constraints, human-activity detection. These get dedicated persona tuning, per-channel eval suites, and priority bug fixes.

| Channel | Reactions | Typing | Attachments | History | Constraints | Persona Overlay |
|---------|-----------|--------|-------------|---------|-------------|-----------------|
| **Telegram** | Yes (`setMessageReaction`) | Yes | Yes | Yes | Yes | `"telegram"` |
| **Discord** | Yes (emoji) | Yes | Yes | Yes | Yes | `"discord"` |
| **iMessage** | Yes (tapbacks + context) | Yes | Yes (+ read receipts) | Yes | Yes | `"imessage"` |
| **Slack** | Yes | Yes | Yes | Yes | Yes | `"slack"` |

**Why these four:** They implement the full `hu_channel_vtable_t` surface — `react`, `start_typing`/`stop_typing`, `get_attachment_path`, `load_conversation_history`, `get_response_constraints`, `human_active_recently`. Persona timing (typing delays, jitter, read-before-reply) is visible and meaningful.

**Test depth:** iMessage has 4+ dedicated test files. Telegram, Discord, Slack have strong coverage in `test_channel_all.c` plus embeds.

## Tier 2 — Core + Maintained

Channels that are strategically important (CLI for dev, Web for dashboard) or well-tested but missing one or more Tier 1 interaction features. Maintained, tested, but not prioritized for persona polish.

| Channel | Missing vs Tier 1 | Strategic Role |
|---------|-------------------|----------------|
| **CLI** | No reactions, no typing | Developer primary interface |
| **Web** | No reactions, no typing | Dashboard streaming |
| **WhatsApp** | No conversation history loading | High-reach messaging |
| **Signal** | No attachment resolution | Privacy-focused audience |
| **Matrix** | No response constraints | Open-protocol audience |
| **Gmail** | Async (email) — reactions/typing N/A | Email integration |
| **Email/IMAP** | Async — reactions/typing N/A | Email integration |
| **Teams** | Stubs unless Graph wired | Enterprise audience |
| **Mattermost** | No response constraints | Self-hosted audience |

## Tier 3 — Community / Experimental

Channels maintained for breadth but not prioritized. May have limited vtable implementation, send-only mode, or niche audience. Community contributions welcome.

| Channel | Notes |
|---------|-------|
| **IRC** | Text-only, no rich features |
| **LINE** | Regional (Japan/SE Asia) |
| **Lark** | Regional (China/enterprise) |
| **OneBot** | QQ/WeChat protocol bridge |
| **DingTalk** | Regional (China/enterprise) |
| **QQ** | Regional (China) |
| **Nostr** | Protocol/crypto community |
| **MQTT** | IoT protocol, not conversational |
| **MaixCam** | Send-only, hardware |
| **Dispatch** | Meta/routing channel |
| **Voice (Sonata)** | Send-only |
| **Twilio** | SMS bridge |
| **Google Chat** | Google Workspace |
| **Google RCS** | Carrier messaging |
| **Facebook** | Meta platform |
| **Instagram** | Meta platform |
| **Twitter/X** | Social, not conversational |

## How Persona Overlays Work

Overlays are NOT per-channel code. They're loaded from persona JSON and matched at prompt-build time by exact string equality on the overlay's `channel` field vs `agent->active_channel`. Any channel can use overlays if the persona JSON includes a matching key.

Overlay lookup: `hu_persona_find_overlay(persona, channel, channel_len)` in `src/persona/persona.c`.

## Action Items

1. **Create per-channel persona overlay defaults** for Tier 1 channels in the starter persona
2. **Write per-channel naturalness eval suites** (20 conversations each) for Tier 1
3. **Audit Tier 1 channel timing** — verify typing delays, jitter, read-before-reply work correctly
4. **Document Tier 2/3 expectations** — what level of support to expect, how to contribute
