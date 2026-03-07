# SeaClaw — Project Status

Last updated: 2026-03-06

## Summary

| Metric                         | Value                  |
| ------------------------------ | ---------------------- |
| Source files (src/ + include/) | **588**                |
| Lines of C/H/ASM code          | **~101K**              |
| Test files                     | 92                     |
| Tests passing                  | **2,910/2,910 (100%)** |
| Binary size (MinSizeRel+LTO)   | **~530 KB**            |
| Peak RSS (test suite)          | **~5.9 MB**            |

## Channels — Honest Status

### Full send + receive (21 channels)

| Channel     | send()         | listen()                   | Config Required                       |
| ----------- | -------------- | -------------------------- | ------------------------------------- |
| CLI         | stdout         | readline                   | None                                  |
| Telegram    | HTTP API       | Long-poll (getUpdates)     | `token`                               |
| Discord     | HTTP API       | GET /messages poll         | `token`, `channel_ids`                |
| Slack       | HTTP API       | conversations.history poll | `token`, `channel_ids`                |
| WhatsApp    | Graph API      | Webhook + poll queue       | `phone_number_id`, `token`            |
| Email       | curl SMTP      | curl IMAP poll             | SMTP/IMAP config                      |
| iMessage    | AppleScript    | chat.db SQLite poll        | `default_target` (macOS only)         |
| Signal      | signal-cli RPC | signal-cli poll            | `http_url`, `account`                 |
| Matrix      | HTTP PUT       | /sync long-poll            | `homeserver`, `access_token`          |
| IRC         | TCP socket     | select()+recv() on socket  | `server`, `port`                      |
| LINE        | Push API       | Webhook + poll queue       | `channel_token`                       |
| Lark        | HTTP API       | Webhook + poll queue       | `app_id`, `app_secret`                |
| Mattermost  | HTTP API       | REST /posts poll           | `url`, `token`                        |
| OneBot      | HTTP API       | Webhook + poll queue       | `api_base`, `access_token`            |
| DingTalk    | HTTP POST      | Webhook + poll queue       | `app_key`, `app_secret`               |
| Nostr       | nak CLI        | nak sub relay poll         | `nak_path`, `relay_url`, `seckey_hex` |
| QQ          | HTTP API       | Webhook + poll queue       | `app_id`, `bot_token`                 |
| Teams       | HTTP API       | Webhook + poll queue       | `app_id`, `app_secret`                |
| Twilio SMS  | HTTP API       | Webhook + poll queue       | `account_sid`, `auth_token`           |
| Google Chat | HTTP API       | Webhook + poll queue       | `bearer_token`, `space_id`            |

### Receive only (1 channel)

| Channel | send()        | listen()       | Config Required                               |
| ------- | ------------- | -------------- | --------------------------------------------- |
| Gmail   | Not supported | Gmail API poll | `client_id`, `client_secret`, `refresh_token` |

### Send only (2 channels)

| Channel | send()         | listen()            | Config Required         |
| ------- | -------------- | ------------------- | ----------------------- |
| MaixCam | Serial (Linux) | Send-only by design | `host` (serial path)    |
| Web     | In-memory      | Gateway events      | `auth_token` (optional) |

### Special

| Channel  | Purpose                  |
| -------- | ------------------------ |
| Dispatch | Forwards to sub-channels |

## Tools — All 73 Real (with all feature flags)

Every tool has a real implementation. In test mode (`SC_IS_TEST`), they return mock
data to avoid side effects. Highlights:

- **File I/O**: read, write, edit, append, diff, apply_patch
- **Shell/Process**: shell, spawn, claude_code, delegate
- **Web**: web_search (Brave/DuckDuckGo/Tavily), web_fetch, http_request
- **Browser**: open, read (HTTP), CDP automation (click/type/scroll — needs Chrome)
- **Memory**: store, recall, list, forget (backed by SQLite/LRU/Markdown)
- **Cron**: add, list, remove, run, runs, update
- **Hardware**: i2c, spi (Linux only), hardware_info, hardware_memory
- **PDF**: extract text and metadata from PDF files (pdftotext or fallback)
- **Business**: spreadsheet (CSV/TSV parse/analyze/query/generate), report (Markdown/HTML structured reports), broadcast (multi-channel send), calendar (Google Calendar CRUD), jira (issue CRUD via REST API), social (Twitter/LinkedIn post/read/analytics), crm (HubSpot contacts/deals), analytics (Plausible/GA metrics), invoice (create/parse/summarize), workflow (DAG steps with approval gates)
- **Other**: database (SQLite), notebook, canvas, schema, pushover, composio, message, image, screenshot, agent*query, agent_spawn, mcp*\*

## Providers — All Real

9 core providers + 90+ compatible service mappings. All make real HTTP API calls:

| Provider     | Streaming    | Env Var                           |
| ------------ | ------------ | --------------------------------- |
| openai       | SSE + **WS** | `OPENAI_API_KEY`                  |
| anthropic    | Yes (SSE)    | `ANTHROPIC_API_KEY`               |
| gemini       | Yes (SSE)    | `GEMINI_API_KEY`                  |
| ollama       | Yes (SSE)    | None (local)                      |
| openrouter   | Yes (SSE)    | `OPENROUTER_API_KEY`              |
| compatible   | Yes (SSE)    | Per-provider or `SEACLAW_API_KEY` |
| claude_cli   | No           | `ANTHROPIC_API_KEY`               |
| codex_cli    | No           | `OPENAI_API_KEY`                  |
| openai-codex | No           | API key from config               |

OpenAI provider supports optional WebSocket streaming (`ws_streaming: true` in config)
with automatic SSE fallback. Compatible covers: Groq, Mistral, DeepSeek, xAI, Cerebras,
Perplexity, Cohere, Together, Fireworks, HuggingFace, LMStudio, and 80+ more.

## MCP Client — Real (stdio transport)

The MCP (Model Context Protocol) client is **fully implemented**:

- JSON-RPC 2.0 over stdio pipes (fork/exec child process)
- `initialize` + `notifications/initialized` handshake
- `tools/list` — discovers tools from MCP servers
- `tools/call` — executes MCP tools with arguments
- Auto-wraps MCP server tools as `sc_tool_t` via `sc_mcp_init_tools()`
- MCP Host server mode (`sc_mcp_host_t`) for exposing SeaClaw tools
- Config: `mcp_servers` array in config.json

## Voice — Real (STT + TTS)

Voice I/O is **fully implemented**:

- **STT**: Groq Whisper (`whisper-large-v3`) by default, configurable endpoint
- **TTS**: OpenAI `tts-1` with voice `alloy` by default, configurable endpoint
- `sc_voice_stt()` — transcribe audio buffer
- `sc_voice_stt_file()` — transcribe from file path
- `sc_voice_tts()` — generate speech audio
- `sc_voice_play()` — play audio (`afplay` on macOS, `paplay`/`aplay` on Linux)
- Configurable API key, model, voice, language

## Sub-Agent System — Real (pthread pool)

Sub-agent spawning is **fully implemented**:

- `sc_agent_pool_create()` — thread pool with configurable max concurrency
- `sc_agent_pool_spawn()` — spawn one-shot or persistent agents
- `sc_agent_pool_query()` — send messages to persistent agents
- `sc_agent_pool_cancel()` / `sc_agent_pool_status()` / `sc_agent_pool_result()`
- `sc_agent_pool_list()` — enumerate all agents in pool
- 64-slot pool with `pthread_mutex_t` synchronization
- `agent_spawn` and `agent_query` tools wired through tool factory

## Memory & Embeddings — Real

- **SQLite** (primary): FTS5 full-text search + vector cosine similarity
- **Markdown** (lightweight): file-backed plain text
- **LRU** (in-memory): bounded-size cache
- **Embedding providers**: Gemini, **Ollama** (local), Voyage
- **Vector stores**: in-memory, pgvector (optional), qdrant (optional)
- **Retrieval engine**: hybrid FTS5 + vector + semantic cache

## Gateway — 27+ Methods + K8s Health

All real (backed by app context):

| Endpoint    | Status | Notes                                    |
| ----------- | ------ | ---------------------------------------- |
| /health     | Real   | Liveness — always 200 OK                 |
| /healthz    | Real   | K8s alias for /health                    |
| /ready      | Real   | Readiness — checks registered components |
| /readyz     | Real   | K8s alias for /ready                     |
| /api/status | Real   | WebSocket connections + status           |

WebSocket control protocol: connect, health, config.get/set/schema, capabilities,
chat.send/history/abort, sessions.list/patch/delete, tools.catalog, channels.status,
cron._, skills._, models.list, usage.summary, push.\*, update.check/run, exec.

## Config Hot-Reload — Real (SIGHUP)

- `kill -HUP <pid>` triggers config reload without gateway restart
- Atomic flag (`stdatomic.h`) for signal-safe communication
- On success: old config freed, new config applied
- On failure: current config preserved, error logged
- Per-channel `suppress_tool_progress` config option

## Memory Engines — All Real (Build-Flag Gated)

| Engine     | Build Flag                      | Status | Notes                                 |
| ---------- | ------------------------------- | ------ | ------------------------------------- |
| SQLite     | `SC_ENABLE_SQLITE` (ON)         | Real   | FTS5 + vector cosine; primary engine  |
| PostgreSQL | `SC_ENABLE_POSTGRES`            | Real   | Full libpq implementation behind flag |
| Redis      | `SC_ENABLE_REDIS_ENGINE`        | Real   | Raw TCP + RESP protocol, no hiredis   |
| LanceDB    | `SC_ENABLE_LANCEDB_ENGINE` (ON) | Real   | SQLite-backed text search engine      |
| pgvector   | `SC_ENABLE_POSTGRES`            | Real   | Vector store via libpq + pgvector ext |

## What Remains Stubbed

Nothing. All subsystems have real implementations, gated by build flags where external dependencies are required.

## Web UI Dashboard

14 views, all connected to the gateway via WebSocket:

| View     | Data Source                              | Interactive?                       |
| -------- | ---------------------------------------- | ---------------------------------- |
| Overview | health, capabilities, channels, sessions | Refresh                            |
| Chat     | chat.send/history, events                | Full: type, send, streaming, abort |
| Sessions | sessions.list/patch/delete, chat.history | Select, rename, delete, resume     |
| Agents   | config.get, sessions.list, capabilities  | Navigate to config                 |
| Models   | models.list, config.get                  | Display only                       |
| Voice    | chat.send + browser STT                  | Mic button, speech-to-text         |
| Tools    | tools.catalog                            | Search, expand params              |
| Channels | channels.status                          | Display only                       |
| Skills   | skills.list/install/enable/disable       | Install, toggle                    |
| Cron     | cron.list/add/remove/run                 | Add, run, delete jobs              |
| Config   | config.get/set/schema                    | Edit fields, raw JSON, save        |
| Nodes    | nodes.list, health                       | Display only                       |
| Usage    | usage.summary                            | Display only                       |
| Logs     | WebSocket events                         | Filter, clear                      |

## External Dependencies

| Dependency         | Status         | Required For                      |
| ------------------ | -------------- | --------------------------------- |
| libc               | Linked         | Everything                        |
| SQLite3 (vendored) | Linked         | Memory engine                     |
| libcurl            | Linked         | HTTP client, provider API calls   |
| pthread            | Linked         | Agent dispatcher, sub-agent pool  |
| libpq              | Optional (OFF) | PostgreSQL memory engine          |
| OpenSSL            | Optional       | WSS, TLS for WebSocket            |
| math (-lm)         | Linked         | Vector math, retrieval algorithms |
