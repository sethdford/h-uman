---
title: Migration from OpenClaw to Human
description: Guide to migrating from OpenClaw (TypeScript) to Human (C, static binary)
updated: 2026-03-02
---

# Migration from OpenClaw to Human

This guide walks you through migrating from [OpenClaw](https://github.com/openclaw/openclaw) (TypeScript, Node.js) to Human (C, static binary). Human is a drop-in replacement that preserves your memory, config structure, and channel integrations while dramatically reducing resource usage and improving security.

## Why migrate?

| Benefit          | OpenClaw                                                                       | Human                                         |
| ---------------- | ------------------------------------------------------------------------------ | ----------------------------------------------- |
| **Cost**         | $599+ setup (typical M-series Mac), $300+/mo API overhead from bloated runtime | $5 hardware—runs on ARM SBCs, Raspberry Pi Zero |
| **Binary size**  | ~28 MB (npm dist)                                                              | **~528 KB** core binary                         |
| **Memory**       | Node.js heap (100+ MB typical)                                                 | **< 5 MB** peak RSS                             |
| **Dependencies** | 1,200+ npm packages, Node.js ≥22                                               | **0**—libc + optional SQLite, libcurl           |
| **Supply chain** | Large attack surface, malicious skill risk                                     | Single binary, curated skill registry           |
| **Secrets**      | Plain-text API keys in config                                                  | ChaCha20-Poly1305 encrypted at rest             |
| **Privacy**      | Telemetry possible via ecosystem                                               | No telemetry, full data ownership               |

## Prerequisites

1. **Install Human binary** — either:
   - **Build from source** (recommended for your platform):

     ```bash
     git clone https://github.com/sethdford/human.git
     cd human
     mkdir -p build && cd build
     cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON -DSC_ENABLE_ALL_CHANNELS=ON
     cmake --build .
     sudo cp human /usr/local/bin/
     ```

   - **Download release** (when available):
     ```bash
     # Check releases for your platform
     curl -Lo human https://github.com/sethdford/human/releases/latest/download/human-$(uname -m)
     chmod +x human
     sudo mv human /usr/local/bin/
     ```

2. **Locate your OpenClaw data**:
   - Config: `~/.openclaw/config.json` (or `./config.json` in project root)
   - Memory (brain): typically `~/.openclaw/brain.db` or project-local `brain.db`

## 1. Automatic memory migration

Human can import memories directly from OpenClaw’s SQLite brain database.

```bash
# Migrate from OpenClaw brain.db into Human's default memory location
human migrate sqlite ~/.openclaw/brain.db

# Or from a project-local brain
human migrate sqlite /path/to/openclaw-project/brain.db
```

Output example:

```
Migration: 42 from sqlite, 0 from markdown, 42 imported
```

The migration tool detects common schema variants (OpenClaw, ZeroClaw) and maps `key`/`id`/`name`, `content`/`value`/`text`/`memory`, and `category`/`kind`/`type` automatically. Memories are written to Human’s configured backend (SQLite or Markdown by default).

## 2. Config migration

Human uses the **same config structure** as OpenClaw (snake_case). Most keys map 1:1.

### Key mappings

| OpenClaw                             | Human                              | Notes                                |
| ------------------------------------ | ------------------------------------ | ------------------------------------ |
| `providers` (top-level)              | `providers` (top-level array)        | Same structure                       |
| `default_provider` / `default_model` | `default_provider` / `default_model` | Same top-level keys                  |
| `accounts` (per channel)             | `channels.<channel>.accounts`        | Same structure                       |
| `clients`                            | `providers`                          | Each client becomes a provider entry |

### Example: OpenClaw config → Human config

**OpenClaw (`config.json`):**

```json
{
  "default_provider": "openai",
  "default_model": "gpt-4o",
  "providers": [
    {
      "name": "openai",
      "api_key": "sk-proj-xxx"
    }
  ]
}
```

**Human (`~/.human/config.json`):**

```json
{
  "models": {
    "providers": {
      "openai": {
        "api_key": "sk-proj-xxx"
      }
    }
  },
  "agents": {
    "defaults": {
      "model": { "primary": "gpt-4o" }
    }
  }
}
```

### Providers

- Map each OpenClaw provider to the top-level `providers` array.
- Human expects array form: `[{ "name": "...", "api_key": "...", "base_url": "..." }]`.

### Channels

Channel structure is compatible. Copy `channels` (or `accounts` under each channel) into Human’s `channels` section. See [Channel-by-channel setup](#3-channel-by-channel-setup).

## 3. Channel-by-channel setup

Human supports the same channels as OpenClaw. After migrating config, enable them one by one.

| Channel      | Config path                  | Notes                                         |
| ------------ | ---------------------------- | --------------------------------------------- |
| **Telegram** | `channels.telegram.accounts` | `bot_token`, `allow_from`, `reply_in_private` |
| **Discord**  | `channels.discord.accounts`  | `token`, `guild_id`, `allow_from`             |
| **Slack**    | `channels.slack.accounts`    | `bot_token`, `app_token`, `allow_from`        |
| **Signal**   | `channels.signal.accounts`   | Signal device pairing                         |
| **Nostr**    | `channels.nostr`             | `private_key`, `owner_pubkey`, `relays`       |
| **IRC**      | `channels.irc.accounts`      | `host`, `port`, `nick`, `channel`, `tls`      |
| **Matrix**   | `channels.matrix.accounts`   | Access token, server URL                      |
| **WhatsApp** | `channels.whatsapp.accounts` | Meta webhook config                           |
| **Webhook**  | `channels.webhook.accounts`  | Generic HTTP webhook                          |

**Example — Telegram:**

```json
{
  "channels": {
    "telegram": {
      "accounts": {
        "main": {
          "bot_token": "123456:ABC-DEF...",
          "allow_from": ["@username"],
          "reply_in_private": true
        }
      }
    }
  }
}
```

Run `human onboard --channels-only` to add or adjust channels interactively.

## 4. Skill migration

OpenClaw skills are npm packages or local JSON/TS modules. Human uses a **curated skill registry** with TOML manifests and `SKILL.md` instructions.

### Finding equivalents

| OpenClaw skill type     | Human equivalent                     |
| ----------------------- | -------------------------------------- |
| npm `@openclaw/skill-*` | Registry skill with same purpose       |
| Custom/local skills     | Convert to `*.skill.json` + `SKILL.md` |

**Search and install from registry:**

```bash
human skills search code
human skills install code-review
human skills list
```

**Skill registry:** [https://github.com/human/skill-registry](https://github.com/human/skill-registry)

### Converting a custom skill

1. Create `skills/<name>/<name>.skill.json` with:
   - `name`, `version`, `description`, `author`
   - `tools`, `system_prompt`, `triggers`, `config`
2. Add `SKILL.md` with usage and config docs.
3. Optionally submit a PR to the [skill registry](https://github.com/human/skill-registry).

## 5. Verify migration

```bash
human doctor          # Config, provider, memory OK?
human status          # Full system status
human agent -m "What do you remember about me?"
human channel status  # Channel health
```

## FAQ / Common issues

### "Config error" or "Provider not found"

- Ensure top-level `providers` array exists and each provider has `api_key` (or `base_url` for local).
- Use top-level `default_provider` and `default_model` for model selection.

### "Migration: 0 imported"

- Check that the SQLite path is correct and the file exists.
- OpenClaw brain may use a different schema; the migrator auto-detects common column names. If it still fails, open an issue with your schema.

### Channel not receiving messages

- Verify `allow_from` is set (empty = deny all; `"*"` = allow all).
- For webhook channels (Telegram, etc.), ensure the gateway is running (`human gateway`) and reachable (tunnel or port forwarding).

### Secrets in plain text after migration

- Run `human onboard` or ensure `"secrets": { "encrypt": true }` in config.
- Re-add API keys via onboard so they are encrypted with ChaCha20-Poly1305.

### Memory backend mismatch

- Human defaults to SQLite for full installs, Markdown for minimal. Set `memory.backend` in config (`"sqlite"` or `"markdown"`).
