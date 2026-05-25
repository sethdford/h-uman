---
title: "W5 — Agent-Writable Persona + Procedural Memory"
created: 2026-05-10
status: closed
parent: 2026-05-10-memory-roadmap-overview.md
depends_on:
  - 2026-05-10-w1-bitemporal-foundation.md
  - 2026-05-10-w4-self-rag-provenance.md
risk: high
scope: src/persona/, src/agent/, include/human/, ui/
last_audit: 2026-05-25
---

# W5 — Agent-Writable Persona + Procedural Memory

## Goal

Let h-uman propose persona overlay deltas and procedural memory additions (system-prompt-level heuristics) based on what it observes during conversations — gated by user confirmation. Add a persona-evolver subagent that runs weekly and pulls together feedback signals into a small set of proposed changes. Make the M2 mission ("Personal Model — measurable adaptation in tone/timing after 50 conversations") concrete and testable.

## Motivation

Persona today is read-only at runtime. The persona JSON in `~/.human/personas/<name>.json` only changes when the user edits it. This means:

- Learnings during a session don't persist to the persona file.
- "Remember to send Casey a birthday note in advance" lives in commitment store, not persona.
- M2 — measurable adaptation in tone/timing after 50 conversations — has no closed-loop mechanism.

Claude Code's auto-edit of `CLAUDE.md` (visible from v2.0.64, Q4 2025) and LangMem's `update_system_prompt` show the pattern: agent proposes, user confirms (or auto-confirms via policy), persona evolves. We have everything we need except the proposal-and-gate mechanism.

## Prior art

- Claude Code memory autoedit: docs.claude.com/en/code/memory.
- LangMem SDK `update_system_prompt`: langchain-ai.github.io/langgraph/concepts/memory.
- MemRL (arxiv early 2026) and MemEvolve (Dec 2025): RL-based meta-learning for memory write policies.
- Project prior work: `src/persona/feedback.c`, `src/persona/analyzer.c`, `src/agent/self_improve.c` exist; this workstream wires them together.

## Design

### 1. Persona delta as a memory entry

A persona delta is just a memory entry with a special category. No new schema for deltas themselves — they live alongside everything else, get bitemporal treatment from W1, and are erasable via W4.

```c
typedef enum {
    HU_PERSONA_DELTA_OVERLAY,        /* per-channel overlay change */
    HU_PERSONA_DELTA_VOCAB,          /* preferred / avoided vocab */
    HU_PERSONA_DELTA_TRAIT,          /* trait shift */
    HU_PERSONA_DELTA_RULE,           /* communication rule */
    HU_PERSONA_DELTA_PROCEDURAL,     /* system-prompt-level heuristic */
} hu_persona_delta_kind_t;

typedef struct hu_persona_delta {
    hu_persona_delta_kind_t kind;
    const char *target;        /* channel name, trait name, rule scope */
    size_t target_len;
    const char *before;        /* current value or NULL if new */
    size_t before_len;
    const char *after;         /* proposed value */
    size_t after_len;
    const char *justification; /* why proposed — must reference observed evidence */
    size_t justification_len;
    float confidence;
    int64_t proposed_at;
    const char *proposed_by;   /* "persona-evolver", "self_improve", "user" */
    size_t proposed_by_len;
} hu_persona_delta_t;
```

Stored in a new `persona_deltas` table; joined with the persona JSON at load time.

### 2. Three pathways

**a. Inline propose (during turn).** When `src/agent/self_improve.c` detects something it should remember (a correction, a preference, a heuristic), it writes a `hu_persona_delta_t` with status `proposed`. Inline confirmation is offered ("Want me to remember that you prefer 'fwd' over 'forwarded'? y/n") in conversational channels where it doesn't break flow, otherwise queued.

**b. Persona-evolver subagent (weekly).** New module `src/agent/persona_evolver.c`. Runs on `cron-style` schedule, reads the last week of conversations + outcomes + W4 erasure log + W2 quarantine reviews, proposes a small batch (≤ 5) of deltas. Each delta cites evidence ("you corrected me 3 times this week when I called Casey 'Cassandra'").

**c. User-initiated.** `human persona propose --channel=imessage --shorter` opens an interactive review.

### 3. Gating + apply

`human persona deltas` lists pending proposals with evidence. `--apply <id>` accepts; `--reject <id>` rejects. Bulk: `--apply-all-from persona-evolver`.

Auto-apply is **off by default**. A config flag (`persona.auto_apply_high_confidence`) lets advanced users auto-apply deltas with `confidence > 0.9`, but every auto-apply still writes an audit entry the user can revert from `human persona history`.

UI mirror: in W4's memory-view, a "Proposed deltas" tab.

### 4. Procedural memory

The `HU_PERSONA_DELTA_PROCEDURAL` kind is for system-prompt-level heuristics. Examples: "always show provenance receipts to this user," "when discussing finances, ask before drafting." These are stored as deltas but applied at prompt-build time in `src/persona/prompt.c` (the existing prompt builder).

A procedural delta becomes a `## Heuristics` section appended to the system prompt. This is the in-app equivalent of Claude Code editing `CLAUDE.md`.

### 5. Reverting

Every delta has an `undo_id` linking to the previous state. `human persona revert <id>` writes a new delta with the inverse change. The bitemporal layer (W1) preserves history naturally.

### 6. Honest M2 measurement

Adds `eval_suites/persona_adaptation.json` with 20 paired tasks: same prompt, two persona variants (pre-evolution and post-evolution snapshot), blind judge picks which response feels more like the user's voice. Threshold: post-evolution win rate ≥ 60%.

## File map

| File | Role |
|------|------|
| `include/human/persona/delta.h` | New — `hu_persona_delta_t`, kind enum, store API |
| `src/persona/delta.c` | New — schema + CRUD + apply |
| `src/persona/prompt.c` | Extend — read procedural deltas at prompt build |
| `src/agent/self_improve.c` | Extend — emit `hu_persona_delta_t` proposals |
| `include/human/agent/persona_evolver.h` | New |
| `src/agent/persona_evolver.c` | New — weekly cycle, evidence gathering |
| `src/main.c` | `human persona deltas/propose/apply/reject/revert/history` |
| `src/config.c` | New `persona.auto_apply_high_confidence` flag |
| `ui/src/views/persona-deltas-view.ts` | New tab in memory-view |
| `tests/test_persona_delta.c` | New |
| `tests/test_persona_evolver.c` | New |
| `tests/test_self_improve_proposals.c` | New |
| `eval_suites/persona_adaptation.json` | New |

## Test strategy

- Delta CRUD: round-trip; status transitions; undo links.
- Self-improve emits proposal on synthetic correction event; assert delta written.
- Persona-evolver: stub a week of fixture conversations + outcomes; assert ≤ 5 deltas with evidence citations.
- Procedural delta applied: build prompt; assert `## Heuristics` section present.
- Revert: apply, revert, assert state matches pre-apply snapshot.
- Auto-apply gating: confidence below threshold → not auto-applied.
- ASan clean.

## Success criteria

- Persona-adaptation eval: post-evolution win rate ≥ 60% (blind A/B).
- M2 closed loop: after 50 fixture conversations, persona file shows ≥ 3 applied deltas with measurable behavioral change.
- All existing tests pass.
- Binary size delta: < 50 KB.

## Risks

| Risk | Mitigation |
|------|------------|
| Agent proposes deltas user didn't want | Default off auto-apply; every apply visible in `history`; revert is one command |
| Procedural deltas accumulate into a giant prompt | Cap on procedural section length (4 KB); periodic AutoDream consolidation phase merges similar heuristics |
| Drift from user voice over time | Adaptation eval gates merges; quarterly drift detector (mentioned in canvas roadmap) |
| Persona file becomes ungrokkable | Persona file stays canonical; deltas are metadata until user accepts; UI shows clean before/after |

## Open questions

1. Should procedural deltas bypass channel overlays or stack with them? Recommendation: stack — procedural is global default, overlays still win per-channel.
2. Does the persona-evolver run in-binary or as a separate process? Recommendation: in-binary (it's just a scheduled call to `hu_persona_evolver_run` from the daemon's idle loop, like W2's AutoDream).

## References

- Claude Code memory: code.claude.com/docs/en/memory
- LangMem update_system_prompt: langchain-ai.github.io/langgraph
- MemRL: early 2026 paper
