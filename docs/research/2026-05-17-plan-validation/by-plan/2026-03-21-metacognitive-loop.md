---
plan: docs/plans/2026-03-21-metacognitive-loop.md
auditor: group-5-cognition-twin
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Self-monitoring meta-controller that watches the turn for signals (uncertainty, tool failures, contradiction), plans an action (e.g., switch strategy, escalate to slow path), and applies a directive into the next prompt.

## Key Claims (from the plan)
- Claim 1: `src/agent/metacognition.c` module
- Claim 2: Signal/action/apply API
- Claim 3: Per-agent metacognition state initialized in agent ctor
- Claim 4: Wired into agent stream/turn
- Claim 5: Tests in `tests/test_metacognition.c`

## Evidence

### Implemented? (code exists)
- `src/cognition/metacognition.c` (658 LOC) — full module.
- `include/human/cognition/metacognition.h` — API.
- Note: file path landed as `src/cognition/metacognition.c`, not `src/agent/metacognition.c` as the plan suggested.

### Proven? (tests exist)
- `tests/test_metacognition.c` — 76 `hu_metacog*` references (substantial coverage).

### Wired? (called in runtime path / dispatch)
- `src/agent/agent.c:691` — `hu_metacognition_init(&out->infra.metacognition)` at agent creation.
- `src/agent/cli.c:657` — `hu_metacognition_apply_config` from CLI config.
- `src/agent/agent_stream.c:1949-1957` — `hu_metacognition_monitor` → `hu_metacognition_plan_action` → `hu_metacognition_apply` writes directive into prompt.
- `src/agent/agent_turn.c:4155-4169` — `hu_metacog_label_from_followup`, `hu_metacog_history_update_outcome`, `hu_metacog_estimate_difficulty` invoked end-of-turn for learning.

## Gaps
- Plan frontmatter says `status: in-progress` — closer to ground truth than the others, but the loop is fully wired.
- Path drift: plan path `src/agent/metacognition.c` vs actual `src/cognition/metacognition.c`.

## Notes
This is the most "in-progress" plan that's actually furthest along — wired in both stream and non-stream paths, with end-of-turn outcome learning.
