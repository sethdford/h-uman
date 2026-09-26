---
title: "Fleet — pooled sub-agents (spawn) limits"
created: 2026-03-20
status: active
---

# Fleet — pooled sub-agents (spawn) limits

The **fleet** is the **`hu_agent_pool_t`** that backs **`agent_spawn`**, **`/spawn`**, **`delegate`** (named agents), and related slash commands. This used to be contrasted with a separate **scheduler** `max_concurrent` owned by `hu_subagent_manager_t`; that module (`src/subagent.c`) was merged into `src/agent/spawn.c` on 2026-09-20 as a duplicate of its live twin, so the pool’s own `pool_max_concurrent` is now the only concurrency limit.

## Limits (config → pool)

Set under `agent` in `~/.human/config.json`:

| Key | Default | Meaning |
| --- | ------- | ------- |
| `pool_max_concurrent` | 8 | Max **running** one-shot/persistent workers at once |
| `fleet_max_spawn_depth` | 8 | Max **nesting** depth (child depth = parent `spawn_depth` + 1). `0` = unlimited |
| `fleet_max_total_spawns` | 0 | Lifetime **starts** in this process (`0` = unlimited) |
| `fleet_budget_usd` | 0 | Session spend cap (`0` = off). Requires the spawn caller to pass `shared_cost_tracker` — see below |

## Runtime behavior

- **Root** agents have `spawn_depth == 0`. Each successful spawn creates a child with `spawn_depth == parent + 1`.
- **`shared_cost_tracker`**: Spawn paths pass the parent’s cost tracker when available so token usage accrues to the same session and **fleet budget** checks use `hu_cost_session_total`.
- **Pool-level cost tracker — no longer bindable.** The pool still carries a `fleet_cost_tracker` field that both the budget check and **`/fleet`** “session spend” fall back to, but its only setter (`hu_agent_pool_bind_fleet_cost_tracker`) was deleted on 2026-09-20 with zero callers — see `docs/plans/2026-09-20-dead-code-plan.md` Appendix B. The field is initialized to NULL and nothing sets it, so the budget check reads only the caller’s `shared_cost_tracker`, and a spawn with `fleet_budget_usd > 0` and no `shared_cost_tracker` always fails with `HU_ERR_INVALID_ARGUMENT`.

## Observability

- **`/fleet`** — limits, running count, slots in use, lifetime spawns started, session spend (always `0` while no pool cost tracker can be bound — see above).
- **`/agents`** — per-slot status (unchanged).

## Errors

| Code | When |
| ---- | ---- |
| `HU_ERR_FLEET_DEPTH_EXCEEDED` | Would exceed `fleet_max_spawn_depth` |
| `HU_ERR_FLEET_SPAWN_CAP` | Would exceed `fleet_max_total_spawns` |
| `HU_ERR_FLEET_BUDGET_EXCEEDED` | Session total already ≥ `fleet_budget_usd` |
| `HU_ERR_INVALID_ARGUMENT` | `fleet_budget_usd > 0` but no cost tracker available for the check |

## References

- [`docs/standards/ai/skills-vs-agents.md`](skills-vs-agents.md) — when to spawn vs use skills
- [`docs/research/2026-03-20-sota-agents-skills-companion.md`](../../research/2026-03-20-sota-agents-skills-companion.md) — multi-agent economics
