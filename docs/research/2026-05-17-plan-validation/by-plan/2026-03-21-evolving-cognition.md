---
plan: docs/plans/2026-03-21-evolving-cognition.md
auditor: group-5-cognition-twin
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: FULL
verdict: PARTIAL
confidence: MEDIUM
---

## Plan Summary
Closed feedback loop where the agent extracts evolved opinions/preferences from past sessions and uses them as in-prompt directives for the next turn. Per-user style learning via outcome classification on prior assistant replies.

## Key Claims (from the plan)
- Claim 1: `src/intelligence/skill_learning.c` (plan-cited filename) for skill feedback
- Claim 2: Tests in `tests/test_skill_learning.c`
- Claim 3: Outcome hook updating turn correlation rows
- Claim 4: Wire into `agent_turn.c`, `prompt.c`, `bootstrap.c`
- Claim 5: Evolved opinion / preference table

## Evidence

### Implemented? (code exists)
- `src/cognition/evolving.c` (306 LOC) — evolved opinions module.
- `src/memory/evolved_opinions.c` — storage + extraction.
- `include/human/cognition/evolving.h` — API.
- BUT: `src/intelligence/skill_learning.c` (named in the plan) does NOT exist. No `hu_skill_learning_*` symbol.

### Proven? (tests exist)
- `tests/test_evolving_cognition.c` — 18 `hu_evolv*` references.
- `tests/test_skill_learning.c` (cited in plan): does NOT exist.

### Wired? (called in runtime path / dispatch)
- `src/agent/agent_turn.c:2652-2669` — `hu_evolved_opinions_ensure_table` → `get` → `build_directive` → `free`. Directive flows into prompt.
- `src/agent/frontier_prompt.c:103-120` — duplicate call site for frontier prompt path.
- `src/daemon.c:6182-6195` — daemon-side opinion retrieval and directive build for proactive paths.
- `src/daemon.c:10591` — `hu_evolved_opinions_extract_and_store` post-turn.

## Gaps
- Plan-named `skill_learning.c` and its test never materialized; the actual mechanism is `evolved_opinions` (memory table) + `cognition/evolving.c`.
- No explicit "skill outcome" feedback loop verified — the implementation focuses on opinion/preference extraction, not skill-trust delta wiring.
- `turn_correlation_id` row update flow not separately verified here.

## Notes
The plan's *spirit* (per-user learning that loops back into the next turn) is shipped via evolved opinions; the *letter* (skill_learning module) is not. Mark PARTIAL because the deliverables in the plan body don't all exist by name, even though equivalent functionality does.
