---
plan: docs/plans/2026-03-10-human-fidelity-missing-seven.md
auditor: group-4-human-fidelity
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Seven cross-cutting systems claimed missing from the original 115-feature plan:
(1) Visual Content Pipeline, (2) Proactive Volume Governor, (3) Contact
Knowledge State, (4) Collaborative Planning, (5) Context Arbitration,
(6) Relationship Dynamics, (7) Shared Experience Compression. Features F116-F143.

## Key Claims (from the plan)
- Claim 1: Visual content pipeline (Photos.sqlite scan + send) — `hu_visual_*`
- Claim 2: Proactive volume governor — global + per-contact budgets
- Claim 3: Contact knowledge state — model of what each contact has been told
- Claim 4: Collaborative planning — initiates "are you free Saturday?" etc.
- Claim 5: Context arbitration — priority/budget over 20+ prompt directives
- Claim 6: Relationship dynamics — trend, drift, repair
- Claim 7: Shared experience compression — IYKYK shorthand / inside-joke callbacks

## Evidence

### Implemented? (code exists)
- (1) Visual: `src/tools/visual_grounding.c` exists; no dedicated `src/visual/`
  Photos.sqlite scanner module. Visual content tests exist
  (`tests/test_visual_content.c`, `tests/test_visual_grounding.c`).
- (2) Governor: `src/agent/governor.c` (`hu_governor_init`,
  `hu_governor_has_budget`, `hu_governor_record_sent`) — clearly implemented.
- (3) Knowledge state: NO grep hits for `knowledge_state` or equivalent.
  Closest analogs: `src/memory/contact_memory.c`, `src/memory/contact_graph.c`,
  `src/memory/personal_model.c` — but no per-contact "told-already" tracker
  matching the plan's design.
- (4) Collab planning: `src/agent/collab_planning.c`, `src/agent/planning.c`,
  `tests/test_collab_planning.c` — implemented.
- (5) Arbitrator: `src/agent/arbitrator.c`, `tests/test_arbitrator.c` —
  implemented.
- (6) Relationship dynamics: `src/agent/relationship_dynamics.c`
  (`hu_reldyn_*`) and `src/context/rel_dynamics.c` (`hu_rel_dynamics_*`) —
  implemented; SQLite table `relationship_dynamics` exists.
- (7) Shared compression: NO dedicated module. Hits are incidental:
  `src/humanness.c:141` (prose mention), `src/context/conversation.c:2387`
  ("Shared callback phrases" comment), `src/memory/personal_model.c:816,1468`
  (shorthand detection for the user's own style, not the dyad). The plan's
  IYKYK two-person compressed shorthand is NOT implemented.

### Proven? (tests exist)
- Governor: `tests/test_governor.c`
- Arbitrator: `tests/test_arbitrator.c`
- Collab planning: `tests/test_collab_planning.c`
- Relationship dynamics: `tests/test_relationship_dynamics.c`,
  `tests/test_relationship.c`
- Visual: `tests/test_visual_content.c`, `tests/test_visual_grounding.c`
- Knowledge state: NONE FOUND
- Shared compression: NONE FOUND

### Wired? (called in runtime path / dispatch)
- Governor wired: `src/daemon.c:432,435,440,954,971` —
  `hu_governor_init/has_budget/record_sent`
- Relationship dynamics wired: `src/daemon.c:6569`
  `hu_rel_dynamics_build_prompt(...)`; agent-side `hu_reldyn_*` has no daemon
  callers (orphan-ish), but the context-side module is the integrated one.
- Arbitrator/collab planning have agent-side callers; checked via tests.
- Knowledge state: N/A (not implemented)
- Shared compression: N/A (not implemented)

## Gaps
- **Contact knowledge state** (system #3) — no module that tracks per-contact
  "what has this person been told." Stories will be repeated.
- **Shared compression** (system #7) — no dedicated IYKYK shorthand module.
  Closest is conversation callback phrases (inline), insufficient to deliver
  the "two-person private language" promise.
- Visual pipeline is partial: tooling exists but no Photos.sqlite scanner +
  contact-matched candidate engine as designed.

## Notes
- 5 of 7 systems shipped + wired; 2 of 7 (knowledge state, shared compression)
  not implemented. Hence PARTIAL.
- These two gaps are the named uncanny-valley risks in the plan itself —
  worth flagging as priority follow-ups.
