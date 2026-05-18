---
plan: docs/plans/2026-05-11-init-11-proactivity-typing.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: MEDIUM
---

## Plan Summary
Two complementary features: a **PRISM proactivity gate** that decides
when the agent reaches out unsolicited, and **Stephanie2 typing
simulation** that mimics human typing cadence per-channel. Moves the
agent from "feels like a chatbot" to "feels like a real human contact".

## Key Claims (from the plan)
- PRISM gate referenced by name as the proactivity decision policy
- Stephanie2 typing-cadence model on outbound channels
- Surface in `include/human/agent/proactive.h`, channel layer, humanness module
- Integration with `feeds/awareness.c` and the agent scheduler

## Evidence

### Implemented? (code exists)
- Proactive subsystem **predates the plan**: `src/agent/proactive.c`, `include/human/agent/proactive.h` exist. `hu_proactive_budget_t`, `hu_proactive_context_t`, `hu_proactive_throttle_t` are referenced throughout `src/daemon.c`.
- Typing simulation exists in `src/channels/imessage.c`: `imessage_simulate_typing()` at line 1134, called at line 1412. Comment: *"Typing indicator with chat ID caching and group chat support."* Duration computed by `hu_imessage_typing_duration()`.
- Grep for `PRISM`, `Stephanie2`, `Stephanie`, `typing_simulator` (the canonical names from the plan) returned **zero hits in source**. The proactive gate and typing models that exist do not use the plan's vocabulary.
- No `src/agent/typing*`, no `src/channels/typing*` shared module — typing logic is per-channel-inline.

### Proven? (tests exist)
- `tests/test_proactive.c` (1428 LOC), `tests/test_proactive_ext.c` (253 LOC), `tests/test_proactive_throttle.c` (227 LOC) — extensive coverage of the pre-existing proactive surface.
- No tests named for PRISM or Stephanie2 specifically.

### Wired? (called in runtime path / dispatch)
- `src/daemon.c` line 919: `hu_service_run_proactive_checkins(...)` actively invokes the proactive subsystem.
- `src/channels/imessage.c` line 1412: typing-sim called on every outbound send (when daemon hasn't already started typing).

## Gaps
- The PRISM-specific decision logic (per the plan's arXiv refs) and Stephanie2's specific cadence model are not present — what exists is an older, simpler typing-duration + check-in throttler.
- Cross-channel typing-cadence module not present (each channel rolls its own).

## Notes
This is a "pre-existing equivalents exist; plan's specific named
algorithms did not land" case. Functionality is roughly delivered;
the SOTA-2026 upgrades named in the plan did not ship.
