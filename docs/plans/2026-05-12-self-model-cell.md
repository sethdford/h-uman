# Plan — self_model cell: render-now (Story E) + expand-later (Story F)

> **Plan author:** assistant
> **Date:** 2026-05-12
> **Status:** design doc only — implementation deferred until PR #61 lands
> **Tracks the last item from:** the W9 cell wiring survey (Stories A/B/C/D/E)

---

## Survey finding that shapes this plan

The codebase **already has** a `hu_self_model_t` struct with five fields, and `hu_world_model_merge_persona` populates **all five** of them every turn:

```c
// include/human/agent/world_model.h:177-183
typedef struct hu_self_model {
    char name[64];                  // populated from persona->name
    char focused_topics[200];       // top-3 of wm->recent_topics, ';'-joined
    char recent_drift_kind[32];     // latest persona delta kind ("BOUNDARY", "TONE", ...)
    char recent_drift_value[160];   // delta value text
    float confidence_in_self;       // [0, 1] from persona identity completeness
} hu_self_model_t;
```

Bridge references to `wm->self_model.*`: **zero**. Same situation as `recent_changes` and `hyperedges` before PR #61.

This collapses "build a self_model cell" into two stories:

| Story | Scope | Effort | Ships |
|---|---|---|---|
| **E** | Render the 5 existing fields | ½ day, pure render-side | Next PR |
| **F** | Add `capabilities` + agent's own mood + multi-turn drift history | 3–5 days, new data model + populators + tests | Separate sprint |

Story E unlocks immediate value (the planner can see "we're tracking these 3 topics, we just shifted tone last turn, confidence 0.7"). Story F is the real "the agent has a self-image" upgrade.

---

## Story E — Render `wm->self_model` in the W7 bridge

### Why ship E alone first

1. **Zero blast radius.** Data is already populated and lifetime-managed (`self_model` is inline POD, no heap, no clones, no frees).
2. **Immediate measurable impact.** Today the planner has access to `name`/`focused_topics`/`drift_kind`/`drift_value`/`confidence_in_self` via the C struct but the *LLM* never sees them. Rendering them puts them into every system prompt.
3. **Follows the exact pattern that succeeded on Stories C+D.** One surgical render section, 3-4 tests, no new APIs.

### What to render

A new section between "Recent changes:" (Story C) and "Communication style:" (existing):

```
Self model:
- I am: <name>
- Tracking: <focused_topics>
- Most recent shift: <recent_drift_kind> — <recent_drift_value>
- Self-confidence: <confidence_in_self formatted "high" / "medium" / "low">
```

Each line is conditional: omit the line if its field is empty (zero), so a fresh contact won't get all four bullets. Confidence is rendered as a bucketed adjective (≥0.7 = high, ≥0.4 = medium, ≥0.1 = low, otherwise omit) so the LLM doesn't try to reason about float precision.

### Implementation

Single edit in `src/agent/world_model_bridge.c::hu_w7_render_world_model`, immediately after the Story D "Multi-entity facts:" block:

```c
/* Story E (sprint-4 follow-up) — render wm->self_model.
 *
 * hu_world_model_merge_persona already populates all five fields every
 * turn. Render them so the LLM can see "we are tracking X topics, we
 * just shifted tone last turn, confidence Y". Conditional per-field so
 * a fresh contact doesn't get a section of blanks. */
bool self_signal = wm->self_model.name[0]
    || wm->self_model.focused_topics[0]
    || wm->self_model.recent_drift_kind[0]
    || wm->self_model.confidence_in_self >= 0.1f;
if (self_signal) {
    ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Self model:\n", 12);
    if (wm->self_model.name[0])
        ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- I am: %s\n",
                               wm->self_model.name);
    if (wm->self_model.focused_topics[0])
        ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- Tracking: %s\n",
                               wm->self_model.focused_topics);
    if (wm->self_model.recent_drift_kind[0]) {
        if (wm->self_model.recent_drift_value[0])
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap,
                                   "- Most recent shift: %s — %s\n",
                                   wm->self_model.recent_drift_kind,
                                   wm->self_model.recent_drift_value);
        else
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap,
                                   "- Most recent shift: %s\n",
                                   wm->self_model.recent_drift_kind);
    }
    const char *bucket = NULL;
    if (wm->self_model.confidence_in_self >= 0.7f) bucket = "high";
    else if (wm->self_model.confidence_in_self >= 0.4f) bucket = "medium";
    else if (wm->self_model.confidence_in_self >= 0.1f) bucket = "low";
    if (bucket)
        ok = ok && buf_appendf(alloc, &buf, &blen, &bcap,
                               "- Self-confidence: %s\n", bucket);
}
```

### Tests (4 new in `tests/test_world_model_bridge.c`)

| Test | Assertion |
|---|---|
| `bridge_render_with_self_model_emits_section` | Persona present + non-empty recent_topics + applied delta → output contains `"Self model:"`, `"I am: "`, `"Tracking: "`, `"Most recent shift: "`, `"Self-confidence: "` |
| `bridge_render_with_empty_self_model_omits_section` | Fresh contact, no persona, no topics, no drift → output does NOT contain `"Self model:"` |
| `bridge_render_self_confidence_bucket_low_medium_high` | Three persona snapshots with different identity-completeness → confidence bucket renders correctly |
| `bridge_render_self_model_partial_fields_renders_only_present` | Persona name but no topics, no drift → output has `"I am: "` but no `"Tracking: "` or `"Most recent shift: "` |

### Acceptance criteria

- [ ] All 4 new tests pass
- [ ] Full `human_tests` fleet still passes (10238 → 10242)
- [ ] No new allocations / no new APIs / no new data structures
- [ ] Build clean (MinSizeRel + Debug + LTO)

### Failure modes handled

| Mode | Behavior |
|---|---|
| `self_model` is all zero | Section skipped |
| Drift has kind but no value | Renders kind only ("Most recent shift: BOUNDARY") |
| Confidence < 0.1 | Confidence line omitted (avoid noise) |
| Persona present but ToM merge ran twice | Idempotent (same string), no double-render |

---

## Story F — Expand the self_model data model

### What's missing today

The user asked for "capabilities, recent own behavior, mood drift." Matching to existing fields:

| Asked | Today | Gap |
|---|---|---|
| capabilities | not on `self_model`; `wm->tom.user_expects_we_can` is the USER's perception | **NEW field needed** — agent's own list of usable tools/skills/channels |
| recent own behavior | `recent_drift_kind` + `recent_drift_value` = latest single delta | **EXPAND** — last N drifts in a ring buffer (4-8 slots) |
| mood drift | `wm->valence/arousal/dominant_emotion` = USER's emotional state | **NEW field needed** — agent's own emotional state (separate from user) |

### Proposed Phase F.1 — capabilities cell

Add to `hu_self_model_t`:

```c
char capabilities[6][32];   // top-6 enabled tools/skills by recent-use frequency
size_t capabilities_count;
```

Populator: walk `hu_agent_t::tools` list, filter by `is_configured()`, sort by last_used timestamp, copy top 6 names into the slab. Owner: `hu_world_model_merge_self_capabilities(wm, agent)` — new function, called from the same merge_persona codepath.

Render section addition:
```
- Capabilities I have: shell, web_search, memory_query
```

Wire: agent_turn.c + agent_stream.c call new function before `hu_w7_render_world_model`. ~30 LOC + 3 tests.

### Proposed Phase F.2 — drift history ring buffer

Replace single `recent_drift_kind` / `recent_drift_value` with a ring:

```c
typedef struct hu_self_drift_entry {
    char kind[32];
    char value[160];
    int64_t at_ms;
} hu_self_drift_entry_t;

// ... in hu_self_model_t:
hu_self_drift_entry_t recent_drifts[8];
size_t recent_drifts_count;
```

Keep `recent_drift_kind` and `recent_drift_value` as back-compat aliases for `recent_drifts[0]` so existing callers don't break.

Populator: persona-delta application code (already runs every turn) appends into the ring with rollover. Render section:
```
- How I've been shifting: TONE (yesterday), BOUNDARY (3 days ago), TONE (last week)
```

~40 LOC + 3 tests.

### Proposed Phase F.3 — agent's own emotional state

New field separate from the existing `wm->valence/arousal/dominant_emotion` (which stays as user-side):

```c
typedef struct hu_self_mood {
    float valence;          // [-1, 1] — own positivity/negativity (separate from user's)
    float arousal;           // [0, 1] — own energy level
    char dominant_emotion[32]; // "calm", "focused", "tense", ...
    int64_t at_ms;
} hu_self_mood_t;
// ... in hu_self_model_t:
hu_self_mood_t own_mood;
```

Populator candidates (research, pick one in F.3):
- **A — sentiment of own recent drafts.** Run the response on a tiny sentiment classifier (already in `src/agent/`); aggregate over the last 5 turns.
- **B — derive from drift history.** Multiple BOUNDARY/TONE shifts in 24h → "uncertain"; consistent style → "calm".
- **C — manually opted-in via persona overlay.** Persona declares `agent_baseline_mood: "warm"`; world model layers turn-by-turn deviations.

Recommend **B** (zero new infrastructure, uses data we already have, deterministic). C is a stretch goal.

~60 LOC + 5 tests.

### Story F as a whole

- Total scope: ~130 LOC + 11 tests across 3 phases.
- Risk: medium — touches the merge_persona codepath, but each phase is independently testable and rollbackable.
- Ships as **three separate PRs** (F.1 / F.2 / F.3) so each is independently reviewable.

---

## Recommendation

**Ship Story E in the next PR** — it's the same successful pattern as C+D, half-day scope, immediate impact. Stop there until F.1 is properly scoped against the actual `hu_tool_t` / `hu_channel_t` registry.

Then **Story F over a multi-day sprint** with its own three-PR breakdown:
1. F.1 — capabilities
2. F.2 — drift history
3. F.3 — agent's own mood

Story F should NOT block. The world model is already valuable without it; we ship Story E and validate the value of the rendered self-model in real conversations before expanding the data model speculatively.

## Out of scope (separate sprints)

- Any agent-side LLM fine-tuning to internalize the self_model (M3 territory).
- Cross-conversation self-model drift tracking (different from per-turn).
- Persona evolution from self_model trajectory (an M2.x feedback loop).
