# Changelog

All notable changes to human are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [0.4.0] - 2026-03-08

### Added

- **Better Than Human (BTH) framework**: 8-layer conversational intelligence system —
  conversation metrics, persona replay analysis, emotional graph, A/B response evaluation,
  proactive scheduling, superhuman services (commitment keeper, predictive coaching,
  emotional first aid, silence interpreter), tool relevance scoring
- **Persona system expansion**: full-profile analyzer, creator with JSON serialization,
  circadian time-of-day overlays, relationship depth tracking, pattern radar, per-contact
  auto-profiling from iMessage chat history, replay auto-tune daily cron
- **Conversation intelligence**: narrative detection, engagement tracking, emotion analysis,
  typing quirks (double_space_to_newline), attachment context with vision, group classification,
  response splitting, honesty guardrails, anti-repetition, farewell detection
- **Memory subsystem**: episodic summarization, STM→LTM promotion, emotional graph persistence,
  daily memory consolidation cron, graph communities with dedup, LRU + SQLite + vector backends
- **LLMCompiler**: DAG-based parallel tool execution with cycle detection, batch builder,
  variable resolution
- **Observability dashboard**: live BTH metrics, per-turn counters, pipeline health
- **Contact auto-profiling**: detect texting style from iMessage history, generate per-contact
  persona overlays
- **3 new fuzz targets**: graph, persona parse, fast capture harnesses
- **434 new tests** (3219 → 3653): promotion, vision, conversation, persona, consolidation,
  proactive, replay, BTH E2E, integration

### Fixed

- **Security**: path traversal in file tools (deny when policy NULL), constant-time gateway auth,
  OAuth race condition (pthread_mutex), WebSocket origin validation
- **Memory safety**: graph community memory leak (goto cleanup), agent_turn A/B use-after-free,
  sqlite generate_id collision (static counter), persona 57× strdup NULL checks
- **Robustness**: daemon non-blocking proactive scheduling (replaced sleep()), send_all EINTR
  retry, load_conversation_history error checking, graph store error logging, gmtime_r/localtime_r
  portability
- **CI**: websocket.c fread return value handling, gmail.c format string, accessibility contrast
  (disconnect-banner 4.5:1, sidebar active state), UI lint zero warnings
- **Stability**: conversation stack-buffer-overflow in word frequency, persona field parsing and
  deinit for all relational/social/listening/repair fields

### Changed

- Version bump 0.3.0 → 0.4.0
- Test count: 3219 → 3653 (434 new tests, 0 failures, 0 ASan errors)
- Fuzz validation: 12 harnesses, ~170M iterations, zero crashes

## [0.3.0] - 2026-03-03

### Added

- **Structured output**: `response_format` field on `hu_chat_request_t` — OpenAI, Anthropic,
  Gemini, Compatible, Ollama, OpenRouter providers all emit JSON mode payloads; 6 new tests
- **Config schema versioning**: `config_version` field with `hu_config_migrate()` — auto-migrates
  v1 configs (e.g., moves top-level `memory_backend` into `memory.backend`); 4 migration tests
- **CI ARM64 cross-compile**: `cmake/aarch64-linux-gnu.cmake` toolchain + `cross-arm64` GitHub
  Actions job verifying ARM64 ELF output
- **Ollama integration test**: graceful skip when Ollama isn't running, real chat when available;
  dedicated CI job with `ollama/ollama` service container
- **Streaming to channels**: daemon uses `hu_agent_turn_stream` + `send_event` for Telegram
  (edit), Discord (edit), and Slack (update); falls back to non-streaming for others
- **Plugin system wiring**: `plugin_loader` integrated into `main.c`, config parsing for
  `plugins.paths[]` and `plugins.enabled`, `register_channel` callback on host, shutdown cleanup;
  6 plugin tests
- **Debian packaging**: `packaging/debian/` (control, rules, changelog, copyright),
  `scripts/build-deb.sh`, CMake install targets
- **OAuth2 PKCE**: `src/gateway/oauth.c` — PKCE verifier/challenge generation (S256),
  Google/GitHub authorize URL builder, code exchange, token refresh, session validation; 8 tests
- **Multi-tenant gateway**: `src/gateway/tenant.c` — in-memory tenant store with CRUD, role-based
  access (admin/user/readonly), sliding-window rate limiting (RPM), token usage quotas; 10 tests
- **Voice channel**: `src/channels/voice_channel.c` for audio I/O integration

## [0.2.0] - 2026-03-03

### Added

- **Feature gating**: 12 CMake options (`HU_ENABLE_CRON`, `HU_ENABLE_PUSH`, `HU_ENABLE_UPDATE`,
  `HU_ENABLE_SKILLS`, `HU_ENABLE_PERIPHERALS`, `HU_ENABLE_TUNNELS`, `HU_ENABLE_RUNTIME_EXOTIC`,
  `HU_ENABLE_TOOLS_BROWSER`, `HU_ENABLE_TOOLS_ADVANCED`, `HU_ENABLE_OTEL`, etc.) for conditional
  compilation — minimal binary drops from 431 KB to 365 KB
- **CMake presets**: `minimal`, `default`, `full`, `release`, `dev` build profiles in
  `CMakePresets.json`
- **10 business tools**: analytics, broadcast, calendar, CRM, invoice, jira, report, social,
  spreadsheet, workflow — all security-hardened
- **3 new channels**: Teams (incoming webhook), Twilio SMS (Messages API + Basic auth),
  Google Chat (webhook) — all with `HU_IS_TEST` mocks and HTTPS enforcement
- **Config schema validation**: `hu_config_validate_strict` checks unknown keys, type mismatches,
  invalid URLs (`http://` rejected), path traversal (`..`), numeric range limits; strict mode via
  `HUMAN_STRICT_CONFIG=1`
- **Fuzz testing**: libFuzzer harnesses for JSON parsing, config loading, and tool parameter
  handling (`fuzz/` directory, `HU_ENABLE_FUZZ=ON`)
- **Benchmark suite**: JSON, memory backend, and config parsing benchmarks with
  `clock_gettime`/`mach_absolute_time` timing (`bench/`, `HU_ENABLE_BENCH=ON`)
- **Unified memory factory**: `hu_memory_create_from_config()` replaces 3 duplicated memory
  creation paths in `cli.c`, `main.c`, and `cli_commands.c`
- **Channel integration tests**: full lifecycle + negative cases for Teams, Twilio, Google Chat
- **Peripheral mock tests**: 15 tests for Arduino, STM32/Nucleo, RPi (create/read/write/deinit +
  error cases)
- **Provider test coverage**: 21 new tests covering all 9 providers — chat mocks, NULL request
  handling, empty message graceful errors
- **Config validation tests**: unknown key detection, type checking, URL validation, path traversal
- **Minimal preset CI job**: `minimal-build` in GitHub Actions catches feature gating regressions
- **10 documentation pages**: cron, skills, tunnels, MCP, TUI, migrate, vector memory, hardware
  peripherals, gateway API, Docker deployment — all written from source code

### Fixed

- **Jira tool**: user-controlled `base_url` accepted HTTP (now HTTPS-only enforced)
- **Spreadsheet tool**: no data size limit (now 1 MB cap against DoS)
- **Calendar tool**: unbounded `calendar_id`/`event_id` (now 200/100 char limits)
- **5 tools**: empty credential acceptance (analytics, CRM, social, broadcast, invoice)
- **5 compiler warnings**: seatbelt unused variable, pdf unused result, 3 provider const-discard
- `config.c` split into `config_serialize.c` + `config_getters.c` (was 74 KB object)
- `-Werror` enabled on `human_core` and `human` targets

### Changed

- STUBS.md audited: channels 20→22, tools 36→54, sandbox backends 4→11
- Test count: 1,834 → **2,198** (364 new tests)
- Binary size (full release): 398 KB → 431 KB (new tools/channels), minimal: **365 KB**

---

## [2026.3.3d] - 2026-03-03

### Added

- **APNS push notifications**: ES256 JWT P8 signing via OpenSSL for Apple Push Notification
  service — completes the push notification system (FCM was already implemented)
- **OTel HTTP export**: `otel_record_event` and `otel_record_metric` now POST OTLP JSON
  payloads to `/v1/logs` and `/v1/metrics` on the configured collector endpoint
- **Dispatch channel wired**: `HU_CHANNEL_DISPATCH` added to enum and channel catalog
- **4 new UI components**: `hu-data-table`, `hu-date-picker`, `hu-search`, `hu-segmented-control`
- **Design tokens**: motion tokens, expanded semantic tokens, platform token sync (iOS/Android)
- **Lighthouse CI**: `.lighthouserc.json` for automated performance/accessibility auditing
- **8 orphaned tests wired**: `test_new_features.c` (PDF, health, config, ws_streaming) now runs

### Fixed

- Removed orphan `HU_CHANNEL_WEBHOOK` enum value (unused)
- Fixed `thread_binding.c` duplicate in CMakeLists.txt
- Fixed STUBS.md tool count (66 → 54, matches actual factory)
- Filled production API paths: social (LinkedIn post/read), jira (get/comment/update),
  crm (contact_create/deal_create/deal_update/notes), calendar (DELETE graceful response)
- `.gitignore` now covers all E2E screenshot directories

### Changed

- Test count: 2,258 → 2,266 (8 previously orphaned tests now running)
- UI design-tokens sync across CSS, iOS (Swift), Android (Kotlin)
- Website documentation updates (gateway, docker, cron, MCP, migration, TUI, vector, hardware)

## [2026.3.3c] - 2026-03-03

### Added

- **10 business automation tools**: spreadsheet, report, broadcast, calendar, jira, social, crm,
  analytics, invoice, workflow — all with real implementations + `HU_IS_TEST` mock paths
- **spreadsheet**: CSV/TSV parse, analyze (stats), query (filter by column), generate
- **report**: structured Markdown/HTML report generation with templates (executive_summary,
  weekly_status, incident_report, financial_summary) and sections
- **broadcast**: multi-channel message dispatch with per-channel delivery tracking
- **calendar**: Google Calendar API integration — list events, create, delete, check availability
- **jira**: Jira REST API v3 — list (JQL), create issue, get details, comment, update
- **social**: Twitter/X + LinkedIn — post, read mentions, schedule, engagement analytics
- **crm**: HubSpot CRM API — contacts list/create, deals list/create/update, notes
- **analytics**: Plausible + Google Analytics — overview, top pages, referrers, realtime visitors
- **invoice**: create invoices (Markdown/JSON), parse invoice text, aggregate summaries
- **workflow**: DAG workflow engine with approval gates — create, run, approve, cancel, status, list
- **10 new headers**: `include/human/tools/{spreadsheet,report,broadcast,calendar_tool,jira,
social,crm,analytics,invoice,workflow}.h`
- **20 new tests**: create + functional test for each tool (analyze, template, list, post,
  contacts, overview, parse, create+run, approval_gate)
- Tool factory count increased from 25 to 35 base tools

### Changed

- `HU_TOOLS_COUNT_BASE` in factory.c: 25 → 35
- STUBS.md: 56 → 66 tools, 2,238 → 2,258 tests

## [2026.3.3b] - 2026-03-03

### Added

- **Microsoft Teams channel**: `hu_teams_create`, `hu_teams_on_webhook` (Bot Framework Activity
  JSON parsing), `hu_teams_poll` ring buffer queue, `hu_teams_destroy`; sends via Bot Framework
  REST API with Bearer auth
- **Twilio SMS channel**: `hu_twilio_create`, `hu_twilio_on_webhook` (form-urlencoded `From`/`Body`
  parsing), `hu_twilio_poll` ring buffer queue, `hu_twilio_destroy`; sends via Twilio Messages API
  with Basic auth
- **Google Chat channel**: `hu_google_chat_create`, `hu_google_chat_on_webhook` (Google Chat event
  JSON parsing with `message.text`/`message.sender.name`), `hu_google_chat_poll` ring buffer queue,
  `hu_google_chat_destroy`; sends via Google Chat Spaces API with Bearer auth
- **3 new channel headers**: `include/human/channels/teams.h`, `twilio.h`, `google_chat.h`
- **12 new tests**: create, health_check, webhook+poll, poll_null_args for each of Teams, Twilio,
  Google Chat
- **CMake options**: `HU_ENABLE_TEAMS`, `HU_ENABLE_TWILIO`, `HU_ENABLE_GOOGLE_CHAT` (all included
  in `HU_ENABLE_ALL_CHANNELS`)

### Changed

- Channel catalog: 21 send+receive channels (was 18)
- `channel_catalog.c`: added Teams, Twilio, Google Chat as `HU_LISTENER_POLLING`
- `hu_channel_catalog_requires_runtime`: returns true for Teams, Twilio, Google Chat

## [2026.3.3] - 2026-03-03

### Added

- **Matrix inbound polling**: `hu_matrix_poll` via Matrix `/sync` API with `since` token tracking,
  room timeline parsing, and sender filtering (skips own messages)
- **IRC inbound polling**: `hu_irc_poll` via `select()`+`recv()` on existing socket, PRIVMSG parsing,
  automatic PING/PONG handling, and line-buffered read with `\r\n` framing
- **SSE streaming for Ollama, OpenRouter, Compatible**: all three providers now implement
  `supports_streaming` and `stream_chat` vtable methods; in test mode returns mock delta chunks,
  in prod delegates to non-streaming chat then emits as single chunk (full SSE parsing TBD)
- **Real `update.check`**: control protocol handler now calls `hu_update_check()` to fetch latest
  version from GitHub releases API, compares with current, returns `{available, current, latest}`
- **Real `update.run`**: control protocol handler now calls `hu_update_apply()` to download and
  replace binary, returns `{status: "updated"}` or error string
- **Enriched `nodes.list`**: local node now includes `hostname`, `version` (from `hu_version_string()`),
  and `uptime_secs` (from `CLOCK_MONOTONIC`) in addition to existing fields
- **LINE/Lark/DingTalk/QQ/OneBot webhook handlers**: `hu_{ch}_on_webhook` parses platform-specific
  JSON payloads and queues inbound messages into a ring buffer for `hu_{ch}_poll` consumption
- **Mattermost REST polling**: `hu_mattermost_poll` via `GET /api/v4/channels/{id}/posts` with
  `after` cursor tracking, post ordering, and user ID extraction
- **Channel headers**: all 7 new channels get public headers in `include/human/channels/`
- **Slack inbound polling**: `hu_slack_poll` via Slack `conversations.history` API; `hu_slack_create_ex`
  accepts `channel_ids` for multi-channel polling, bot message filtering, and `last_ts` cursor tracking
- **WhatsApp inbound**: `hu_whatsapp_on_webhook` parses WhatsApp Cloud API webhook payloads
  (entry → changes → value → messages), queues inbound text messages for `hu_whatsapp_poll` consumption
- **Multi-agent orchestration wiring**: agent pool, mailbox, policy engine, thread binding,
  and agent profiles wired into `cli.c`, `main.c` (service, gateway, MCP modes)
- **Policy engine in tool dispatch**: `hu_agent_check_policy` evaluates tool calls against policy
  engine before execution; denied calls return "denied by policy"
- **Slash commands**: `/spawn <task>`, `/agents`, `/cancel <id>` in agent loop using agent pool
- **`hu_message_tool_set_channel`**: post-creation channel injection for the message tool
- **`hu_gateway_path_is`**: path-matching utility for gateway routing
- **Config parsing**: `pool_max_concurrent`, `default_profile`, `policy`, `plugins`, `slack`, `whatsapp`
  sections in `~/.human/config.json`
- **OTel observer**: created when config has `otel_endpoint`
- **28 new tests**: 6 streaming, 4 matrix/irc poll, 10 webhook+poll (LINE, Lark, Mattermost, OneBot,
  DingTalk × webhook+null), 1 update_apply, 13 roadmap integration, 4 channel tests
- **Performance profile**: 398 KB binary (MinSizeRel+LTO), ~5.1 MB peak RSS

### Changed

- Matrix channel listener type: `HU_LISTENER_GATEWAY` → `HU_LISTENER_POLLING`
- IRC channel listener type: `HU_LISTENER_GATEWAY` → `HU_LISTENER_POLLING`
- Slack channel listener type: `HU_LISTENER_GATEWAY` → `HU_LISTENER_POLLING`
- WhatsApp channel listener type: `HU_LISTENER_WEBHOOK_ONLY` → `HU_LISTENER_POLLING`
- LINE, Lark, Mattermost, OneBot, DingTalk, QQ listener types: `HU_LISTENER_WEBHOOK_ONLY` → `HU_LISTENER_POLLING`
- Ollama, OpenRouter, Compatible providers: streaming column "No" → "Yes (SSE)"
- `nodes.list` response: added `hostname`, `version`, `uptime_secs` to local node
- `update.check` response: now returns real version comparison instead of hardcoded false
- `update.run` response: now calls `hu_update_apply()` instead of returning "up_to_date"
- Service loop `channels` array expanded from 8 → 10 to accommodate Slack and WhatsApp
- `hu_channels_config_t` extended with `hu_slack_channel_config_t` and `hu_whatsapp_channel_config_t`

## [2026.3.2] - 2026-03-02

### Added

- **Cross-platform sandbox tier system**: 8 sandbox backends with OS-native kernel isolation
  - **Seatbelt** (macOS): kernel-level SBPL profiles via `sandbox-exec`, near-zero overhead
  - **Landlock** (Linux): kernel-level filesystem ACLs via Landlock LSM (fixed: now applies real rules)
  - **seccomp-BPF** (Linux): kernel-level syscall filtering, blocks ptrace/mount/reboot/kexec
  - **Landlock+seccomp** (Linux): combined backend for Chrome-grade FS + syscall isolation
  - **WASI** (cross-platform): capability-based isolation via wasmtime/wasmer
  - **Firecracker** (Linux): hardware-level microVM isolation via KVM, sub-200ms boot
  - **AppContainer** (Windows): Job Object + capability-based isolation
- **Sandbox `apply` callback**: vtable extension for kernel-level sandboxes that apply restrictions
  programmatically after fork() rather than via argv wrapping
- **`hu_process_run_sandboxed`**: new API in process_util that invokes sandbox `apply` callback
  between fork() and child execution, enabling Landlock/seccomp enforcement
- **Network isolation proxy**: `hu_net_proxy_t` for domain-based network filtering with wildcard
  subdomain matching, composable with any sandbox backend
- **Tiered auto-detection**: `HU_SANDBOX_AUTO` now selects the strongest available backend per OS:
  macOS Seatbelt -> Linux Landlock+seccomp -> Bubblewrap -> Firejail -> Firecracker -> Docker -> WASI -> noop
- **Config schema**: all new backends configurable via `~/.human/config.json` `security.sandbox` field
  (`"seatbelt"`, `"seccomp"`, `"landlock+seccomp"`, `"wasi"`, `"firecracker"`, `"appcontainer"`)
- **Claude Code integration**: `claude_code` tool delegates coding tasks to Claude CLI via `fork`/`exec`
- **MCP server**: `human --mcp` exposes all tools as JSON-RPC over stdin/stdout (MCP protocol)
- **MCP client**: load external MCP servers from `config.json` and merge their tools into the agent
- **Bidirectional email**: IMAP polling for inbound messages, SMTP with auth for outbound
- **Bidirectional iMessage**: poll macOS `chat.db` for inbound, AppleScript for outbound
- **Always-on service loop**: `human service` daemonizes; polls channels, dispatches to agent, sends replies
- **Persistent sessions**: service loop saves/loads per-sender conversation history via session store
- **Full cron syntax**: `*/N` steps, `N-M` ranges, `N-M/S` range-steps, `N,M,O` comma lists
- **Shell completions**: bash and zsh completion scripts for all subcommands
- **Homebrew formula**: `Formula/human.rb` for macOS distribution
- **Channel docs**: individual documentation pages for Email, iMessage, IRC, Matrix, Web channels
- **E2E test script**: `scripts/e2e-test.sh` validates full service loop end-to-end
- **`.mcp.json`**: project-level config to register human as MCP server for Claude Code

### Changed

- `delegate` tool now routes to `claude_code` when agent is "claude_code"
- `hu_service_run` accepts an `hu_agent_t` for message dispatch (was poll-only)
- `hu_service_channel_t` includes `hu_channel_t *channel` for sending replies
- Service loop clears and restores per-session history before each agent turn
- Cron field parser uses `strtol` with bounds checking (was `atoi`)
- Autonomy level cast clamped to valid enum range in service loop

### Fixed

- Memory leak in `read_stdin_line` (MCP server) — returned capacity now tracked
- Use-after-free risk in `handle_tools_list` — buffer length captured before free
- Memory leak in tool factory when MCP tools merged — allocation size tracked through realloc
- Forward-reference bug in `dispatcher.c` — moved declarations before first use
- Docker-compose healthcheck URL corrected to port 3000
- Off-by-one in MCP `tools/call` response JSON literal length

### Security

- Cron parser rejects malformed expressions (negative values, overflow, step-of-zero)
- Service loop isolates sessions — no cross-sender context bleed
- Autonomy level validated before policy construction

## [2026.3.1] - 2026-03-01

### Added

- WSS/TLS support for WebSocket client via OpenSSL
- ASan build instructions in documentation
- Optional engine stubs documented

### Fixed

- Gemini provider URL bug
- Optional model config handling
- Spawn tool schema validation
- JSON logging suppressed in interactive CLI
- Preprocessor balance across channels and tests

## [2026.2.20] - 2026-02-20

### Added

- Complete nullclaw-to-human rename
- Claude CLI provider
- Zig artifacts removed (pure C11)

### Changed

- Full C11 port replacing Zig runtime — feature parity achieved
