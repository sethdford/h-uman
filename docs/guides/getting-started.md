---
title: Getting Started with SeaClaw
description: Quick setup guide from zero to first AI chat in under 5 minutes
updated: 2026-03-02
---

# Getting Started with SeaClaw

Get from zero to your first AI chat in under 5 minutes.

## 1. Install

### Option A: Build from source (recommended)

**Prerequisites:** C compiler (gcc or clang), CMake 3.16+, optionally SQLite3 and libcurl.

- **macOS:** `brew install cmake sqlite3`
- **Linux:** `sudo apt-get install cmake libsqlite3-dev libcurl4-openssl-dev`

```bash
git clone https://github.com/sethdford/seaclaw.git
cd seaclaw
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON -DSC_ENABLE_ALL_CHANNELS=ON
cmake --build .
```

The binary is `build/seaclaw`. Add it to your PATH or copy it:

```bash
sudo cp build/seaclaw /usr/local/bin/
```

### Option B: Download binary (when available)

Check [releases](https://github.com/sethdford/seaclaw/releases) for your platform. Download and place `seaclaw` in your PATH.

## 2. Quick setup

Run the interactive wizard to create your config:

```bash
seaclaw onboard --interactive
```

The wizard will:

- Choose a provider (OpenAI, Anthropic, OpenRouter, Ollama, etc.)
- Prompt for an API key (or skip for local providers)
- Pick a default model
- Optionally configure channels (Telegram, Discord, Nostr, etc.)
- Create `~/.seaclaw/config.json` and workspace templates

**Fast path** (skip wizard):

```bash
seaclaw onboard --api-key sk-... --provider openrouter
```

## 3. First chat

Send a single message:

```bash
seaclaw agent -m "Hello!"
```

Or start an interactive session:

```bash
seaclaw agent
```

Type messages and press Enter. Use `Ctrl+C` to exit.

**Full-screen TUI** (split panes, tabs, streaming):

```bash
seaclaw agent --tui
```

## 4. Gateway mode

To receive webhooks (Telegram, Slack, Discord, etc.), start the gateway:

```bash
seaclaw gateway
```

By default it binds to `127.0.0.1:3000`. To pair:

1. Get the 6-digit pairing code from logs or `seaclaw status`
2. `curl -X POST http://127.0.0.1:3000/pair -H "X-Pairing-Code: 123456"`
3. Use the returned bearer token for `/webhook` requests

Custom port:

```bash
seaclaw gateway --port 8080
```

## 5. Channel setup (Telegram example)

1. Create a bot via [@BotFather](https://t.me/BotFather).
2. Add the bot token in config under `channels.telegram.accounts`:

```json
{
  "channels": {
    "telegram": {
      "accounts": {
        "main": {
          "bot_token": "123456:ABC-DEF...",
          "allow_from": ["@your_username"],
          "reply_in_private": true
        }
      }
    }
  }
}
```

3. Start the gateway (`seaclaw gateway`). Configure Telegram’s webhook to point at your gateway (use a tunnel like ngrok or Tailscale if not on localhost).

Or use the interactive wizard:

```bash
seaclaw onboard --channels-only
```

## 6. Install skills

Search the skill registry and install:

```bash
seaclaw skills search code
seaclaw skills install code-review
seaclaw skills list
```

Update installed skills:

```bash
seaclaw skills update
```

Skill registry: [https://github.com/seaclaw/skill-registry](https://github.com/seaclaw/skill-registry)

## 7. MCP server mode

Expose SeaClaw tools via the Model Context Protocol for Claude Desktop, Cursor, or other MCP clients:

```bash
seaclaw mcp
```

Configure your MCP client to run `seaclaw mcp` as the server command. SeaClaw will provide its tools (file read, shell, git, memory, etc.) to the host application.

**Example** (Claude Desktop `claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "seaclaw": {
      "command": "seaclaw",
      "args": ["mcp"]
    }
  }
}
```

## 8. Next steps

- **Security hardening:** Enable `secrets.encrypt`, set `gateway.require_pairing`, configure `allow_from` per channel. See [security/threat-model.md](security/threat-model.md).
- **Memory:** Use SQLite with embeddings for richer recall. Configure `memory.backend`, `embedding_provider`, and `vector_weight` in config.
- **Peripherals:** Flash Arduino, STM32, or Raspberry Pi with `seaclaw hardware scan` and `seaclaw hardware flash`.
- **Background service:** `seaclaw service install` to run gateway + channels as a system service.
- **Cron tasks:** `seaclaw cron add "0 9 * * *" "daily digest"` for scheduled jobs.

Run `seaclaw --help` for the full command reference.
