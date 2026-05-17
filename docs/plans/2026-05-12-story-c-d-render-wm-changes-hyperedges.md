# Story C + Story D — Render `wm->recent_changes` and `wm->hyperedges` in the W7 bridge

> **Plan author:** assistant
> **Date:** 2026-05-12
> **Branch:** `story-c-d-w9-render` (off `origin/sprint-4-m2-measurement` @ `9db4ead5`)
> **Worktree:** `/Users/sethford/Documents/human-story-cd`

---

## Surprise finding (informs the entire plan)

The W9 cell survey turned up that **both `wm->recent_changes` and `wm->hyperedges` are already populated** in `hu_world_model_load` (src/agent/world_model.c lines 584–628 and 651–701 respectively). They have full struct definitions, allocation, deduplication, capping, and ownership/free wiring. They are persisted in cache, copied in `hu_world_model_clone`, and freed in `hu_world_model_free`.

**They are simply never rendered into the prompt.** Zero references to either field in `src/agent/world_model_bridge.c`.

This collapses Story C + Story D from "design and populate a new cell" to "add a render section for an already-built cell." Both are surgical bridge edits with no new data infrastructure, no new ownership concerns, no new allocations, and no new tests for population behavior. The existing tests for `recent_changes` and `hyperedges` (P4.2 / P4.3 era) already pin the population side; this PR adds tests for rendering.

## Story C — Render `wm->recent_changes`

### Why

`hu_world_model_load` already derives up to 8 SUPERSEDED / RETRACTED relation rows per snapshot (line 584–628). They carry `relation_id`, `prior_id`, `kind`, `at_ms`, and a mechanical `summary` like `"rel #4 supersedes #7"`. The mechanical summary is useless to an LLM, but the `relation_id` is in `wm->relations` — so the bridge can join locally and produce a readable line like:

```
Recent changes:
- ada worked_at openai (no longer holds)
- alice met_at acme (updated)
```

This restores long-conversation coherence — the agent can know "we just established that Ada moved from OpenAI to Anthropic" without re-querying memory.

### What

In `src/agent/world_model_bridge.c::hu_w7_render_world_model`, between the existing "Recent topics:" block (line 319-326) and the "Communication style:" block (line 327-330), insert a new section:

```c
if (wm->recent_changes_count > 0) {
    ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Recent changes:\n", 16);
    size_t cap = wm->recent_changes_count > 5 ? 5 : wm->recent_changes_count;
    for (size_t i = 0; i < cap; i++) {
        const hu_world_recent_change_t *ch = &wm->recent_changes[i];
        const char *kind = (ch->kind == HU_WORLD_CHANGE_SUPERSEDED)
                               ? "updated" : "no longer holds";
        const hu_graph_relation_t *rel = NULL;
        for (size_t r = 0; r < wm->relations_count; r++) {
            if (wm->relations[r].id == ch->relation_id) {
                rel = &wm->relations[r];
                break;
            }
        }
        if (rel && rel->source_name && rel->target_name) {
            const char *t = hu_relation_type_to_string(rel->type);
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- %s %s %s (%s)\n",
                                   rel->source_name, t ? t : "->", rel->target_name, kind);
        } else {
            ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- %s\n", ch->summary);
        }
    }
}
```

Cap at 5 to keep the section concise (population already caps at 8).

### Failure modes

| Mode | Behavior |
|---|---|
| `wm->recent_changes_count == 0` | Section skipped entirely (no header) |
| `relation_id` not in `wm->relations` | Fall back to mechanical `summary` field |
| `source_name`/`target_name` NULL | Fall back to mechanical `summary` field |
| 6th+ change in same window | Silently truncated (5-cap) |

### Tests

In `tests/test_world_model_bridge.c`:

| Test | Assertion |
|---|---|
| `bridge_render_with_recent_changes_emits_section` | After upserting a relation that supersedes a prior, render output contains `"Recent changes:"` and the new source→target names with `(updated)` |
| `bridge_render_with_no_recent_changes_omits_section` | Fresh contact with no superseded/retracted relations → output does NOT contain `"Recent changes:"` |
| `bridge_render_recent_changes_caps_at_five` | Seed 7 superseded relations → output contains exactly 5 `"- "` lines under `"Recent changes:"` |

## Story D — Render `wm->hyperedges`

### Why

`hu_world_model_load` already pulls up to 16 hyperedges per snapshot (line 641–700) — n-ary facts like `met_at(alice[subject], bob[subject], acme[location])`. They live in `wm->hyperedges[]` with full deep-copied `members` and `provenance`. They are completely invisible to the LLM today.

The W12 LLM planner can query hyperedges via retrieval plans (it knows the `HU_MEM_HYPEREDGE` step kind), but the world-model context block — the foundation of every prompt — never surfaces them. Adding them gives the agent a relational view of the world without requiring a separate retrieval step.

### What

In `src/agent/world_model_bridge.c::hu_w7_render_world_model`, right after the Story C "Recent changes:" block, insert:

```c
if (wm->hyperedges_count > 0) {
    ok = ok && buf_append(alloc, &buf, &blen, &bcap, "Multi-entity facts:\n", 20);
    size_t cap = wm->hyperedges_count > 5 ? 5 : wm->hyperedges_count;
    for (size_t i = 0; i < cap; i++) {
        const hu_hyperedge_t *he = &wm->hyperedges[i];
        if (!he->relation_label[0]) continue;
        ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "- %s: ", he->relation_label);
        for (size_t j = 0; j < he->members_count && j < 6; j++) {
            if (j > 0) ok = ok && buf_append(alloc, &buf, &blen, &bcap, ", ", 2);
            const char *name = NULL;
            for (size_t k = 0; k < wm->entities_count; k++) {
                if (wm->entities[k].id == he->members[j].entity_id) {
                    name = wm->entities[k].name;
                    break;
                }
            }
            if (name) {
                ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "%s[%s]",
                                       name, he->members[j].role);
            } else {
                ok = ok && buf_appendf(alloc, &buf, &blen, &bcap, "ent#%lld[%s]",
                                       (long long)he->members[j].entity_id, he->members[j].role);
            }
        }
        ok = ok && buf_append(alloc, &buf, &blen, &bcap, "\n", 1);
    }
}
```

Cap at 5 hyperedges and 6 members per hyperedge to keep the section bounded.

### Failure modes

| Mode | Behavior |
|---|---|
| `wm->hyperedges_count == 0` | Section skipped entirely |
| `relation_label[0] == '\0'` | Skip that hyperedge (defensive) |
| Member entity_id not in `wm->entities` | Render as `ent#<id>[role]` |
| `members_count == 0` | Renders header line with empty member list — acceptable, matches the underlying state |
| 7+ members on a single hyperedge | Truncated silently at 6 |

### Tests

In `tests/test_world_model_bridge.c`:

| Test | Assertion |
|---|---|
| `bridge_render_with_hyperedges_emits_multi_entity_section` | Seed a 3-member hyperedge `met_at(alice[subject], bob[subject], acme[location])`; render → output contains `"Multi-entity facts:"`, `"met_at:"`, `"alice[subject]"`, `"acme[location]"` |
| `bridge_render_with_no_hyperedges_omits_section` | Fresh contact, no hyperedges → output does NOT contain `"Multi-entity facts:"` |
| `bridge_render_hyperedges_caps_at_five` | Seed 7 hyperedges → output contains exactly 5 `"- "` lines under `"Multi-entity facts:"` |
| `bridge_render_hyperedge_member_not_in_entities_renders_id` | Seed a hyperedge with a member id not in `wm->entities` → output contains `"ent#"` placeholder |

## Combined acceptance criteria

- [ ] Full `human_tests` fleet: 10232 / all pass + new tests, 0 ASan errors
- [ ] 7 new tests (3 for Story C, 4 for Story D) all PASS
- [ ] No new allocations / no new data structures / no new public APIs — purely render-side
- [ ] PR opened against `origin/sprint-4-m2-measurement`

## Why Stories C and D land together

Both are pure render-side additions to the same function with the same buf-append idiom and identical lifetime semantics. They reuse the same shared infrastructure (`hu_world_model_load`, `hu_world_model_free`, `buf_appendf`) and add to the same `hu_w7_render_world_model` blob. Splitting them into two PRs would double the review surface for what is essentially one cross-cutting "surface more of the W9 snapshot in the prompt" theme. The four tests for D and three tests for C are independent so a failure on one does not block the other from landing.

## Out of scope (separate sprint)

- **`self_model` cell construction.** `hu_world_model_t` has a `self_model` field but no dedicated cell-build pass; merge_persona seeds `name`, `confidence_in_self`, `focused_topics` from existing data. A real agent self-model (capabilities, recent own behavior, mood drift) is a multi-day sprint with a new data model. Punt.
- **Improving the mechanical `summary` field of `hu_world_recent_change_t`.** Story C works around it by joining `wm->relations`. Rewriting the population-side `snprintf("rel #X supersedes #Y", ...)` to produce a human-readable summary would simplify the bridge but expands the blast radius into the existing P4.3 population code and its tests. Defer.
- **The 3 deferred CI failures** (`ui-e2e` axe, `visual-regression` snapshot drift, `build-ios` "Overview" tab). Not in the W9-wiring story. Address in a separate UI-maintenance sprint.
