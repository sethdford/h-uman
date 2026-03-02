<p align="center">
  <img src="seaclaw.svg" alt="seaclaw" width="200" />
</p>

<h1 align="center">SeaClaw</h1>

<p align="center">
  <strong>Bring AI to every device on Earth.</strong><br>
  <strong>282 KB binary. < 5 MB RAM. Boots in <2 ms. Runs on anything with a CPU.</strong>
</p>

<p align="center">
  <a href="https://github.com/sethdford/seaclaw/actions/workflows/seaclaw-ci.yml"><img src="https://github.com/sethdford/seaclaw/actions/workflows/seaclaw-ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://sethdford.github.io/seaclaw"><img src="https://img.shields.io/badge/docs-sethdford.github.io/seaclaw-informational" alt="Documentation" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT" /></a>
</p>

The smallest fully autonomous AI assistant infrastructure — a static C binary that fits on any $5 board, boots in milliseconds, and requires nothing but libc.

```
282 KB binary · <2 ms startup · 1,946 tests · 50+ providers · 20 channels · 47 tools · Pluggable everything
```

### Features

- **Impossibly Small:** 282 KB static binary — no runtime, no VM, no framework overhead.
- **Near-Zero Memory:** < 5 MB peak RSS. Runs comfortably on the cheapest ARM SBCs and microcontrollers.
- **Instant Startup:** <2 ms on Apple Silicon, <8 ms on a 0.8 GHz edge core.
- **True Portability:** Single self-contained binary across ARM, x86, and RISC-V. Drop it anywhere, it just runs.
- **Feature-Complete:** 50+ providers, 20 channels, 47 tools, hybrid vector+FTS5 memory, multi-layer sandbox, tunnels, hardware peripherals, MCP, subagents, streaming, voice — the full stack.
- **Interactive TUI:** Full-screen terminal UI with split panes, markdown rendering, multi-session tabs (Ctrl+T), tool approval prompts, streaming output, and input history. Optional `--tui` flag.
- **Performance-Optimized:** Per-turn arena allocator, HTTP connection pooling, HTTP/2, system prompt caching — all benefiting from C-level control.

### Why seaclaw

- **Lean by default:** C11 compiles to a tiny static binary. No runtime overhead, no garbage collector, no framework.
- **Secure by design:** pairing, strict sandboxing (landlock, firejail, bubblewrap, docker), explicit allowlists, workspace scoping, encrypted secrets.
- **Fully swappable:** core systems are vtable interfaces (providers, channels, tools, memory, tunnels, peripherals, observers, runtimes).
- **No lock-in:** OpenAI-compatible provider support + pluggable custom endpoints.

## Landscape

Similar projects in the autonomous AI assistant space (data sourced from each project's own documentation):

|                   | [OpenClaw](https://github.com/openclaw/openclaw) | [NanoBot](https://github.com/HKUDS/nanobot) | [PicoClaw](https://github.com/sipeed/picoclaw) | [ZeroClaw](https://github.com/zeroclaw-labs/zeroclaw) | **SeaClaw**       |
| ----------------- | ------------------------------------------------ | ------------------------------------------- | ---------------------------------------------- | ----------------------------------------------------- | ----------------- |
| **Language**      | TypeScript                                       | Python                                      | Go                                             | Rust                                                  | **C**             |
| **RAM** ¹         | —                                                | —                                           | < 10 MB                                        | < 5 MB                                                | **< 5 MB**        |
| **Binary Size** ¹ | ~28 MB (npm dist)                                | N/A (Python)                                | ~8 MB                                          | ~8.8 MB                                               | **282 KB**        |
| **Runtime Deps**  | Node.js ≥22                                      | Python ≥3.11                                | None (static)                                  | None (static)                                         | **None (static)** |

> ¹ RAM and binary size figures for other projects are self-reported from their respective READMEs. SeaClaw's numbers are measured locally with `/usr/bin/time -l` on a MinSizeRel + LTO build.

SeaClaw's verified numbers (measured on macOS arm64, March 2026):

```
Binary size:   282 KB core / 380 KB full (MinSizeRel + LTO)
Peak RSS:      < 5 MB
Startup:       <2 ms (Apple Silicon M4 Max)
Tests:         1,946 passing, 0 ASan errors
```

Reproduce locally:

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON
cmake --build .
ls -lh seaclaw

/usr/bin/time -l ./seaclaw --help
/usr/bin/time -l ./seaclaw status
```

## Quick Start

> **Prerequisites:** C compiler (gcc or clang), CMake 3.16+, and optionally SQLite3 and libcurl.
> macOS: `brew install cmake sqlite3`
> Linux: `sudo apt-get install cmake libsqlite3-dev libcurl4-openssl-dev`

```bash
git clone https://github.com/sethdford/seaclaw.git
cd seaclaw
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON -DSC_ENABLE_ALL_CHANNELS=ON
cmake --build .
cd ..

# Quick setup
seaclaw onboard --api-key sk-... --provider openrouter

# Or interactive wizard
seaclaw onboard --interactive

# Chat
seaclaw agent -m "Hello, seaclaw!"

# Interactive mode
seaclaw agent

# Start gateway runtime (gateway + all configured channels/accounts + heartbeat + scheduler)
seaclaw gateway                # default: 127.0.0.1:3000
seaclaw gateway --port 8080    # custom port

# Check status
seaclaw status

# Run system diagnostics
seaclaw doctor

# Check channel health
seaclaw channel status

# Start specific channels
seaclaw channel start telegram
seaclaw channel start discord
seaclaw channel start signal

# Manage background service
seaclaw service install
seaclaw service status

# Migrate memory from OpenClaw
seaclaw migrate openclaw --dry-run
seaclaw migrate openclaw
```

> **Dev fallback (no global install):** prefix commands with `build/` (example: `build/seaclaw status`).

## Edge MVP (Hybrid Host + WASM Logic)

If you want edge deployment (Cloudflare Worker) with Telegram + OpenAI while keeping agent policy in WASM, see:

`examples/edge/cloudflare-worker/`

This pattern keeps networking/secrets in the edge host and lets you swap/update logic by replacing a WASM module.

## Architecture

Every subsystem is a **vtable interface** — swap implementations with a config change, zero code changes.

| Subsystem         | Interface        | Ships with                                                                                                                                                     | Extend                                                    |
| ----------------- | ---------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------- |
| **AI Models**     | `Provider`       | 50+ providers (OpenRouter, Anthropic, OpenAI, Gemini, Ollama, llama.cpp, Groq, Mistral, xAI, DeepSeek, Together, Fireworks, Perplexity, Cohere, Bedrock, etc.) | `custom:https://your-api.com` — any OpenAI-compatible API |
| **Channels**      | `Channel`        | CLI, Telegram, Signal, Discord, Slack, iMessage, Matrix, WhatsApp, Webhook, IRC, Lark/Feishu, OneBot, Line, DingTalk, Email, Nostr, QQ, MaixCam, Mattermost    | Any messaging API                                         |
| **Memory**        | `Memory`         | SQLite with hybrid search (FTS5 + vector cosine similarity), Markdown                                                                                          | Any persistence backend                                   |
| **Tools**         | `Tool`           | 47 built-in: shell, file ops, git, memory, browser, screenshot, composio, http, cron, hardware, web search, delegate, and more                                 | Any capability                                            |
| **Observability** | `Observer`       | Noop, Log, File, Multi                                                                                                                                         | Prometheus, OTel                                          |
| **Runtime**       | `RuntimeAdapter` | Native, Docker (sandboxed), WASM (wasmtime)                                                                                                                    | Any runtime                                               |
| **Security**      | `Sandbox`        | Landlock, Firejail, Bubblewrap, Docker, auto-detect                                                                                                            | Any sandbox backend                                       |
| **Identity**      | `IdentityConfig` | OpenClaw (markdown), AIEOS v1.1 (JSON)                                                                                                                         | Any identity format                                       |
| **Tunnel**        | `Tunnel`         | None, Cloudflare, Tailscale, ngrok, Custom                                                                                                                     | Any tunnel binary                                         |
| **Heartbeat**     | Engine           | [`src/heartbeat.c`](src/heartbeat.c) periodic tasks                                                                                                            | —                                                         |
| **Skills**        | Loader           | TOML manifests + SKILL.md instructions                                                                                                                         | Community skill packs                                     |
| **Peripherals**   | `Peripheral`     | Serial, Arduino, Raspberry Pi GPIO, STM32/Nucleo                                                                                                               | Any hardware interface                                    |
| **Cron**          | Scheduler        | Cron expressions + one-shot timers with JSON persistence                                                                                                       | —                                                         |

### Memory System

All custom, zero external dependencies:

| Layer              | Implementation                                                |
| ------------------ | ------------------------------------------------------------- |
| **Vector DB**      | Embeddings stored as BLOB in SQLite, cosine similarity search |
| **Keyword Search** | FTS5 virtual tables with BM25 scoring                         |
| **Hybrid Merge**   | Weighted merge (configurable vector/keyword weights)          |
| **Embeddings**     | `EmbeddingProvider` vtable — OpenAI, custom URL, or noop      |
| **Hygiene**        | Automatic archival + purge of stale memories                  |
| **Snapshots**      | Export/import full memory state for migration                 |

```json
{
  "memory": {
    "backend": "sqlite",
    "auto_save": true,
    "embedding_provider": "openai",
    "vector_weight": 0.7,
    "keyword_weight": 0.3,
    "hygiene_enabled": true,
    "snapshot_enabled": false
  }
}
```

## Security

seaclaw enforces security at **every layer**.

| #   | Item                             | Status | How                                                                                                  |
| --- | -------------------------------- | ------ | ---------------------------------------------------------------------------------------------------- |
| 1   | **Gateway not publicly exposed** | Done   | Binds `127.0.0.1` by default. Refuses `0.0.0.0` without tunnel or explicit `allow_public_bind`.      |
| 2   | **Pairing required**             | Done   | 6-digit one-time code on startup. Exchange via `POST /pair` for bearer token.                        |
| 3   | **Filesystem scoped**            | Done   | `workspace_only = true` by default. Null byte injection blocked. Symlink escape detection.           |
| 4   | **Access via tunnel only**       | Done   | Gateway refuses public bind without active tunnel. Supports Tailscale, Cloudflare, ngrok, or custom. |
| 5   | **Sandbox isolation**            | Done   | Auto-detects best backend: Landlock, Firejail, Bubblewrap, or Docker.                                |
| 6   | **Encrypted secrets**            | Done   | API keys encrypted with ChaCha20-Poly1305 using local key file.                                      |
| 7   | **Resource limits**              | Done   | Configurable memory, CPU, disk, and subprocess limits.                                               |
| 8   | **Audit logging**                | Done   | Signed event trail with configurable retention.                                                      |

### Channel Allowlists

- Empty allowlist = **deny all inbound messages**
- `"*"` = **allow all** (explicit opt-in)
- Otherwise = exact-match allowlist

Nostr additionally: the `owner_pubkey` is **always** allowed regardless of `dm_allowed_pubkeys`. Private keys are encrypted at rest via SecretStore (`enc2:` prefix) and only decrypted into memory while the channel is running; zeroed on channel stop.

### Nostr Channel Setup

`seaclaw` speaks Nostr natively via NIP-17 (gift-wrapped private DMs) and NIP-04 (legacy DMs), using [`nak`](https://github.com/fiatjaf/nak).

**Prerequisites:** Install `nak` and ensure it's in your `$PATH`.

**Setup via onboarding wizard:**

```bash
seaclaw onboard --interactive   # Step 7 configures Nostr
```

The wizard will:

1. Generate a new keypair for your bot or import a key & encrypt it with ChaCha20-Poly1305
2. Ask for your (owner) pubkey (npub or hex) — always allowed through DM policy
3. Configure relays and DM relays (kind:10050 inbox)
4. Display the bot's pubkey

Or configure manually in the [config](#configuration).

**How it works:** On startup, seaclaw announces its DM inbox relays (kind:10050), then listens for incoming NIP-17 gift wraps and NIP-04 encrypted DMs. Outbound messages mirror the sender's protocol. Multi-relay rumor deduplication prevents duplicate responses when the same message is delivered via multiple relays.

## Configuration

Config: `~/.seaclaw/config.json` (created by `onboard`)

> **OpenClaw compatible:** seaclaw uses the same config structure as [OpenClaw](https://github.com/openclaw/openclaw) (snake_case). Providers live under `models.providers`, the default model under `agents.defaults.model.primary`, and channels use `accounts` wrappers.
> Top-level `default_provider` / `default_model` keys are not supported.

```json
{
  "default_temperature": 0.7,

  "models": {
    "providers": {
      "openrouter": { "api_key": "sk-or-..." },
      "groq": { "api_key": "gsk_..." },
      "anthropic": {
        "api_key": "sk-ant-...",
        "base_url": "https://api.anthropic.com"
      }
    }
  },

  "agents": {
    "defaults": {
      "model": { "primary": "openrouter/anthropic/claude-sonnet-4" },
      "heartbeat": { "every": "30m" }
    },
    "list": [
      {
        "id": "researcher",
        "model": { "primary": "openrouter/anthropic/claude-opus-4" },
        "system_prompt": "..."
      }
    ]
  },

  "channels": {
    "telegram": {
      "accounts": {
        "main": {
          "bot_token": "123:ABC",
          "allow_from": ["user1"],
          "reply_in_private": true,
          "proxy": "socks5://..."
        }
      }
    },
    "discord": {
      "accounts": {
        "main": {
          "token": "disc-token",
          "guild_id": "12345",
          "allow_from": ["user1"],
          "allow_bots": false
        }
      }
    },
    "irc": {
      "accounts": {
        "main": {
          "host": "irc.libera.chat",
          "port": 6697,
          "nick": "seaclaw",
          "channel": "#seaclaw",
          "tls": true,
          "allow_from": ["user1"]
        }
      }
    },
    "slack": {
      "accounts": {
        "main": {
          "bot_token": "xoxb-...",
          "app_token": "xapp-...",
          "allow_from": ["user1"]
        }
      }
    },
    "nostr": {
      "private_key": "enc2:...",
      "owner_pubkey": "hex-pubkey-of-owner",
      "relays": [
        "wss://relay.damus.io",
        "wss://nos.lol",
        "wss://relay.nostr.band"
      ],
      "dm_allowed_pubkeys": ["*"],
      "display_name": "SeaClaw",
      "about": "AI assistant on Nostr",
      "nip05": "seaclaw@yourdomain.com",
      "lnurl": "lnurl1..."
    }
  },

  "tools": {
    "media": {
      "audio": {
        "enabled": true,
        "language": "ru",
        "models": [{ "provider": "groq", "model": "whisper-large-v3" }]
      }
    }
  },

  "mcp_servers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem"]
    }
  },

  "memory": {
    "backend": "sqlite",
    "auto_save": true,
    "embedding_provider": "openai",
    "vector_weight": 0.7,
    "keyword_weight": 0.3
  },

  "gateway": {
    "port": 3000,
    "require_pairing": true,
    "allow_public_bind": false
  },

  "autonomy": {
    "level": "supervised",
    "workspace_only": true,
    "max_actions_per_hour": 20
  },

  "runtime": {
    "kind": "native",
    "docker": {
      "image": "alpine:3.20",
      "network": "none",
      "memory_limit_mb": 512,
      "read_only_rootfs": true
    }
  },

  "tunnel": { "provider": "none" },
  "secrets": { "encrypt": true },
  "identity": { "format": "openclaw" },

  "security": {
    "sandbox": { "backend": "auto" },
    "resources": { "max_memory_mb": 512, "max_cpu_percent": 80 },
    "audit": { "enabled": true, "retention_days": 90 }
  }
}
```

### Full Web Search + Shell Access

Use this when you want full web-search provider control plus unrestricted shell command allowlist behavior:

```json
{
  "http_request": {
    "enabled": true,
    "search_base_url": "https://searx.example.com",
    "search_provider": "auto",
    "search_fallback_providers": ["jina", "duckduckgo"]
  },
  "autonomy": {
    "level": "full",
    "allowed_commands": ["*"],
    "allowed_paths": ["*"],
    "require_approval_for_medium_risk": false,
    "block_high_risk_commands": false
  }
}
```

- `http_request.search_base_url` accepts either instance root (`https://host`) or explicit endpoint (`https://host/search`).
- Invalid `http_request.search_base_url` now fails config validation at startup (no automatic fallback for malformed URL).
- `http_request.search_provider` supports: `auto`, `searxng`, `duckduckgo` (`ddg`), `brave`, `firecrawl`, `tavily`, `perplexity`, `exa`, `jina`.
- `http_request.search_fallback_providers` is optional and is tried in order when the primary provider fails.
- Provider env vars: `BRAVE_API_KEY`, `FIRECRAWL_API_KEY`, `TAVILY_API_KEY`, `PERPLEXITY_API_KEY`, `EXA_API_KEY`, `JINA_API_KEY` (or shared `WEB_SEARCH_API_KEY` where supported). DuckDuckGo and SearXNG do not require API keys.
- `allowed_commands: ["*"]` enables wildcard command allowlist matching.
- `allowed_paths: ["*"]` allows access outside workspace, except system-protected paths.

### Web UI / Browser Relay

Use `channels.web` for browser UI events (WebChannel v1):

```json
{
  "channels": {
    "web": {
      "accounts": {
        "default": {
          "transport": "local",
          "listen": "127.0.0.1",
          "port": 32123,
          "path": "/ws",
          "auth_token": "replace-with-long-random-token",
          "allowed_origins": [
            "http://localhost:5173",
            "chrome-extension://your-extension-id"
          ]
        }
      }
    }
  }
}
```

- Local: keep `"listen": "127.0.0.1"`.
- Local and relay use the same pairing flow: send `pairing_request`, receive `pairing_result`, then include UI `access_token` in every `user_message`.
- `auth_token` is optional hardening for WebSocket upgrade and required when binding non-loopback addresses.
- Remote host: set `"listen": "0.0.0.0"` and terminate TLS at proxy/CDN (`wss://...`).
- UI/extension should live in a separate repository and connect via this WebSocket endpoint.
- Relay transport (outbound agent socket) is configured via:

```json
{
  "channels": {
    "web": {
      "accounts": {
        "default": {
          "transport": "relay",
          "relay_url": "wss://relay.seaclaw.io/ws/agent",
          "relay_agent_id": "default",
          "relay_token": "replace-with-relay-token",
          "relay_token_ttl_secs": 2592000,
          "relay_pairing_code_ttl_secs": 300,
          "relay_ui_token_ttl_secs": 86400,
          "relay_e2e_required": false
        }
      }
    }
  }
}
```

- Relay token lifecycle (dedicated): `relay_token` (config) -> `SEACLAW_RELAY_TOKEN` (env) -> persisted `web-relay-<account_id>` credential -> generated token.
- Relay UI handshake: send `pairing_request` with one-time `pairing_code`, receive `pairing_result` with UI `access_token` JWT (and optional `set_cookie` string for relay HTTP layer).
- Relay `user_message` must include valid UI JWT in `access_token` (top-level or `payload.access_token`).
- If E2E is enabled (`relay_e2e_required=true`), UI and agent exchange X25519 keys during pairing and send encrypted payloads in `payload.e2e`.
- WebChannel event envelope is defined in [`spec/webchannel_v1.json`](spec/webchannel_v1.json).

## Gateway API

| Endpoint    | Method | Auth                            | Description                                |
| ----------- | ------ | ------------------------------- | ------------------------------------------ |
| `/health`   | GET    | None                            | Health check (always public)               |
| `/pair`     | POST   | `X-Pairing-Code` header         | Exchange one-time code for bearer token    |
| `/webhook`  | POST   | `Authorization: Bearer <token>` | Send message: `{"message": "your prompt"}` |
| `/whatsapp` | GET    | Query params                    | Meta webhook verification                  |
| `/whatsapp` | POST   | None (Meta signature)           | WhatsApp incoming message webhook          |

## Commands

| Command                                           | Description                                            |
| ------------------------------------------------- | ------------------------------------------------------ |
| `onboard --api-key sk-... --provider openrouter`  | Quick setup with API key and provider                  |
| `onboard --interactive`                           | Full interactive wizard                                |
| `onboard --channels-only`                         | Reconfigure channels/allowlists only                   |
| `agent -m "..."`                                  | Single message mode                                    |
| `agent`                                           | Interactive chat mode (CLI)                            |
| `agent --tui`                                     | Full-screen terminal UI with tabs, approval, streaming |
| `gateway`                                         | Start long-running runtime (default: `127.0.0.1:3000`) |
| `service install\|start\|stop\|status\|uninstall` | Manage background service                              |
| `doctor`                                          | Diagnose system health                                 |
| `status`                                          | Show full system status                                |
| `channel status`                                  | Show channel health/status                             |
| `cron list\|add\|remove\|pause\|resume\|run`      | Manage scheduled tasks                                 |
| `skills list\|install\|remove\|info`              | Manage skill packs                                     |
| `hardware scan\|flash\|monitor`                   | Hardware device management                             |
| `models list\|info\|benchmark`                    | Model catalog                                          |
| `migrate openclaw [--dry-run] [--source PATH]`    | Import memory + migrate config from OpenClaw           |

## Development

Build and tests require a C11 compiler and CMake 3.16+.

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DSC_ENABLE_ALL_CHANNELS=ON
cmake --build .                            # Dev build
./seaclaw_tests                             # 1,946 tests
cd ..
```

**ASan (AddressSanitizer)** for leak/overflow detection during development:

```bash
cmake -B build -DSC_ENABLE_ASAN=ON
cmake --build build
./build/seaclaw_tests
```

Release build (282 KB core):

```bash
mkdir -p build-release && cd build-release
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON
cmake --build .
```

### Channel Flow Coverage

Channel CJM coverage (ingress parsing/filtering, session key routing, account propagation, bus handoff) is validated by tests in:

- `src/channel_manager.c` (runtime channel registration/start semantics + listener mode wiring)
- `src/config.c` (OpenClaw-compatible `channels.*.accounts` parsing, multi-account selection/ordering, aliases)
- `src/gateway/gateway.c` (Telegram/WhatsApp/LINE/Lark routed session keys from webhook payloads)
- `src/daemon.c` (gateway-loop inbound route resolution for Discord/QQ/OneBot/Mattermost/MaixCam)
- `src/channels/discord.c`, `src/channels/mattermost.c`, `src/channels/qq.c`, `src/channels/onebot.c`, `src/channels/signal.c`, `src/channels/line.c`, `src/channels/whatsapp.c` (per-channel inbound/outbound contracts)

### Project Stats

```

Language: C11 + ASM (aarch64, x86_64)
Source files: 432
Lines of code: ~64,000
Test files: 71
Tests: 1,946
Binary: 282 KB core / 380 KB full (MinSizeRel + LTO)
Peak RSS: < 5 MB
Startup: <2 ms (Apple Silicon)
Dependencies: libc + optional SQLite, libcurl

```

### Source Layout

```

src/
main.c CLI entry point + command routing
agent/ Agent loop, context, planner, compaction, dispatcher
channels/ 20 channel implementations (cli, telegram, discord, ...)
providers/ 50+ AI provider implementations
memory/ SQLite + markdown + LRU backends, embeddings, vector search
tools/ 47 tool implementations
security/ Policy, pairing, secrets, sandbox backends
runtime/ Runtime adapters (native, docker, wasm, cloudflare)
core/ Allocator, arena, error, json, http, string, slice
observability/ Log + metrics observers
gateway/ HTTP gateway server
config.c Config loading/merging (~/.seaclaw/config.json)
...

include/seaclaw/ Public C headers
tests/ 71 test files, 1,946 tests
asm/ Platform-specific assembly (aarch64, x86_64, generic C)

```

## Versioning

seaclaw uses **CalVer** (`YYYY.M.D`) for releases — e.g. `v2026.2.20`.

- **Tag format:** `vYYYY.M.D` (one release per day max; patch suffix `vYYYY.M.D.N` if needed)
- **No stability guarantees yet** — the project is pre-1.0, config and CLI may change between releases
- **`seaclaw --version`** prints the current version

## Contributing

Implement a vtable interface, submit a PR:

- New `Provider` -> `src/providers/`
- New `Channel` -> `src/channels/`
- New `Tool` -> `src/tools/`
- New `Memory` backend -> `src/memory/`
- New `Tunnel` -> `src/tunnel/`
- New `Sandbox` backend -> `src/security/`
- New `Peripheral` -> `src/peripherals/`
- New `Skill` -> `~/.seaclaw/workspace/skills/<name>/`

## Disclaimer

seaclaw is a pure open-source software project. It has **no token, no cryptocurrency, no blockchain component, and no financial instrument** of any kind. This project is not affiliated with any token or financial product.

## License

MIT — see [LICENSE](LICENSE)

---

**seaclaw** — Bring AI to every device on Earth.

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=sethdford/seaclaw&type=date&legend=top-left)](https://www.star-history.com/#sethdford/seaclaw&type=date&legend=top-left)

```

```
