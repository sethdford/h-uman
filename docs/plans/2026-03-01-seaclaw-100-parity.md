---
title: SeaClaw 100% Parity — Porting Plan (COMPLETED)
description: Historical plan for porting Zig modules to C, retained for reference
updated: 2026-03-02
---

# SeaClaw 100% Parity — Porting Plan (COMPLETED)

> **Status: COMPLETED** — All Zig modules have been ported to C. This document is retained as historical reference.

**Goal:** Port every remaining Zig module to C, reaching full feature parity.

**Architecture:** SeaClaw follows the original Zig reference's vtable-driven design. Every extension point (provider, channel, tool, memory engine, sandbox backend) is a struct of function pointers registered in a factory.

**Tech Stack:** C11, CMake, libcurl (HTTP), libsqlite3 (memory), pthreads (concurrency), ChaCha20-Poly1305 + SHA-256 (crypto via `asm/`). Platform deps: libpq (postgres), hiredis (redis). Test framework: custom `test_framework.h` with ASan.

**Final state:** 1,697 tests passing, 0 ASan errors. ~52,000 lines of C source + ~22,000 lines of tests. 239 KB MinSizeRel binary.

---

## Dependency Graph

```
Layer 0 (independent — all parallel)
├── WP-01: Compatible URL table
├── WP-02: Web search providers (5)
├── WP-03: Config schema parity
├── WP-04: Security sandbox backends (3)
└── WP-05: Cron engine + cron tools (6)

Layer 1 (depends on Layer 0)
├── WP-06: Memory tools are DONE (store/recall/list/forget already real)
├── WP-07: Tools Tier 2 — delegate, message, browser, image, screenshot
├── WP-08: Tools Tier 3 — composio, pushover, schedule, schema, hardware, i2c, spi, browser_open
├── WP-09: Channels Batch 1 — HTTP-based (7): Matrix, WhatsApp, Email, Mattermost, DingTalk, Lark, Line
└── WP-10: Channels Batch 2 — Protocol-based (7): IRC, Web, iMessage, OneBot, Nostr, MaixCam, Dispatch

Layer 2 (depends on Layer 1)
├── WP-11: MCP client (JSON-RPC over stdio)
├── WP-12: Subagent (pthread spawn + task mgmt)
├── WP-13: RAG pipeline
├── WP-14: Voice (Whisper STT/TTS)
└── WP-15: Multimodal (image encoding, vision)

Layer 3 (depends on Layer 2)
├── WP-16: Memory engines — Postgres, Redis, API
├── WP-17: Memory engines — Lucid, LanceDB
├── WP-18: Vector stores — pgvector, qdrant
├── WP-19: CLI commands (6 stubs)
└── WP-20: Cloudflare Workers runtime

Layer 4 (final)
├── WP-21: Test parity (~2700 additional tests)
├── WP-22: E2E validation + binary size + RSS
└── WP-23: Promote SeaClaw to repo root
```

---

## AUDIT CHECKPOINTS

After every layer, run a full audit cycle:

```bash
# Build
cd seaclaw/build && cmake .. -DCMAKE_C_FLAGS="-fsanitize=address -g" && cmake --build . -j$(nproc)

# Test
./seaclaw_tests   # must show 0 FAIL, 0 ASan errors

# Size check (release)
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel && cmake --build . -j$(nproc)
ls -lh seaclaw      # target: < 1 MB
```

Audit questions at each checkpoint:

1. What did we get wrong?
2. What did we miss?
3. What allocations aren't freed?
4. What error paths aren't tested?
5. What Zig behavior did we fail to replicate?

---

## LAYER 0 — Foundation (all parallel)

### WP-01: Compatible Provider URL Table

**Effort:** 30 min | **Zig ref:** `src/providers/factory.zig` L46–149 (~104 lines)

**Files:**

- Modify: `seaclaw/src/providers/factory.c`
- Modify: `seaclaw/include/seaclaw/provider.h` (add `sc_compatible_provider_url` declaration)
- Test: `seaclaw/tests/test_provider_all.c` (add `test_compatible_url_lookup_*`)

**What to do:**

1. Read `src/providers/factory.zig` lines 46–149 for the `compat_providers` table
2. Add a `static const struct { const char *name; const char *url; }` lookup table to `factory.c`
3. Add `const char *sc_compatible_provider_url(const char *name)` that searches the table
4. Wire it into `sc_compatible_create` so that if `base_url` is NULL, it looks up by provider name
5. Add tests for: groq, mistral, deepseek, together, fireworks, perplexity, cerebras (at least 7 providers)
6. Test unknown provider returns NULL
7. Build + test

**Audit:** Verify every Zig table entry has a C counterpart. Compare URL strings exactly.

---

### WP-02: Web Search Providers (5 missing)

**Effort:** 2 hours | **Zig ref:** 313 lines total

**Files to create:**

- `seaclaw/src/tools/web_search_providers/exa.c` (ref: `src/tools/web_search_providers/exa.zig`, 53 lines)
- `seaclaw/src/tools/web_search_providers/firecrawl.c` (ref: 58 lines)
- `seaclaw/src/tools/web_search_providers/perplexity.c` (ref: 53 lines)
- `seaclaw/src/tools/web_search_providers/jina.c` (ref: 88 lines)
- `seaclaw/src/tools/web_search_providers/searxng.c` (ref: 61 lines)

**Files to modify:**

- `seaclaw/src/tools/web_search.c` — register new providers in the dispatcher
- `seaclaw/CMakeLists.txt` — add new source files
- Test: `seaclaw/tests/test_tools_extended.c` (or new `test_web_search_providers.c`)

**Pattern:** Each provider implements `sc_web_search_provider_t` with `search(alloc, query, api_key, results, count)`. Read the existing `brave.c` / `tavily.c` / `duckduckgo.c` for the pattern, then port from Zig.

**What to do per provider:**

1. Read the Zig source to understand the HTTP endpoint, request format, response parsing
2. Create the `.c` file following the `brave.c` pattern
3. Register in the web_search dispatcher
4. Add a test that exercises the provider with `SC_IS_TEST` mock responses
5. Build + test

---

### WP-03: Config Schema Parity

**Effort:** 4 hours | **Zig ref:** `src/config_types.zig` (1,328 lines), `src/config.zig` (4,053 lines)

**Files:**

- Modify: `seaclaw/include/seaclaw/config.h` — expand `sc_config_t` with all missing fields
- Modify: `seaclaw/include/seaclaw/config_types.h` — add missing type definitions
- Modify: `seaclaw/src/config.c` — expand `sc_config_parse_json` to handle all new fields
- Test: `seaclaw/tests/test_config_extended.c` — add tests for new fields

**What to do:**

1. Read `src/config_types.zig` for all struct definitions (AudioMediaConfig, DockerRuntimeConfig, ReliabilityConfig, channel configs, DmScope, etc.)
2. Add C equivalents to `config_types.h`
3. Read `src/config.zig` for JSON parsing logic per field
4. Add parsing for each new field in `sc_config_parse_json`
5. Add defaults in `set_defaults()`
6. Test: parse a full `config.example.json` and verify all fields populated
7. Build + test

**Audit:** Diff every field in `sc_config_t` against `config_types.zig` structs. Every Zig field must have a C counterpart.

---

### WP-04: Security Sandbox Backends

**Effort:** 2 hours | **Zig ref:** firejail (135), bubblewrap (150), landlock (96) = 381 lines

**Files:**

- Modify: `seaclaw/src/security/firejail.c`
- Modify: `seaclaw/src/security/bubblewrap.c`
- Modify: `seaclaw/src/security/landlock.c`
- Test: `seaclaw/tests/test_security_extended.c`

**What to do:**

1. Read each Zig file for the sandbox interface: `spawn_sandboxed(argv, env, working_dir)`
2. Port to C: each backend constructs a command line and calls `sc_process_exec`
3. Firejail: `firejail --quiet --private=<dir> -- <cmd>`
4. Bubblewrap: `bwrap --ro-bind / / --bind <dir> <dir> -- <cmd>`
5. Landlock: Linux-only syscalls (`landlock_create_ruleset`, etc.)
6. All three must return `SC_ERR_NOT_SUPPORTED` on unsupported platforms
7. Add `builtin.is_test` / `SC_IS_TEST` guards — never actually spawn in tests
8. Test: verify command construction without execution
9. Build + test

---

### WP-05: Cron Engine + Cron Tools (6)

**Effort:** 3 hours | **Zig ref:** 6 tools × ~150 lines = ~920 lines + cron engine

**Files:**

- Create or modify: `seaclaw/src/cron.c`, `seaclaw/include/seaclaw/cron.h` (cron engine)
- Modify: `seaclaw/src/tools/cron_add.c` (43→~80 lines)
- Modify: `seaclaw/src/tools/cron_list.c`
- Modify: `seaclaw/src/tools/cron_remove.c`
- Modify: `seaclaw/src/tools/cron_run.c`
- Modify: `seaclaw/src/tools/cron_runs.c`
- Modify: `seaclaw/src/tools/cron_update.c`
- Modify: `seaclaw/CMakeLists.txt`
- Test: `seaclaw/tests/test_cron.c` (new)

**What to do:**

1. Read Zig cron engine (likely in `src/cron.zig` or embedded in main)
2. Implement `sc_cron_store_t` — CRUD for cron entries backed by SQLite or in-memory
3. Each cron tool: parse JSON args → call cron store → return result
4. `cron_add`: parse schedule expression + tool name + args → store
5. `cron_list`: list all entries as JSON
6. `cron_remove`: delete by ID
7. `cron_run`: execute a cron entry immediately
8. `cron_runs`: list past execution history
9. `cron_update`: modify schedule or args
10. Tests: CRUD lifecycle, invalid inputs, empty store
11. Build + test

---

## AUDIT 0: Layer 0 Checkpoint

Build + test. Verify all new code compiles clean with `-Wall -Werror -fsanitize=address`. All tests pass. Review each WP against Zig reference for missed behavior.

---

## LAYER 1 — Channels + Tools

### WP-07: Tools Tier 2 — Core Agent Tools

**Effort:** 4 hours | **Zig ref:** delegate (378), message (238 — already real), browser (324), image (400), screenshot (102) = ~1,200 lines

**Files:**

- Modify: `seaclaw/src/tools/delegate.c` (65→~150 lines)
- Modify: `seaclaw/src/tools/browser.c` (43→~120 lines)
- Modify: `seaclaw/src/tools/image.c` (76→~150 lines)
- Modify: `seaclaw/src/tools/screenshot.c` (62→~80 lines)
- Test: `seaclaw/tests/test_tools_extended.c`

**What to do:**

- `delegate`: Parse JSON args for `agent_id` + `task`. Call subagent spawn (or return error if subagent not available). Use `SC_IS_TEST` guard.
- `browser`: Parse URL arg. Use `sc_process_exec` to open browser (`open` on macOS, `xdg-open` on Linux). `SC_IS_TEST` guard.
- `image`: Parse `url` or `path` arg. Validate URL with `sc_validate_url`. Return image description placeholder (actual vision requires multimodal).
- `screenshot`: Validate URL. Return stub result (actual screenshot requires headless browser).
- Add tests for arg validation, error paths, test-mode success paths.

---

### WP-08: Tools Tier 3 — Peripheral + Utility Tools

**Effort:** 5 hours | **Zig ref:** composio (617), pushover (278), schedule (412), schema (702), hardware_info (196), hardware_memory (300), i2c (437), spi (399), browser_open (231) = ~3,572 lines

**Files to modify:** Each `seaclaw/src/tools/<name>.c`
**Tests:** `seaclaw/tests/test_tools_tier3.c` (new)

**What to do per tool:**

1. Read the Zig reference for JSON parameter schema and execution logic
2. Replace stub body with real logic
3. `composio`: HTTP POST to Composio API (needs API key from config)
4. `pushover`: HTTP POST to Pushover API
5. `schedule`: Parse cron expression, delegate to cron engine
6. `schema`: Return JSON schema for all tools (introspection)
7. `hardware_info`: Read `/proc/cpuinfo`, `/proc/meminfo` (Linux), `sysctl` (macOS)
8. `hardware_memory`: Memory stats via platform APIs
9. `i2c` / `spi`: Hardware I/O via file descriptors (`/dev/i2c-*`, `/dev/spidev*`). `SC_IS_TEST` guard.
10. `browser_open`: Like browser but opens a URL, not a full browser tool
11. All hardware tools return `SC_ERR_NOT_SUPPORTED` on unsupported platforms
12. Tests: parameter parsing, validation, test-mode paths

---

### WP-09: Channels Batch 1 — HTTP-Based (7)

**Effort:** 8 hours | **Zig ref:** Matrix (944), WhatsApp (978), Email (801), Mattermost (1,192), DingTalk (121), Lark (1,468), Line (977) = ~6,481 lines

**Files to modify:** Each `seaclaw/src/channels/<name>.c`
**Tests:** `seaclaw/tests/test_channels_batch1.c` (new)

**Priority order:** DingTalk (smallest, warmup) → WhatsApp → Email → Line → Matrix → Mattermost → Lark

**Pattern for each channel:**

1. Read Zig source for API endpoints, auth method, message format
2. Expand the context struct with API tokens, base URL, polling state
3. Implement `send`: HTTP POST to the platform's send message API
4. Implement `start`/`listen`: long-poll or webhook registration
5. Implement `stop`: cleanup polling thread
6. Implement `health_check`: ping the API
7. Wire config fields (from WP-03)
8. `SC_IS_TEST` guard: mock HTTP responses in test mode
9. Tests: config validation, send with mock, health check, error paths

**Channel-specific notes:**

- **DingTalk**: Simple webhook POST with HMAC-SHA256 signing
- **WhatsApp**: Meta Cloud API, webhook verification with challenge token
- **Email**: SMTP send via libcurl, IMAP poll for listen (or stub listen)
- **Line**: Messaging API, webhook for events, reply token pattern
- **Matrix**: Client-Server API, /sync long-poll, access token auth
- **Mattermost**: REST API + WebSocket for real-time events
- **Lark**: Most complex — Open API with tenant access token, event subscription

---

### WP-10: Channels Batch 2 — Protocol-Based (7)

**Effort:** 8 hours | **Zig ref:** IRC (1,059), Web (2,140), iMessage (915), OneBot (927), Nostr (1,635), MaixCam (722), Dispatch (801) = ~8,199 lines

**Files to modify:** Each `seaclaw/src/channels/<name>.c`
**Tests:** `seaclaw/tests/test_channels_batch2.c` (new)

**Priority order:** IRC → OneBot → Web → iMessage → Dispatch → MaixCam → Nostr

**Channel-specific notes:**

- **IRC**: Raw TCP socket, RFC 2812 protocol. NICK/USER/JOIN/PRIVMSG. Needs socket I/O.
- **OneBot**: WebSocket or HTTP, follows OneBot 11/12 spec
- **Web**: HTTP server (like gateway) serving a chat UI. Biggest channel.
- **iMessage**: macOS only, uses AppleScript or `imessage-exporter`. Return `SC_ERR_NOT_SUPPORTED` on non-macOS.
- **Dispatch**: Meta-channel that routes to other channels
- **MaixCam**: Serial/UART communication to MaixCAM device
- **Nostr**: WebSocket to relay, NIP-01 events, Schnorr signatures (secp256k1)

---

## AUDIT 1: Layer 1 Checkpoint

Full rebuild + ASan test. Verify:

- All 14 channels have real `send` implementations
- All tools have real `execute` implementations
- Test count should be ~800+
- No memory leaks

---

## LAYER 2 — Agent Advanced

### WP-11: MCP Client

**Effort:** 4 hours | **Zig ref:** `src/mcp.zig` (594 lines)

**Files:**

- Rewrite: `seaclaw/src/mcp.c` (19→~300 lines)
- Modify: `seaclaw/include/seaclaw/mcp.h`
- Test: `seaclaw/tests/test_mcp.c` (new)

**What to do:**

1. Read `src/mcp.zig` for JSON-RPC 2.0 over stdio protocol
2. Implement `sc_mcp_client_t` with:
   - `spawn(command, args)` → fork+exec, pipe stdin/stdout
   - `call(method, params)` → write JSON-RPC request, read response
   - `list_tools()` → call "tools/list" method
   - `call_tool(name, args)` → call "tools/call" method
3. Implement `sc_mcp_init_tools` → spawn MCP server, list tools, create tool wrappers
4. `SC_IS_TEST` guard: mock responses, no real process spawn
5. Tests: JSON-RPC serialization, mock tool listing, error handling

---

### WP-12: Subagent

**Effort:** 4 hours | **Zig ref:** `src/subagent.zig` (475 lines)

**Files:**

- Rewrite: `seaclaw/src/subagent.c` (30→~250 lines)
- Modify: `seaclaw/include/seaclaw/subagent.h`
- Test: `seaclaw/tests/test_subagent.c` (new)

**What to do:**

1. Read `src/subagent.zig` for thread spawn + task management
2. Implement `sc_subagent_t` with:
   - `create(alloc, agent_config)` → allocate subagent context
   - `spawn(task, callback)` → pthread_create with agent_turn in the thread
   - `wait(timeout_ms)` → join or timeout
   - `destroy()` → cleanup
3. Task result collection via shared state + mutex
4. `SC_IS_TEST` guard: synchronous execution, no real threads
5. Tests: create/destroy lifecycle, spawn with mock agent, timeout handling

---

### WP-13: RAG Pipeline

**Effort:** 2 hours | **Zig ref:** `src/rag.zig` (370 lines)

**Files:**

- Rewrite: `seaclaw/src/rag.c` (12→~180 lines)
- Modify: `seaclaw/include/seaclaw/rag.h`
- Test: `seaclaw/tests/test_rag.c` (new)

**What to do:**

1. Read `src/rag.zig` for retrieve-augment-generate flow
2. Implement `sc_rag_query`:
   - Retrieve: call memory retrieval engine for relevant context
   - Augment: prepend retrieved context to user prompt
   - Generate: call provider with augmented prompt
3. Config: max_context_tokens, min_relevance_score, retrieval strategy
4. Tests: mock retrieval + mock provider, verify augmented prompt format

---

### WP-14: Voice

**Effort:** 3 hours | **Zig ref:** `src/voice.zig` (595 lines)

**Files:**

- Rewrite: `seaclaw/src/voice.c` (17→~200 lines)
- Modify: `seaclaw/include/seaclaw/voice.h`
- Test: `seaclaw/tests/test_voice.c` (new)

**What to do:**

1. Read `src/voice.zig` for Whisper integration
2. `sc_voice_stt`: Send audio to Whisper API (OpenAI or local), return transcript
3. `sc_voice_tts`: Send text to TTS API, return audio bytes
4. HTTP transport via libcurl
5. `SC_IS_TEST` guard: return mock transcript/audio
6. Tests: mock STT/TTS, error handling, config validation

---

### WP-15: Multimodal

**Effort:** 3 hours | **Zig ref:** `src/multimodal.zig` (888 lines)

**Files:**

- Rewrite: `seaclaw/src/multimodal.c` (9→~300 lines)
- Modify: `seaclaw/include/seaclaw/multimodal.h`
- Test: `seaclaw/tests/test_multimodal.c` (new)

**What to do:**

1. Read `src/multimodal.zig` for image encoding + vision request building
2. `sc_multimodal_encode_image`: Read file → base64 encode → return data URI
3. `sc_multimodal_build_vision_request`: Build provider-specific vision message format
4. Support: OpenAI vision format, Anthropic image block format, Gemini inline_data
5. Tests: base64 encoding, format building for each provider, file read errors

---

## AUDIT 2: Layer 2 Checkpoint

Full rebuild + ASan test. Verify:

- MCP can serialize/deserialize JSON-RPC
- Subagent lifecycle doesn't leak
- RAG augments prompts correctly
- Voice/multimodal format correctly for each provider
- Test count should be ~950+

---

## LAYER 3 — Storage + CLI

### WP-16: Memory Engines — Postgres, Redis, API

**Effort:** 6 hours | **Zig ref:** postgres (771), redis (1,092), api (1,265) = 3,128 lines

**Files to modify:**

- `seaclaw/src/memory/engines/postgres.c` (156→~350 lines)
- `seaclaw/src/memory/engines/redis.c` (104→~400 lines)
- `seaclaw/src/memory/engines/api.c` (115→~350 lines)
- `seaclaw/CMakeLists.txt` — conditional linking for libpq, hiredis
- Tests: `seaclaw/tests/test_memory_engines_ext.c` (new)

**What to do:**

- **Postgres**: Link libpq conditionally (`SC_ENABLE_POSTGRES`). Implement store/recall/list/forget/count using SQL (CREATE TABLE IF NOT EXISTS, INSERT, SELECT with FTS, DELETE). Connection pooling optional.
- **Redis**: Link hiredis conditionally (`SC_ENABLE_REDIS`). Use HSET/HGET/HDEL/HSCAN for key-value. TTL support.
- **API**: HTTP-based memory backend. POST/GET/DELETE to configurable endpoint. Uses libcurl.
- All three: `SC_IS_TEST` returns mock data. Real paths gated by `SC_ENABLE_*`.
- Tests: vtable contract tests (same pattern as SQLite tests), error paths, config validation

---

### WP-17: Memory Engines — Lucid, LanceDB

**Effort:** 3 hours | **Zig ref:** lucid (959), lancedb (741) = 1,700 lines

**Files to modify:**

- `seaclaw/src/memory/engines/lucid.c` (117→~300 lines)
- `seaclaw/src/memory/engines/lancedb.c` (101→~250 lines)
- Tests: `seaclaw/tests/test_memory_engines_ext.c`

**What to do:**

- **Lucid**: HTTP API to Lucid backend. Similar pattern to API engine.
- **LanceDB**: Link LanceDB C library or use HTTP API. Vector-native storage.
- Both are lower priority — can ship with stubs if external deps are problematic.

---

### WP-18: Vector Stores — pgvector, qdrant

**Effort:** 4 hours | **Zig ref:** pgvector (518), qdrant (821) = 1,339 lines

**Files to modify:**

- `seaclaw/src/memory/vector/store_pgvector.c` (stub→~250 lines)
- `seaclaw/src/memory/vector/store_qdrant.c` (stub→~350 lines)
- Tests: `seaclaw/tests/test_vector_stores.c` (new)

**What to do:**

- **pgvector**: SQL with `vector` extension. INSERT with embedding, SELECT with cosine distance. Needs libpq.
- **qdrant**: HTTP API (REST). PUT/POST points, search with vector.
- Both gated by `SC_ENABLE_*` flags.
- Tests: mock HTTP/SQL responses, serialization, error paths

---

### WP-19: CLI Commands (6 stubs)

**Effort:** 4 hours | **Zig ref:** `src/main.zig` (~2,000 lines of handlers)

**Files:**

- Modify: `seaclaw/src/main.c`
- Test: `seaclaw/tests/test_cli.c` (new or extend existing)

**Stub commands to implement:**

1. `channel` — list/status/test channels
2. `memory` — list/search/export/import memories
3. `workspace` — show/set workspace directory
4. `capabilities` — list provider capabilities and limits
5. `models` — list available models per provider
6. `auth` — manage API keys and pairing
7. `update` — self-update binary from GitHub releases

**What to do per command:**

1. Read Zig handler for argument parsing and output format
2. Implement in `cmd_*` function
3. Wire into the command dispatch table in `main()`
4. Test: argument parsing, help text, error paths

---

### WP-20: Cloudflare Workers Runtime

**Effort:** 1 hour | **Zig ref:** `src/runtime.zig` L347–388 (~42 lines)

**Files:**

- Create: `seaclaw/src/runtime/cloudflare.c`
- Modify: `seaclaw/include/seaclaw/runtime.h`
- Modify: `seaclaw/CMakeLists.txt`

**What to do:**

1. Read the CloudflareWorkers adapter in `runtime.zig`
2. Implement as a runtime that delegates to WASM-compatible I/O
3. Mostly configuration + routing — minimal actual code
4. Gated behind `SC_ENABLE_CLOUDFLARE`

---

## AUDIT 3: Layer 3 Checkpoint

Full rebuild + ASan. Verify:

- Memory engines connect (or gracefully fail) with real backends
- CLI commands produce correct output
- Test count should be ~1100+
- No memory leaks in any engine lifecycle

---

## LAYER 4 — Final Validation

### WP-21: Test Parity

**Effort:** 10 hours | **Target:** Match Zig's 3,371 tests → need ~2,700 more

**Approach:**

1. Read each Zig test file in `src/` (embedded `test "..."` blocks)
2. Group by module and create corresponding C test functions
3. Focus areas:
   - Provider edge cases: streaming, error classification, rate limiting
   - Channel integration: webhook verification, message formatting
   - Tool boundary: input validation, sandboxing, output formatting
   - Memory retrieval: ranking accuracy, edge cases
   - Security: policy bypass attempts, injection attacks
   - Config: malformed JSON, missing fields, env overrides
4. Every test must pass with ASan enabled

---

### WP-22: E2E Validation

**Effort:** 4 hours

**Checks:**

1. `cmake --build . -DCMAKE_BUILD_TYPE=MinSizeRel` → binary < 1 MB
2. Peak RSS during tests < 50 MB
3. All `SC_IS_TEST` guards verified: no real network calls in tests
4. Cross-compile: Linux x86_64, Linux aarch64, macOS aarch64
5. Full config.example.json loads without error
6. `./seaclaw agent` starts and handles one CLI turn
7. `./seaclaw gateway` starts and responds to /health
8. `./seaclaw doctor` reports all systems nominal

---

### WP-23: Promote SeaClaw to Repo Root

**Effort:** 2 hours

**Steps:**

1. Move `seaclaw/src/` → `src/` (replacing Zig source)
2. Move `seaclaw/include/` → `include/`
3. Move `seaclaw/asm/` → `asm/`
4. Move `seaclaw/tests/` → `tests/`
5. Move `seaclaw/CMakeLists.txt` → `CMakeLists.txt`
6. Delete: `build.zig`, `build.zig.zon`, `build.zig.zon2json-lock`, `vendor/sqlite3/`
7. Rewrite: `Dockerfile` (CMake build instead of Zig)
8. Rewrite: `docker-compose.yml` (binary name)
9. Rewrite: `.github/workflows/ci.yml` (from `seaclaw-ci.yml`)
10. Rewrite: `.github/workflows/release.yml` (CMake matrix build)
11. Rewrite: `README.md` (C/CMake instructions)
12. Rewrite: `AGENTS.md` (update all paths, build commands, API notes)
13. Rewrite: `.githooks/pre-commit` (clang-format or remove)
14. Rewrite: `.githooks/pre-push` (cmake test)
15. Rewrite: `run` script
16. Update: `.gitignore` (remove Zig entries)
17. Update: `flake.nix` (CMake toolchain) or delete if not using Nix
18. Delete: `seaclaw/` directory (now empty)
19. Final: `git add -A && git commit -m "feat: promote SeaClaw to repo root, remove Zig"`

---

## TIMELINE ESTIMATE

| Layer     | Work Packages       | Parallel Slots | Effort                      | Calendar    |
| --------- | ------------------- | -------------- | --------------------------- | ----------- |
| 0         | WP-01 through WP-05 | 5 parallel     | 11.5 hrs total, ~3 hrs wall | Day 1       |
| Audit 0   | —                   | 1              | 1 hr                        | Day 1       |
| 1         | WP-07 through WP-10 | 4 parallel     | 25 hrs total, ~8 hrs wall   | Days 2–3    |
| Audit 1   | —                   | 1              | 1 hr                        | Day 3       |
| 2         | WP-11 through WP-15 | 5 parallel     | 16 hrs total, ~4 hrs wall   | Day 4       |
| Audit 2   | —                   | 1              | 1 hr                        | Day 4       |
| 3         | WP-16 through WP-20 | 5 parallel     | 18 hrs total, ~6 hrs wall   | Days 5–6    |
| Audit 3   | —                   | 1              | 1 hr                        | Day 6       |
| 4         | WP-21 through WP-23 | 3 sequential   | 16 hrs                      | Days 7–8    |
| **Total** | **23 WPs**          | —              | **~90 hrs effort**          | **~8 days** |

---

## EXECUTION STRATEGY

**Recommended: Subagent-Driven Development**

Each WP dispatched as a parallel subagent where dependencies allow. After each layer:

1. Build + ASan test
2. Audit: compare against Zig reference
3. Fix issues found by audit
4. Proceed to next layer

**Parallelization limits:** Max 4 subagents at once (Cursor limit). Layer 0 has 5 WPs — run 4 parallel, then 1. Layer 1 has 4 WPs — all parallel. Layer 2 has 5 WPs — run 4 + 1.

**Quality gates:**

- Every WP must compile with `-Wall -Werror`
- Every WP must pass ASan
- Every WP must include tests
- Every WP must be reviewed against Zig reference before merge
