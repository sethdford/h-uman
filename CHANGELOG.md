# Changelog

All notable changes to seaclaw are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/). Versioning is CalVer (`YYYY.M.D`).

## [2026.3.3c] - 2026-03-03

### Added

- **10 business automation tools**: spreadsheet, report, broadcast, calendar, jira, social, crm,
  analytics, invoice, workflow — all with real implementations + `SC_IS_TEST` mock paths
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
- **10 new headers**: `include/seaclaw/tools/{spreadsheet,report,broadcast,calendar_tool,jira,
social,crm,analytics,invoice,workflow}.h`
- **20 new tests**: create + functional test for each tool (analyze, template, list, post,
  contacts, overview, parse, create+run, approval_gate)
- Tool factory count increased from 25 to 35 base tools

### Changed

- `SC_TOOLS_COUNT_BASE` in factory.c: 25 → 35
- STUBS.md: 46 → 56 tools, 2,226 → 2,258 tests

## [2026.3.3b] - 2026-03-03

### Added

- **Microsoft Teams channel**: `sc_teams_create`, `sc_teams_on_webhook` (Bot Framework Activity
  JSON parsing), `sc_teams_poll` ring buffer queue, `sc_teams_destroy`; sends via Bot Framework
  REST API with Bearer auth
- **Twilio SMS channel**: `sc_twilio_create`, `sc_twilio_on_webhook` (form-urlencoded `From`/`Body`
  parsing), `sc_twilio_poll` ring buffer queue, `sc_twilio_destroy`; sends via Twilio Messages API
  with Basic auth
- **Google Chat channel**: `sc_google_chat_create`, `sc_google_chat_on_webhook` (Google Chat event
  JSON parsing with `message.text`/`message.sender.name`), `sc_google_chat_poll` ring buffer queue,
  `sc_google_chat_destroy`; sends via Google Chat Spaces API with Bearer auth
- **3 new channel headers**: `include/seaclaw/channels/teams.h`, `twilio.h`, `google_chat.h`
- **12 new tests**: create, health_check, webhook+poll, poll_null_args for each of Teams, Twilio,
  Google Chat
- **CMake options**: `SC_ENABLE_TEAMS`, `SC_ENABLE_TWILIO`, `SC_ENABLE_GOOGLE_CHAT` (all included
  in `SC_ENABLE_ALL_CHANNELS`)

### Changed

- Channel catalog: 21 send+receive channels (was 18)
- `channel_catalog.c`: added Teams, Twilio, Google Chat as `SC_LISTENER_POLLING`
- `sc_channel_catalog_requires_runtime`: returns true for Teams, Twilio, Google Chat

## [2026.3.3] - 2026-03-03

### Added

- **Matrix inbound polling**: `sc_matrix_poll` via Matrix `/sync` API with `since` token tracking,
  room timeline parsing, and sender filtering (skips own messages)
- **IRC inbound polling**: `sc_irc_poll` via `select()`+`recv()` on existing socket, PRIVMSG parsing,
  automatic PING/PONG handling, and line-buffered read with `\r\n` framing
- **SSE streaming for Ollama, OpenRouter, Compatible**: all three providers now implement
  `supports_streaming` and `stream_chat` vtable methods; in test mode returns mock delta chunks,
  in prod delegates to non-streaming chat then emits as single chunk (full SSE parsing TBD)
- **Real `update.check`**: control protocol handler now calls `sc_update_check()` to fetch latest
  version from GitHub releases API, compares with current, returns `{available, current, latest}`
- **Real `update.run`**: control protocol handler now calls `sc_update_apply()` to download and
  replace binary, returns `{status: "updated"}` or error string
- **Enriched `nodes.list`**: local node now includes `hostname`, `version` (from `sc_version_string()`),
  and `uptime_secs` (from `CLOCK_MONOTONIC`) in addition to existing fields
- **LINE/Lark/DingTalk/QQ/OneBot webhook handlers**: `sc_{ch}_on_webhook` parses platform-specific
  JSON payloads and queues inbound messages into a ring buffer for `sc_{ch}_poll` consumption
- **Mattermost REST polling**: `sc_mattermost_poll` via `GET /api/v4/channels/{id}/posts` with
  `after` cursor tracking, post ordering, and user ID extraction
- **Channel headers**: all 7 new channels get public headers in `include/seaclaw/channels/`
- **Slack inbound polling**: `sc_slack_poll` via Slack `conversations.history` API; `sc_slack_create_ex`
  accepts `channel_ids` for multi-channel polling, bot message filtering, and `last_ts` cursor tracking
- **WhatsApp inbound**: `sc_whatsapp_on_webhook` parses WhatsApp Cloud API webhook payloads
  (entry → changes → value → messages), queues inbound text messages for `sc_whatsapp_poll` consumption
- **Multi-agent orchestration wiring**: agent pool, mailbox, policy engine, thread binding,
  and agent profiles wired into `cli.c`, `main.c` (service, gateway, MCP modes)
- **Policy engine in tool dispatch**: `sc_agent_check_policy` evaluates tool calls against policy
  engine before execution; denied calls return "denied by policy"
- **Slash commands**: `/spawn <task>`, `/agents`, `/cancel <id>` in agent loop using agent pool
- **`sc_message_tool_set_channel`**: post-creation channel injection for the message tool
- **`sc_gateway_path_is`**: path-matching utility for gateway routing
- **Config parsing**: `pool_max_concurrent`, `default_profile`, `policy`, `plugins`, `slack`, `whatsapp`
  sections in `~/.seaclaw/config.json`
- **OTel observer**: created when config has `otel_endpoint`
- **28 new tests**: 6 streaming, 4 matrix/irc poll, 10 webhook+poll (LINE, Lark, Mattermost, OneBot,
  DingTalk × webhook+null), 1 update_apply, 13 roadmap integration, 4 channel tests
- **Performance profile**: 398 KB binary (MinSizeRel+LTO), ~5.1 MB peak RSS

### Changed

- Matrix channel listener type: `SC_LISTENER_GATEWAY` → `SC_LISTENER_POLLING`
- IRC channel listener type: `SC_LISTENER_GATEWAY` → `SC_LISTENER_POLLING`
- Slack channel listener type: `SC_LISTENER_GATEWAY` → `SC_LISTENER_POLLING`
- WhatsApp channel listener type: `SC_LISTENER_WEBHOOK_ONLY` → `SC_LISTENER_POLLING`
- LINE, Lark, Mattermost, OneBot, DingTalk, QQ listener types: `SC_LISTENER_WEBHOOK_ONLY` → `SC_LISTENER_POLLING`
- Ollama, OpenRouter, Compatible providers: streaming column "No" → "Yes (SSE)"
- `nodes.list` response: added `hostname`, `version`, `uptime_secs` to local node
- `update.check` response: now returns real version comparison instead of hardcoded false
- `update.run` response: now calls `sc_update_apply()` instead of returning "up_to_date"
- Service loop `channels` array expanded from 8 → 10 to accommodate Slack and WhatsApp
- `sc_channels_config_t` extended with `sc_slack_channel_config_t` and `sc_whatsapp_channel_config_t`

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
- **`sc_process_run_sandboxed`**: new API in process_util that invokes sandbox `apply` callback
  between fork() and child execution, enabling Landlock/seccomp enforcement
- **Network isolation proxy**: `sc_net_proxy_t` for domain-based network filtering with wildcard
  subdomain matching, composable with any sandbox backend
- **Tiered auto-detection**: `SC_SANDBOX_AUTO` now selects the strongest available backend per OS:
  macOS Seatbelt -> Linux Landlock+seccomp -> Bubblewrap -> Firejail -> Firecracker -> Docker -> WASI -> noop
- **Config schema**: all new backends configurable via `~/.seaclaw/config.json` `security.sandbox` field
  (`"seatbelt"`, `"seccomp"`, `"landlock+seccomp"`, `"wasi"`, `"firecracker"`, `"appcontainer"`)
- **Claude Code integration**: `claude_code` tool delegates coding tasks to Claude CLI via `fork`/`exec`
- **MCP server**: `seaclaw --mcp` exposes all tools as JSON-RPC over stdin/stdout (MCP protocol)
- **MCP client**: load external MCP servers from `config.json` and merge their tools into the agent
- **Bidirectional email**: IMAP polling for inbound messages, SMTP with auth for outbound
- **Bidirectional iMessage**: poll macOS `chat.db` for inbound, AppleScript for outbound
- **Always-on service loop**: `seaclaw service` daemonizes; polls channels, dispatches to agent, sends replies
- **Persistent sessions**: service loop saves/loads per-sender conversation history via session store
- **Full cron syntax**: `*/N` steps, `N-M` ranges, `N-M/S` range-steps, `N,M,O` comma lists
- **Shell completions**: bash and zsh completion scripts for all subcommands
- **Homebrew formula**: `Formula/seaclaw.rb` for macOS distribution
- **Channel docs**: individual documentation pages for Email, iMessage, IRC, Matrix, Web channels
- **E2E test script**: `scripts/e2e-test.sh` validates full service loop end-to-end
- **`.mcp.json`**: project-level config to register seaclaw as MCP server for Claude Code

### Changed

- `delegate` tool now routes to `claude_code` when agent is "claude_code"
- `sc_service_run` accepts an `sc_agent_t` for message dispatch (was poll-only)
- `sc_service_channel_t` includes `sc_channel_t *channel` for sending replies
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

- Complete nullclaw-to-seaclaw rename
- Claude CLI provider
- Zig artifacts removed (pure C11)

### Changed

- Full C11 port replacing Zig runtime — feature parity achieved
