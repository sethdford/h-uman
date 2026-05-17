---
plan: docs/plans/2026-05-16-audit-followups/03-hook-pipeline-invocation.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: NONE
proven: PARTIAL
wired: NONE
verdict: NOT_STARTED
confidence: HIGH
---

## Plan Summary
Centralize tool dispatch into a single `hu_agent_dispatch_tool()` helper so that pre/post
hooks fire on every tool invocation (parallel, sequential, polling, HuLa). Also adds a
configurable null-registry policy (`HU_NO_HOOKS_PERMIT` / `HU_NO_HOOKS_REFUSE`).

## Key Claims (from the plan)
- Claim 1: New `hu_agent_dispatch_tool(agent, tool_name, args, out)` helper
- Claim 2: All four dispatch sites in `agent_turn.c` call it
- Claim 3: New enum `hu_no_registry_policy_t` with `HU_NO_HOOKS_PERMIT`/`HU_NO_HOOKS_REFUSE`
- Claim 4: Single grep hit for `hu_hook_pipeline_pre_tool` per direction (centralized)

## Evidence

### Implemented? (code exists)
- `grep -rn "hu_agent_dispatch_tool" src/ include/` returns 0 hits — helper not added
- `grep -rn "HU_NO_HOOKS_PERMIT\|HU_NO_HOOKS_REFUSE\|hu_no_registry_policy" src/ include/` returns 0 hits
- `hu_hook_pipeline_pre_tool` is still called from FOUR sites:
  - `src/agent/agent.c:1591`
  - `src/agent/agent_stream.c:1615`
  - `src/agent/agent_turn.c:7781`, `:7896`, `:8404` (three sites)
  - i.e., dispatch is still scattered, not centralized
- Plan claims `agent_turn.c` had hook only at parallel path (line 7740); audit predates
  the additional `agent_turn.c` invocations at 7896 and 8404, which suggest some piecemeal
  patching happened, but it's not the centralized helper the spec describes

### Proven? (tests exist)
- `tests/test_hook_pipeline.c` exists but does not exercise a centralized dispatch helper
- No null-registry-policy test
- The "counting hook fires N times across all paths" assertion in AC-1 does not exist

### Wired? (called in runtime path / dispatch)
- Hook pipeline IS called from agent paths today (4 sites). But the centralization the
  plan demands is not present — hooks can still be skipped if a future path is added
  and forgets to call the pipeline

## Gaps
- No `hu_agent_dispatch_tool()` helper
- Null-registry policy not added; default-to-allow behavior persists
- Hook invocation remains scattered across 4+ sites
- AC-1 through AC-5 all unmet (AC-5's grep test would fail)

## Notes
The audit's sibling fix (sandbox deny-by-default with `hu_shell_must_deny_unsandboxed`)
DID land — see `src/tools/shell.c:57` and 7 tests in `tests/test_shell_sandbox.c`. But
the hook-pipeline plan (03) is a separate item and remains unimplemented.
