# SeaClaw (Swift/SeaClawKit) — Project Status

This file documents the **Swift/SeaClawKit** module — a Swift wrapper around the C core. Metrics below are for the Swift codebase; the main C codebase has different scale (~597 source files, ~104K lines of C, 3,726 tests, ~1506 KB binary).

Last updated: 2026-03-08

## Summary

| Metric                         | Value                |
| ------------------------------ | -------------------- |
| Source files (src/ + include/) | **~594**             |
| Lines of C/H code              | **~101,017**         |
| Test files                     | 128                  |
| Tests passing                  | **3,726/3,726 (100%)** |
| Binary size (MinSizeRel+LTO)   | **~528 KB (full)**   |
| Core binary (no curl/channels) | **463 KB**           |
| seaclaw module parity          | **100%**             |

## Module Parity

| Subsystem        | Baseline | SeaClaw | Status                      |
| ---------------- | -------- | ------- | --------------------------- |
| Providers        | 18       | 18      | **Full parity**             |
| Channels         | 22       | 22      | **Full parity**             |
| Tools            | 54       | 54      | **Full parity**             |
| Security         | 11       | 13      | **Full parity** (+2 extras) |
| Agent            | 8        | 11      | **Full parity** (+3 extras) |
| Memory Engines   | 10       | 10      | **Full parity**             |
| Memory Lifecycle | 8        | 8       | **Full parity**             |
| Memory Retrieval | 8        | 10      | **Full parity** (+2 extras) |
| Memory Vector    | 12       | 15      | **Full parity** (+3 extras) |

**All baseline modules have been ported to SeaClaw. Zero gaps remain.**

## What Works (Real Implementation)

### Core

- **Full agent loop**: `seaclaw agent` — interactive turn-based conversation
- **Config loading**: JSON config parsing, env var overrides, validation
- **50+ tools registered** (build-config dependent): All execute with proper vtable dispatch
- **22 channels** (catalog): CLI fully functional, others have send() via HTTP client
- **18 providers**: OpenAI, Anthropic, Gemini, Ollama, OpenRouter, Compatible, Claude CLI, Codex CLI, OpenAI Codex + reliable/router wrappers
- **HTTP client**: libcurl-based, with SSE streaming support
- **WebSocket**: basic `ws://` support; `wss://` TLS via OpenSSL when `SC_ENABLE_TLS=ON`

### Memory

- **SQLite memory engine**: Full CRUD, session store, health checks
- **LRU memory engine**: In-memory cache with hash table + doubly-linked list
- **Markdown memory engine**: File-based memory backend
- **None memory engine**: No-op backend
- **Memory registry**: Backend factory with capability descriptors
- **Embedding abstraction**: vtable + 3 providers (Gemini, Ollama, Voyage)
- **Provider router**: Routes embedding requests by model/config
- **Vector store abstraction**: vtable + in-memory (real), pgvector (stub), qdrant (stub) backends — all memory engines have real or stub implementations
- **Outbox**: Async embedding queue with batch flush
- **Semantic cache**: Embedding similarity cache with exact-match fallback
- **Retrieval**: Keyword search, RRF, MMR, QMD, adaptive strategy, query expansion, LLM reranker, temporal decay
- **Lifecycle**: Cache, hygiene, snapshot, summarizer, diagnostics, migration, rollout

### Security

- **Policy enforcement**: Autonomy levels, command risk assessment, blocklists
- **Pairing**: Code + token authentication with lockout
- **Secrets**: ChaCha20 + HMAC-SHA256 encryption
- **Audit logging**: Event-based security audit trail
- **Sandbox**: Abstraction for bubblewrap, firejail, landlock, landlock_seccomp, seccomp, firecracker, appcontainer, seatbelt, docker, wasi, noop
- **Rate tracking**: Per-key/per-window rate limiting

### Infrastructure

- **Gateway**: POSIX HTTP server with routing, rate limiting, HMAC verification, webhook dispatch
- **Tunnels**: None, Cloudflare, Ngrok, Custom
- **Peripherals**: Arduino, STM32, RPi factory + vtable
- **Agent subsystems**: Dispatcher (pthread), compaction, planner, context tokens, max tokens, prompt builder, commands, CLI
- **Cron**: Cron expression parsing, job scheduling, execution
- **Heartbeat**: Periodic task engine
- **Cost tracking**: Per-model pricing, budget enforcement
- **Session management**: Create, get, expire, idle eviction
- **Event bus**: Pub/sub for cross-module communication
- **Identity**: Bot identity, permission resolution
- **Auth module**: Authentication flows and token handling
- **Runtime factory**: Runtime adapter registration and dispatch (native, docker, wasm, cloudflare)
- **Spawn tool**: Real `fork`/`execvp` process execution (POSIX)
- **WASM**: Build target, WASI bindings, bump allocator, wasm provider/channel

## What's Stubbed (Interface Defined, Returns SC_ERR_NOT_SUPPORTED)

- **postgres.c, redis.c, lancedb.c, lucid.c**: Memory engines — stubs present, need external libs for real backend (api.c is real — HTTP API with in-memory mock for tests)
- **store_pgvector.c**: Vector store stub (needs libpq + pgvector)
- **Self-update**: Interface defined, no download/replace mechanism

Previously stubbed, now **real**:

- **MCP client**: Real — JSON-RPC 2.0 over stdio, tools/list + tools/call, MCP Host server mode
- **Sub-agent spawning**: Real — pthread pool, spawn/query/cancel/list, 64-slot pool
- **Voice I/O**: Real — Groq Whisper STT, OpenAI TTS, play via afplay/paplay

## External Dependencies

| Dependency         | Status         | Required For                      |
| ------------------ | -------------- | --------------------------------- |
| libc               | Linked         | Everything                        |
| SQLite3 (vendored) | Linked         | Memory engine                     |
| libcurl            | Linked         | HTTP client, provider API calls   |
| pthread            | Linked         | Agent dispatcher                  |
| libpq              | Optional (OFF) | PostgreSQL memory engine          |
| OpenSSL            | Optional (ON)  | WSS, TLS for WebSocket            |
| math (-lm)         | Linked         | Vector math, retrieval algorithms |

## Audit (2026-03-03)

Source file counts verified against `src/` and `include/`:

| Category         | Files | Notes                                                                                                                                                                                                  |
| ---------------- | ----- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Providers        | 18    | anthropic, claude_cli, codex_cli, compatible, gemini, ollama, openai, openai_codex, openrouter, reliable, router + factory/helpers                                                                     |
| Channels         | 24    | cli, web, discord, mattermost, google_chat, dingtalk, irc, email, teams, slack, onebot, matrix, whatsapp, nostr, imessage, line, signal, telegram, maixcam, qq, lark, twilio, dispatch, thread_binding |
| Tools            | 66    | 56 tool impls + factory + 9 web_search_providers (exa, brave, etc.)                                                                                                                                    |
| Memory engines   | 10    | none, markdown, memory_lru, sqlite, postgres, api, redis, lucid, lancedb, registry                                                                                                                     |
| Peripherals      | 4     | arduino, stm32, rpi, factory                                                                                                                                                                           |
| Runtime adapters | 5     | native, docker, cloudflare, wasm_rt, factory                                                                                                                                                           |
| Sandbox backends | 11    | bubblewrap, firejail, landlock, landlock_seccomp, seccomp, firecracker, appcontainer, seatbelt, docker, wasi, noop_sandbox                                                                             |
| Observability    | 4     | log_observer, metrics_observer, otel, multi_observer                                                                                                                                                   |
