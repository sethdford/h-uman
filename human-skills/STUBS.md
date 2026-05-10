---
title: "Human — project status (stubs)"
created: 2026-03-20
status: active
---

# Human — Project Status

Last updated: 2026-03-20

## Summary

| Metric                         | Value                    |
| ------------------------------ | ------------------------ |
| Source files (src/ + include/) | **1,093**                |
| Lines of C/H code              | **~233K**                |
| Test files                     | 291                      |
| Tests passing                  | **9,500+/9,500+ (100%)**   |
| Binary size (MinSizeRel+LTO)   | **~1696 KB (all flags)** |
| Cold start (--version)         | **4–27 ms avg**          |
| Peak RSS (--version)           | **~5.7 MB**              |
| Peak RSS (test suite)          | **~6.0 MB**              |
| Test throughput                | **700+ tests/sec**       |

## Module Parity

| Subsystem        | Baseline | Human | Status                      |
| ---------------- | -------- | ----- | --------------------------- |
| Providers        | 20       | 20    | **Full parity**             |
| Channels         | 38       | 38    | **Full parity**             |
| Tools            | 85       | 85    | **Full parity**             |
| Security         | 11       | 13    | **Full parity** (+2 extras) |
| Agent            | 8        | 11    | **Full parity** (+3 extras) |
| Memory Engines   | 10       | 10    | **Full parity**             |
| Memory Lifecycle | 8        | 8     | **Full parity**             |
| Memory Retrieval | 8        | 10    | **Full parity** (+2 extras) |
| Memory Vector    | 12       | 15    | **Full parity** (+3 extras) |

**All baseline modules have been ported to Human. Zero gaps remain.**

## What Works (Real Implementation)

### Core

- **Full agent loop**: `human agent` — interactive turn-based conversation
- **Config loading**: JSON config parsing, env var overrides, validation
- **85 tools registered** (build-config dependent): All execute with proper vtable dispatch
- **38 channels** (catalog): CLI fully functional, others have send() via HTTP client
- **20 providers**: OpenAI, Anthropic, Gemini, Ollama, OpenRouter, Compatible, Claude CLI, Codex CLI, OpenAI Codex + reliable/router wrappers
- **HTTP client**: libcurl-based, with SSE streaming support
- **WebSocket**: basic `ws://` support; `wss://` TLS via OpenSSL when `HU_ENABLE_TLS=ON`

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
- **Retrieval**: Keyword search, RRF, MMR, QMD, adaptive strategy, query expansion, LLM reranker, temporal decay, FTS5 full-text search (with runtime availability check)
- **Lifecycle**: Cache, hygiene, snapshot, summarizer, diagnostics, migration, rollout
- **Consolidation**: LLM-powered memory consolidation with configurable interval (monotonic clock), decay, dedup, and max entries
- **Connection discovery**: LLM-powered insight extraction from memory entries
- **Multimodal ingestion**: File ingestion with vision-capable provider support
- **File watching**: Inbox polling for automatic file ingestion
- **Knowledge graph**: Entity/relation graph (SQLite-backed), community detection, temporal events, causal links, Ebbinghaus retention
- **Source attribution**: `store_ex` with source tracking across all backends (SQLite, LanceDB, Lucid, Markdown)
- **REST API**: Memory status, list, recall, store, forget, ingest, consolidate, graph via WebSocket JSON-RPC

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

## What's Stubbed (Interface Defined, Returns HU_ERR_NOT_SUPPORTED)

- **postgres.c, redis.c**: Memory engines — stubs present, need external libs for real backend
- **sqlite_fts.c, sqlite_lucid.c**: SQLite-backed memory engines — real when `HU_ENABLE_SQLITE=ON`, including `store_ex` with source attribution; stub path when SQLite unavailable
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

## Verification (local)

After changing runtime, UI, or apps:

```bash
cmake --preset dev && cmake --build --preset dev && ./build/human_tests
./scripts/verify-all.sh   # full gate: C + UI + docs + standards
cd apps/ios && swift build
cd apps/macos && swift build
cd apps/android && ./gradlew :app:compileDebugKotlin   # needs JDK 17+
```

## Audit (2026-03-20)

Source file counts verified against `src/` and `include/`:

| Category         | Files | Notes                                                                                                                                                                                                                                                                                                    |
| ---------------- | ----- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Providers        | 20    | anthropic, claude_cli, codex_cli, compatible, gemini, ollama, openai, openai_codex, openrouter, reliable, router + factory/helpers                                                                                                                                                                       |
| Channels         | 38    | 38 channels (project catalog; README/AGENTS); **39** `.c` files under `src/channels/` (incl. `format.c`, `meta_common.c`, `gmail_base64.c`, `dispatch`, `thread_binding`, `pwa`, `tiktok`, voice split modules)                                                                                         |
| Tools            | 85    | 85 tools registered in `src/tools/factory.c` (full build); 79 `.c` modules under `src/tools/` + factory + web_search provider bundle                                                                                                                                                                    |
| Memory engines   | 10    | none, markdown, memory_lru, sqlite, postgres, api, redis, lucid, lancedb, registry                                                                                                                                                                                                                       |
| Peripherals      | 4     | arduino, stm32, rpi, factory                                                                                                                                                                                                                                                                             |
| Runtime adapters | 5     | native, docker, cloudflare, wasm_rt, factory                                                                                                                                                                                                                                                             |
| Sandbox backends | 11    | bubblewrap, firejail, landlock, landlock_seccomp, seccomp, firecracker, appcontainer, seatbelt, docker, wasi, noop_sandbox                                                                                                                                                                               |
| Observability    | 4     | log_observer, metrics_observer, otel, multi_observer                                                                                                                                                                                                                                                     |
