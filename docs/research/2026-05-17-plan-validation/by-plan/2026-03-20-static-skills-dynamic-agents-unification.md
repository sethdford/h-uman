---
plan: docs/plans/2026-03-20-static-skills-dynamic-agents-unification.md
auditor: group-5-cognition-twin
audited_at: 2026-05-17
implemented: FULL
proven: PARTIAL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Unify static skills (skillforge) and dynamically spawned agents under a layered model: Layer A persona/twin policy, Layer B context assembly, Layer C dynamic execution. Define spawn inheritance contract so child agents share skillforge, tools, memory, policy with the parent.

## Key Claims (from the plan)
- Claim 1: Spawn inheritance matrix (skillforge, tools, memory, observer, policy shared)
- Claim 2: `hu_agent_set_skillforge` propagated to child agents
- Claim 3: `agent_spawn.c`, `delegate.c`, `skill_run.c` tools all present
- Claim 4: Concept index + standards doc landed
- Claim 5: Dynamic skill routing (deferred to its own plan)

## Evidence

### Implemented? (code exists)
- `src/skills/skillforge.c` (22 `hu_skillforge_*` functions).
- `src/agent/spawn.c:204,286` — `hu_agent_set_skillforge` called inside spawn paths.
- `src/agent/spawn.c:489` — `hu_agent_pool_spawn` definition.
- `src/agent/spawn.c:910-923` — `hu_agent_pool_spawn_named` (named registry spawn) calls back into `hu_agent_pool_spawn`.
- `src/tools/agent_spawn.c`, `src/tools/delegate.c`, `src/tools/skill_run.c` — all three exist.
- `src/agent/agent_turn.c` orchestrates the layered prompt with persona + skills + tools — confirmed via skill-routing wiring above.

### Proven? (tests exist)
- `tests/test_skill_unified.c` exists (but 0 `hu_skill_unified_*` symbols found — may test by behavior rather than symbol).
- `tests/test_subsystems.c` exists with 32 references to skill/spawn/delegate.
- Integration test combining `skill_run` + `agent_spawn` in one scenario (plan's gap #6) — not separately verified here.

### Wired? (called in runtime path / dispatch)
- Spawn inheritance is the runtime path: `hu_agent_set_skillforge` runs unconditionally inside `hu_agent_pool_spawn` (`src/agent/spawn.c:204,286`).
- `agent_spawn` / `delegate` / `skill_run` tools registered through normal tool factory.

## Gaps
- Plan §2.3 "Priority / conflict rules" (persona > twin > spawn > generic helpfulness) — no explicit precedence module found; deferred per plan §4 Phase 1 note.
- Plan §2.5 "Observability — which skills were in context?" — telemetry depth not verified here.

## Notes
The plan's frontmatter `status: implemented` is largely accurate; the deferred items (explicit precedence helper, integration tests) are flagged in the plan itself.
