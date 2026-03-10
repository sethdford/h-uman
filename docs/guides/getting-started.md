---
title: Getting Started with Human
description: Quick setup guide from zero to first AI chat in under 5 minutes
updated: 2026-03-02
---

# Getting Started with Human

Get from zero to your first AI chat in under 5 minutes.

## 1. Install

### Option A: Build from source (recommended)

**Prerequisites:** C compiler (gcc or clang), CMake 3.16+, optionally SQLite3 and libcurl.

- **macOS:** `brew install cmake sqlite3`
- **Linux:** `sudo apt-get install cmake libsqlite3-dev libcurl4-openssl-dev`

```bash
git clone https://github.com/sethdford/human.git
cd human
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DHU_ENABLE_LTO=ON -DHU_ENABLE_ALL_CHANNELS=ON
cmake --build .
```

The binary is `build/human`. Add it to your PATH or copy it:

```bash
sudo cp build/human /usr/local/bin/
```

### Option B: Download binary (when available)

Check [releases](https://github.com/sethdford/human/releases) for your platform. Download and place `human` in your PATH.

## 2. Quick setup

Run the interactive wizard to create your config:

```bash
human onboard --interactive
```

The wizard will:

- Choose a provider (OpenAI, Anthropic, OpenRouter, Ollama, etc.)
- Prompt for an API key (or skip for local providers)
- Pick a default model
- Optionally configure channels (Telegram, Discord, Nostr, etc.)
- Create `~/.human/config.json` and workspace templates

**Fast path** (skip wizard):

```bash
human onboard --api-key sk-... --provider openrouter
```

## 3. First chat

Send a single message:

```bash
human agent -m "Hello!"
```

Or start an interactive session:

```bash
human agent
```

Type messages and press Enter. Use `Ctrl+C` to exit.

**Full-screen TUI** (split panes, tabs, streaming):

```bash
human agent --tui
```

## 4. Gateway mode

To receive webhooks (Telegram, Slack, Discord, etc.), start the gateway:

```bash
human gateway
```

By default it binds to `127.0.0.1:3000`. To pair:

1. Get the 6-digit pairing code from logs or `human status`
2. `curl -X POST http://127.0.0.1:3000/pair -H "X-Pairing-Code: 123456"`
3. Use the returned bearer token for `/webhook` requests

Custom port:

```bash
human gateway --port 8080
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

3. Start the gateway (`human gateway`). Configure Telegram’s webhook to point at your gateway (use a tunnel like ngrok or Tailscale if not on localhost).

Or use the interactive wizard:

```bash
human onboard --channels-only
```

## 6. Install skills

Search the skill registry and install:

```bash
human skills search code
human skills install code-review
human skills list
```

Update installed skills:

```bash
human skills update
```

Skill registry: [https://github.com/human/skill-registry](https://github.com/human/skill-registry)

## 7. MCP server mode

Expose Human tools via the Model Context Protocol for Claude Desktop, Cursor, or other MCP clients:

```bash
human mcp
```

Configure your MCP client to run `human mcp` as the server command. Human will provide its tools (file read, shell, git, memory, etc.) to the host application.

**Example** (Claude Desktop `claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "human": {
      "command": "human",
      "args": ["mcp"]
    }
  }
}
```

## 8. Next steps

- **Security hardening:** Enable `secrets.encrypt`, set `gateway.require_pairing`, configure `allow_from` per channel. See [security/threat-model.md](security/threat-model.md).
- **Memory:** Use SQLite with embeddings for richer recall. Configure `memory.backend`, `embedding_provider`, and `vector_weight` in config.
- **Peripherals:** Flash Arduino, STM32, or Raspberry Pi with `human hardware scan` and `human hardware flash`.
- **Background service:** `human service install` to run gateway + channels as a system service.
- **Cron tasks:** `human cron add "0 9 * * *" "daily digest"` for scheduled jobs.

Run `human --help` for the full command reference.
