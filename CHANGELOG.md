# Changelog

All notable changes to seaclaw are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/). Versioning is CalVer (`YYYY.M.D`).

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
