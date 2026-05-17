# Sprint 3 P0 Designs — Summary

Scope: P0 stories only (US-1, US-2, US-3). US-4 through US-8 deferred to a follow-up sprint.

## Risk tier
| Story | Tier | Why |
|---|---|---|
| US-1 (structured-output opt-in) | **Medium** | Touches persona struct + JSON loader + two hot agent paths. Provider compatibility unknown until each provider's `response_format` handling is re-confirmed. |
| US-2 (Pattern C escape paths) | **High** | Streaming-chunk validator semantics is unverified; channel-format signature change affects all callers. Largest blast radius of the three. |
| US-3 (dead `channel` param) | **Low** | Mechanical signature change, internal-only, fully grep-able. |

## Parallel vs serial
All three can run in **parallel worktrees**. They touch disjoint files:
- US-1: `include/human/persona.h`, `src/persona/`, `src/agent/agent_turn.c` & `agent_stream.c` (request-construction lines).
- US-2: `src/daemon.c` (stream callback), `src/channels/format.c`, channel-format header.
- US-3: `src/agent/stop_sequence_registry.{c,h}` + 9 callers.

Light overlap: US-1 and US-2 both touch `agent_turn.c` / `agent_stream.c` but at different line ranges (US-1: request struct ~4067/1258; US-2: not in these files). Merge order: US-3 first (smallest), then US-1 and US-2 either order.

## Total estimated effort
**~M + M + XS ≈ 1.5–2.5 engineer-days** including tests and review.

## Deferred (justification)
US-4–US-8 are correctness-improving but non-blocking; US-2 closes the actual safety gap this sprint. US-6's E2E test value is reduced until US-2 lands, so deferring together is consistent.
