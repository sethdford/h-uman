---
plan: docs/plans/2026-05-11-init-12-mcp-server-mode.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: PARTIAL
verdict: PARTIAL
confidence: MEDIUM
---

## Plan Summary
The h-uman daemon adds an **MCP server endpoint** that exposes the user's
persona + (consented) memory + a curated subset of tools as MCP
resources / tools / prompts so Cursor, Claude Code, Copilot, etc. can
consume h-uman as a persona layer.

## Key Claims (from the plan)
- `hu_mcp_server_t` vtable layering consent + audit + rate-limit policy
- MCP resources/tools/prompts surface from persona + memory + tools
- TCP-exposed bearer-token auth per OWASP ASVS §V13 + RFC 6749
- Daemon flag/config entry to enable
- JSON-RPC 2.0 + SSE / HTTP transport

## Evidence

### Implemented? (code exists)
- Comprehensive MCP module exists:
  - `include/human/mcp_server.h` (83 LOC), `include/human/mcp.h`, `mcp_manager.h`, `mcp_jsonrpc.h`, `mcp_registry.h`, `mcp_resources.h`, `mcp_transport.h`, `mcp_context.h`.
  - Sources: `src/mcp/mcp.c`, `src/mcp/mcp_server.c` (656 LOC), `src/mcp/mcp_manager.c`, `src/mcp/mcp_jsonrpc.c`, `src/mcp/mcp_registry.c`, `src/mcp/mcp_resources.c`, `src/mcp/mcp_transport_http.c`, `src/mcp/mcp_transport_sse.c`, `src/mcp/mcp_transport_stdio.c`, `src/mcp/mcp_tool_wrapper.c`, `src/gateway/mcp_context.c`, `src/tools/mcp_resource_tools.c`, `src/security/mcp_audit.c`.
- Config integration: `include/human/config.h` lines 277–292 define `hu_mcp_server_entry_t`; line 657 declares `hu_mcp_server_entry_t mcp_servers[HU_MCP_SERVERS_MAX]`.

### Proven? (tests exist)
- Eight dedicated test files: `tests/test_mcp.c`, `test_mcp_audit.c`, `test_mcp_http_integration.c`, `test_mcp_jsonrpc.c`, `test_mcp_manager.c`, `test_mcp_resource_tools.c`, `test_mcp_resources.c`, `test_mcp_transport.c`, `test_mcp_transport_sse.c`.

### Wired? (called in runtime path / dispatch)
- `src/app/main.c` line 42 includes `"human/mcp_server.h"` and `"human/mcp_resources.h"`.
- The MCP **client** path (h-uman as a consumer of external MCP servers) appears well-wired via the manager.
- The **server** path — h-uman exposing itself to Cursor/Claude Code — needs more checking: no `cmd_mcp_serve` or equivalent CLI subcommand surfaced in the main `commands[]` table. The infrastructure exists but the user-facing "run as MCP server" verb was not visible in `src/app/main.c`'s subcommand registry.

## Gaps
- A CLI surface like `human mcp serve` or daemon-flag-driven listen mode for the server side was not located.
- Consent / rate-limit / audit modules exist (`mcp_audit.c`); but whether they're actually invoked on the server's request path was not traced end-to-end.

## Notes
This is by far the largest concrete code-mass in the Init series. The
infrastructure is real and well-tested; the question is whether the
"h-uman as MCP server to other agents" user story is plumbed all the
way to a CLI entry point. The plan implied a daemon-mode toggle; the
implementation looks oriented toward a more generic
client+server-capable module.
