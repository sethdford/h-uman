# Spec: Hook Pipeline — Invoke on Every Tool Path

**Status:** Spec — not yet implemented
**Author:** 2026-05-16 audit follow-up
**Owner:** TBD
**Risk:** Medium — touches agent turn dispatch
**Effort:** 2–3 days

## Problem statement

The hook pipeline (one of the six "Claude Code features" advertised in CLAUDE.md)
intercepts pre/post tool calls for security/auditing. The audit found:

> Hook pipeline is invoked in the dispatcher's parallel path
> (`src/agent/agent_turn.c:7740`) but NOT in the sequential/polling execution
> path visible at lines 8222-8241. The hook decision defaults to `HU_HOOK_ALLOW`
> (line 38) if the registry is null.

Two gaps:
1. **Sequential path skips hooks.** If `agent_turn.c` selects the
   non-parallel branch, pre/post hooks never fire.
2. **Null registry defaults to allow.** A misconfigured agent (no registry)
   acts as if hooks were disabled, silently.

Together: a determined caller (or a misconfiguration) can run tools without
triggering hooks intended for security/auditing.

## Acceptance criteria

| AC | Description | Verification |
|---|---|---|
| AC-1 | Every tool invocation in `agent_turn.c` (parallel, sequential, polling, HuLa) passes through `hu_hook_pipeline_pre_tool()`. | New test: install a counting hook; run agent with 10 tools across all paths; assert hook count == 10. |
| AC-2 | Every tool result passes through `hu_hook_pipeline_post_tool()`. | Same test, post-side. |
| AC-3 | If `hu_hook_pipeline_pre_tool` returns `HU_HOOK_DENY`, the tool does NOT execute. | Test: deny-by-default hook; assert tool's execute is never called. |
| AC-4 | If the agent has no hook registry, behavior is configurable: `permissive` (current default) or `strict` (refuse to run tools at all). | Config flag `agent.hooks.no_registry_policy`. Tests for both modes. |
| AC-5 | Hook invocation is centralized — only one site in the codebase. Subsequent tool paths call that helper. | Inspection: grep `hu_hook_pipeline_pre_tool` should find one call site per direction. |

## Design

### Centralize the dispatch

Introduce a single tool-dispatch helper:

```c
hu_error_t hu_agent_dispatch_tool(
    hu_agent_t *agent,
    const char *tool_name,
    const hu_json_value_t *args,
    hu_tool_result_t *out);
```

Inside `hu_agent_dispatch_tool`:

```c
// 1. Permission check (already wired)
if (!hu_permission_check(agent->permission_level,
                         hu_permission_get_tool_level(tool_name)))
    return HU_ERR_SECURITY_DENIED;

// 2. Hook pre (single call site)
hu_hook_decision_t pre = hu_hook_pipeline_pre_tool(
    agent->hooks, tool_name, args, agent->no_registry_policy);
if (pre == HU_HOOK_DENY) return HU_ERR_SECURITY_DENIED;

// 3. Locate tool, execute
hu_tool_t *t = hu_tool_registry_find(agent->tools, tool_name);
if (!t) return HU_ERR_NOT_FOUND;
hu_error_t err = t->execute(t->ctx, agent->alloc, args, out);

// 4. Hook post
hu_hook_pipeline_post_tool(agent->hooks, tool_name, out, err);
return err;
```

Then replace the four existing dispatch sites in `agent_turn.c` with calls to
`hu_agent_dispatch_tool`. Each replacement is mechanical and small.

### Null-registry policy

Add to `hu_agent_t`:

```c
typedef enum {
    HU_NO_HOOKS_PERMIT = 0,  // Current default — backward-compatible
    HU_NO_HOOKS_REFUSE = 1,  // Strict — no tools run without a registry
} hu_no_registry_policy_t;
```

`hu_hook_pipeline_pre_tool(NULL, ..., HU_NO_HOOKS_REFUSE)` returns
`HU_HOOK_DENY`. The agent default stays `PERMIT`; users opt into `REFUSE`
via config.

## Out of scope

- Hook ordering / chaining semantics (already defined elsewhere).
- Hook performance budget (separate spec if needed).

## Audit evidence

- `src/agent/agent_turn.c:7740-7750` — parallel path invokes hooks.
- `src/agent/agent_turn.c:8222-8241` — sequential path does NOT.
- `src/hook_pipeline.c:30-60` — default decision is `HU_HOOK_ALLOW` for null registry.

## Risks

- **Performance regression from extra call site.** *Mitigation:* hook
  registry is a `NULL` check on the fast path; cost is one branch.
- **Test breakage from new dispatch.** *Mitigation:* keep behavior identical
  by default (PERMIT); only existing hook-aware tests change.
- **Existing tools that bypassed hooks intentionally.** *Mitigation:* audit
  call sites before refactor; document any legitimate bypass.
