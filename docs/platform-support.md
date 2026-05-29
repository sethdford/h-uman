---
title: Platform Support & Feature Flags
last_updated: 2026-03-16
---

# Platform Support & Feature Flags

This document catalogs all compile-time feature flags, platform-specific guards,
and test-mode behavior in the h-uman codebase. Use this to understand which
features are available on each platform and what happens when a feature is disabled.

## Build Presets

| Preset    | Build Type   | ASan | SQLite | Channels | Persona | Skills | Notes                        |
| --------- | ------------ | ---- | ------ | -------- | ------- | ------ | ---------------------------- |
| `dev`     | Debug        | ON   | ON     | All      | ON      | ON     | Development (recommended)    |
| `test`    | Debug        | OFF  | ON     | All      | ON      | ON     | Fast test iteration          |
| `release` | MinSizeRel   | OFF  | ON     | All      | ON      | ON     | LTO enabled                  |
| `fuzz`    | Debug        | OFF  | ON     | All      | ON      | ON     | Clang + libFuzzer            |
| `minimal` | MinSizeRel   | OFF  | OFF    | CLI only | OFF     | OFF    | Smallest binary              |

## Core Feature Flags (CMake Options)

### Storage & Networking

| Flag                  | Default | Gated Features                                              | Non-enabled Behavior               |
| --------------------- | ------- | ----------------------------------------------------------- | ---------------------------------- |
| `HU_ENABLE_SQLITE`    | ON      | SQLite memory, graph, feeds, skills, persona mood, eval     | Stubs return `HU_ERR_NOT_SUPPORTED`|
| `HU_ENABLE_POSTGRES`  | OFF     | PostgreSQL memory backend                                   | `HU_ERR_NOT_SUPPORTED`             |
| `HU_ENABLE_PGVECTOR`  | OFF     | pgvector store (requires libpq)                             | Stub vtable, all ops fail          |
| `HU_ENABLE_CURL`      | ON      | libcurl HTTP client                                         | HTTP ops return `HU_ERR_NOT_SUPPORTED` |
| `HU_ENABLE_TLS`       | ON      | OpenSSL for WSS/HTTPS, push notifications                   | Push/WSS stubs                     |

### Module Groups

| Flag                       | Default | Gated Features                                  | Non-enabled Behavior     |
| -------------------------- | ------- | ----------------------------------------------- | ------------------------ |
| `HU_ENABLE_PERSONA`        | ON      | Persona system (profiles, overlays, examples)    | Code paths omitted       |
| `HU_ENABLE_SKILLS`         | OFF     | Skill registry, continuous learning              | stderr message + error   |
| `HU_ENABLE_FEEDS`          | OFF     | Feed ingestion, research agent, social feeds     | Empty translation units  |
| `HU_ENABLE_ML`             | OFF     | BPE, dataloader, experiment loop, agent trainer  | Code paths omitted       |
| `HU_ENABLE_PERIPHERALS`    | OFF     | Arduino, STM32, RPi, SPI, I2C                   | Tools not registered     |
| `HU_ENABLE_TUNNELS`        | OFF     | ngrok, Cloudflare, Tailscale tunnels             | Providers not registered |
| `HU_ENABLE_RUNTIME_EXOTIC` | OFF     | WASM, Cloudflare Workers runtimes                | `HU_ERR_NOT_SUPPORTED`   |
| `HU_ENABLE_TOOLS_BROWSER`  | OFF     | Browser, screenshot, browser_open tools          | Tools not registered     |
| `HU_ENABLE_TOOLS_ADVANCED` | OFF     | Composio, Claude Code, canvas, notebook, DB      | Tools not registered     |
| `HU_ENABLE_CRON`           | OFF     | Cron scheduler                                   | Code paths omitted       |
| `HU_ENABLE_PUSH`           | OFF     | FCM/APNS push notifications                      | Code paths omitted       |
| `HU_ENABLE_UPDATE`         | OFF     | Update check/apply                               | `HU_ERR_NOT_SUPPORTED`   |
| `HU_ENABLE_OTEL`           | OFF     | OpenTelemetry exporter                           | Code paths omitted       |
| `HU_ENABLE_AUTHENTIC`      | OFF     | Authentic existence features                     | Code paths omitted       |
| `HU_ENABLE_CARTESIA`       | OFF     | Cartesia TTS for voice                           | `HU_ERR_NOT_SUPPORTED`   |
| `HU_ENABLE_EMBEDDED_MODEL` | OFF     | llama.cpp/llama-cli provider                     | `HU_ERR_NOT_SUPPORTED`   |

### Build Options

| Flag               | Default | Purpose                              |
| ------------------ | ------- | ------------------------------------ |
| `HU_ENABLE_ASAN`   | OFF     | AddressSanitizer (dev/test builds)   |
| `HU_ENABLE_LTO`    | OFF     | Link-Time Optimization (release)     |
| `HU_ENABLE_FUZZ`   | OFF     | libFuzzer harness compilation        |
| `HU_ENABLE_BENCH`  | OFF     | Benchmark binary (`human_bench`)     |
| `HU_ENABLE_READLINE` | OFF   | readline/libedit for CLI editing     |
| `HU_ENABLE_LINENOISE`| OFF   | linenoise for lightweight CLI        |
| `HU_ENABLE_TUI`    | OFF     | Rich TUI (termbox2)                  |

### Channel Flags

Each channel has its own `HU_ENABLE_<CHANNEL>` flag (e.g.,
`HU_ENABLE_TELEGRAM`, `HU_ENABLE_DISCORD`). CLI is always ON. Others default
to OFF unless `HU_ENABLE_ALL_CHANNELS=ON` is set.

`HU_ENABLE_ALL_CHANNELS=ON` enables all channels appropriate for the current
platform. Apple-only channels (iMessage, PWA) require `__APPLE__`.

### Memory Engine Flags

| Flag                        | Default | Backend              | Non-enabled Behavior     |
| --------------------------- | ------- | -------------------- | ------------------------ |
| `HU_ENABLE_NONE_ENGINE`     | ON      | No-op                | Always built             |
| `HU_ENABLE_MARKDOWN_ENGINE` | ON      | Markdown files        | Always built             |
| `HU_ENABLE_MEMORY_LRU_ENGINE`| ON    | In-memory LRU        | Not registered           |
| `HU_ENABLE_LUCID_ENGINE`    | ON      | Lucid (needs SQLite)  | `HU_ERR_NOT_SUPPORTED`   |
| `HU_ENABLE_LANCEDB_ENGINE`  | ON      | LanceDB (needs SQLite)| `HU_ERR_NOT_SUPPORTED`   |
| `HU_ENABLE_REDIS_ENGINE`    | OFF     | Redis                 | `HU_ERR_NOT_SUPPORTED`   |
| `HU_ENABLE_API_ENGINE`      | OFF     | HTTP API              | Not registered           |

## Platform-Specific Guards

### macOS (`__APPLE__ && __MACH__`)

| Feature            | Implementation                         | Other Platforms            |
| ------------------ | -------------------------------------- | -------------------------- |
| iMessage channel   | ChatDB bridge (`~/Library/Messages/`)  | Not available              |
| PWA bridge         | `osascript` browser detection          | `HU_ERR_NOT_SUPPORTED`     |
| Apple feeds        | Photos, reminders, health              | `HU_ERR_NOT_SUPPORTED`     |
| Calendar           | EventKit                               | Not available              |
| Seatbelt sandbox   | macOS sandbox profiles                 | Not available              |
| Screenshot         | `osascript`                            | Not available              |
| GUI agent          | AppleScript automation                 | Not available              |
| Persona CLI        | JXA integration                        | Not available              |

### Linux (`__linux__`)

| Feature            | Implementation                         | Other Platforms            |
| ------------------ | -------------------------------------- | -------------------------- |
| I2C peripheral     | Linux I2C subsystem                    | Not available              |
| RPi peripheral     | Raspberry Pi GPIO                      | Not available              |
| Firejail sandbox   | firejail                               | Not available              |
| Seccomp sandbox    | seccomp-BPF                            | Not available              |
| Firecracker        | Firecracker VM                         | Not available              |
| Landlock           | Landlock LSM (kernel ≥ 5.13)           | Not available              |
| MaixCam            | MaixCam channel                        | Not available              |

### Windows (`_WIN32`)

| Feature            | Implementation                         | Other Platforms            |
| ------------------ | -------------------------------------- | -------------------------- |
| Plugin loader      | `LoadLibrary` / `GetProcAddress`       | `dlopen` / `dlsym`        |
| Bus signals        | `CreateEvent` / `WaitForSingleObject`  | POSIX signals              |
| AppContainer       | Windows AppContainer sandbox           | Not available              |
| Credential vault   | Windows Credential Store               | Not available              |
| Path security      | Windows-specific path sanitization     | POSIX paths                |

### POSIX (`HU_GATEWAY_POSIX`)

Set when `UNIX AND NOT WIN32`. Required for:

- Gateway socket server (`bind`/`listen`/`accept`/`poll`)
- Process spawning (`fork`/`exec`)
- MCP stdio transport
- Agent dispatcher (parallel execution)
- Shell/git tools
- Several CLI providers (Claude CLI, Codex CLI)

When not set, the gateway returns `HU_ERR_NOT_SUPPORTED` for WebSocket operations.

## Test-Mode Behavior (`HU_IS_TEST`)

When the test binary runs, `HU_IS_TEST` is defined. This provides
deterministic test behavior without real side effects:

| Category         | Test-mode Behavior                                    |
| ---------------- | ----------------------------------------------------- |
| HTTP client      | Returns mock JSON; no real network                    |
| Process spawning | Stub paths; no fork/exec                              |
| Service daemon   | Skips systemd/launchd integration                     |
| MCP transport    | Mock send; no real transport                          |
| Plugin loader    | Mock dlopen                                           |
| Channels         | Mock HTTP for Discord, Slack, Twilio, etc.            |
| Tools            | Shell, PDF, browser, web search use mocks             |
| Skill registry   | Mock data; no network or filesystem                   |
| Embeddings       | Voyage, Ollama providers use mocks                    |
| Auth/OAuth       | Mock device flow                                      |
| Security         | Landlock, Docker, Firecracker, AppContainer use mocks |
| Memory engines   | Redis, Postgres, Lucid, LanceDB use in-memory mocks  |
| WebSocket        | Returns `HU_ERR_NOT_SUPPORTED`                        |
| PWA bridge       | Mock browser detection                                |
| Cost tracking    | Mock pricing                                          |

## Non-enabled Behavior Patterns

| Pattern              | When                                              | Behavior                             |
| -------------------- | ------------------------------------------------- | ------------------------------------ |
| Stub vtable          | SQLite FTS, Lucid, pgvector when feature disabled | All ops return `HU_ERR_NOT_SUPPORTED`|
| Early return         | `hu_sqlite_memory_get_db() == NULL`               | `HU_ERR_NOT_SUPPORTED`               |
| Omitted registration | Channel/provider/tool not compiled                | Not registered; no runtime path      |
| stderr message       | Skills, update when not built                     | `fprintf(stderr, ...)` + error code  |
| Empty TU             | Apple feeds, social feeds when not enabled        | `typedef` to avoid empty TU warning  |
