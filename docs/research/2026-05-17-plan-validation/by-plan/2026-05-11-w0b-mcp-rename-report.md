---
plan: docs/plans/2026-05-11-w0b-mcp-rename-report.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Rename report for W0b: `hu_mcp_server_t` → `hu_mcp_client_t` (client connection type)
and `hu_mcp_host_t` → `hu_mcp_engine_t` (JSON-RPC dispatcher), freeing the
`hu_mcp_server_t` slot for init #12's new server vtable. Includes deprecation shims
for the `hu_mcp_host_*` function family.

## Key Claims (from the plan)
- Claim 1: `hu_mcp_client_t` and family in `include/human/mcp.h`
- Claim 2: `hu_mcp_engine_t` and family in `include/human/mcp_server.h`
- Claim 3: `hu_mcp_server_t` slot is FREE (no live definition)
- Claim 4: 5 `hu_mcp_host_*` deprecation shims in `mcp_server.h`
- Claim 5: All call sites migrated, 9715/9715 tests pass

## Evidence

### Implemented? (code exists)
- `include/human/mcp.h:27` — `typedef struct hu_mcp_client hu_mcp_client_t;` ✓
- `include/human/mcp.h:29-45` — public `hu_mcp_client_*` API (create/connect/list_tools/call_tool/destroy/reconnect/refresh_tools) ✓
- `include/human/mcp_server.h:23` — `typedef struct hu_mcp_engine hu_mcp_engine_t;` ✓
- `include/human/mcp_server.h:26-29+` — public `hu_mcp_engine_*` API ✓
- `grep -rn "\bhu_mcp_server_t\b" include/human/` returns only documentation comments
  (mcp.h:15 historical note, mcp_server.h:17 init-#12 forward pointer) — slot is FREE ✓
- `grep -rn "\bhu_mcp_host_t\b" include/human/` shows only the shim line

### Proven? (tests exist)
- `tests/test_mcp.c` — exercises new client + engine API per the report
- Spot-checked suites in the report (`--filter=mcp_client` 27/27, `--filter=mcp_engine` 24/24)
- 9715/9715 full pass at slice landing per the report

### Wired? (called in runtime path / dispatch)
- `src/mcp.c`, `src/mcp_manager.c`, `src/mcp_server.c` all reference the new names
- `src/main.c` uses `hu_mcp_engine_*` (per report's call-site count of 19 hits)

## Gaps
- None for W0b itself
- Plan correctly notes: removal of the 5 `hu_mcp_host_*` shims is deferred to a future
  major version bump (after init #12 ships)

## Notes
This is a rename report, not a forward plan. The work it documents has shipped.
The `hu_mcp_server_t` slot remains free as of 2026-05-17.
