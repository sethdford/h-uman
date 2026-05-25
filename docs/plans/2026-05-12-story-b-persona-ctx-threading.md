---
title: Story B — Thread `hu_persona_context_t` through agent_turn / agent_stream / memory_loader
status: active
created: 2026-05-12
last_audit: 2026-05-25
---

# Story B — Thread `hu_persona_context_t` through agent_turn / agent_stream / memory_loader

> **Plan author:** assistant
> **Date:** 2026-05-12
> **Sprint:** post-sprint-4 follow-up to Story A
> **Branch:** `story-b-persona-ctx-threading` (off `origin/sprint-4-m2-measurement` @ `9db4ead5`)
> **Worktree:** `/Users/sethford/Documents/human-story-b`

---

## Problem

`hu_world_model_merge_persona` (defined in `src/agent/world_model.c`) takes the cached world-model snapshot and folds in:

- **`tom.interaction_style`** — channel-aware pragmatics digest (formality, directness, face-saving, vulnerability tier)
- **`tom.user_thinks_we_are`** — persona-grounded ToM updated with persona identity
- Channel-overlay clauses (per `hu_persona_overlay_t` for the active channel)
- Delta-driven ToM adjustments from the recent persona delta queue

This merge is gated on a non-NULL `hu_persona_context_t *persona_ctx` argument to `hu_w7_render_world_model`. Today, the **only** production call site that passes a non-NULL `persona_ctx` is `daemon.c:7439` (the iMessage batch path). The three main per-turn render call sites all pass `NULL`:

| Call site | File | Line |
|---|---|---|
| Main per-turn render | `src/agent/agent_turn.c` | 3489 |
| Streaming render | `src/agent/agent_stream.c` | 1008 |
| Memory-loader supplement | `src/agent/memory_loader.c` | 386 |

Net effect: on the production agent path, `interaction_style` is **populated by the persona system but then never reaches the LLM prompt** because the merge is skipped. The world-model block goes out with whatever default ToM the W7 facade produced — typically just `negatives → user_expects_we_cannot`.

This is the #1 single-callsite leverage win identified by the W9 cell wiring survey. Story A wired the verifier to honor `[hard]/[soft]/[confirm]/[policy]` *after* drafting; Story B threads the channel-aware pragmatics digest *into* drafting so the model is steered correctly *before* generating.

## Goal

After this story:

1. `hu_w7_render_world_model` is called with a non-NULL `hu_persona_context_t` on the three main paths whenever `agent->persona` is set.
2. The persona context carries:
   - `persona = agent->persona`
   - `channel = agent->active_channel` (length `active_channel_len`)
   - `delta_limit = 8` (mirrors `daemon.c`)
3. A new helper `hu_memory_loader_set_persona_context()` is added so the memory loader has a stable API to receive the persona context (rather than reaching into globals).
4. A new test pins the behavior: building a persona with a channel overlay that sets `interaction_style = "be terse and direct"` causes the rendered world-model context to contain that string when rendered via the main path (post-fix), and **does not** when rendered with `persona_ctx = NULL` (pre-fix behavior preserved as a regression guard).

## Non-goals

- We are NOT changing the persona overlay schema, the merge logic, or the channel-aware pragmatics digest. Those are existing, tested infrastructure (sprint-2b W9 work).
- We are NOT touching the daemon batch path (already correct).
- We are NOT touching test files that intentionally call with `persona_ctx = NULL` to exercise the back-compat path; those are correct.

## Design

### API delta (1 new function)

```c
/* Bind a persona context for W9 graph rendering. When set AND the loader
 * subsequently calls hu_w7_render_world_model, the bridge merges persona
 * overlay + identity + delta-driven ToM into the snapshot.
 *
 * Pass NULL to clear (back-compat). The caller retains ownership of the
 * pointed-to persona and channel-name buffer; the loader stores the
 * pointer/lengths only. */
void hu_memory_loader_set_persona_context(hu_memory_loader_t *loader,
                                          const hu_persona_context_t *ctx);
```

`hu_memory_loader_t` gains one optional pointer field:

```c
const hu_persona_context_t *persona_ctx; /* optional; threaded to hu_w7_render_world_model */
```

`memory_loader.c`'s call to `hu_w7_render_world_model` (line 386) changes its last argument from `NULL` to `loader->persona_ctx`.

### Call-site changes

In `agent_turn.c` (around line 3482, right before the `hu_w7_render_world_model` call):

```c
hu_persona_context_t pctx;
const hu_persona_context_t *pctx_p = NULL;
if (agent->persona) {
    pctx.persona = agent->persona;
    pctx.channel = agent->active_channel;
    pctx.channel_len = agent->active_channel_len;
    pctx.delta_limit = 8; /* mirrors daemon.c batch path */
    pctx_p = &pctx;
}
hu_w7_render_world_model(/* ... */, &agent->personal_model, pctx_p);
```

Identical pattern in `agent_stream.c` around line 1003.

In `agent_turn.c` at the loader-init site (line 1382), add one line after `hu_memory_loader_set_personal_model`:

```c
hu_persona_context_t pctx_for_loader;
if (agent->persona) {
    pctx_for_loader.persona = agent->persona;
    pctx_for_loader.channel = agent->active_channel;
    pctx_for_loader.channel_len = agent->active_channel_len;
    pctx_for_loader.delta_limit = 8;
    hu_memory_loader_set_persona_context(&loader, &pctx_for_loader);
}
```

Same in `agent_stream.c`'s loader-init site (line 399).

### Lifetime / safety

- `pctx` is a stack local that outlives the synchronous `hu_w7_render_world_model` call. The bridge copies the pointer-fields it needs into the snapshot; it does **not** retain `pctx` beyond the call. (Verified by reading `hu_world_model_merge_persona` in `src/agent/world_model.c` — it dereferences `persona_ctx` only within the function body.)
- `agent->active_channel` is stable for the duration of the turn (set before `hu_agent_turn` and not mutated until after).
- `delta_limit = 8` is a soft cap. The merge function safely handles any persona with fewer deltas.

## Failure modes

| Mode | Mitigation |
|---|---|
| Persona has no overlay for the active channel | `hu_persona_overlay_t` lookup returns NULL → merge fills nothing extra → empty `interaction_style` → bridge skips the "Interaction style:" line. Back-compat preserved. |
| Persona has overlay but `interaction_style` is empty | Same as above. |
| `agent->persona == NULL` (no persona configured) | `pctx_p` stays NULL → bridge skips persona merge → back-compat preserved. |
| `agent->active_channel == NULL` (no active channel set) | `pctx.channel = NULL`, `channel_len = 0`. Bridge falls back to identity-only merge per the docstring at `world_model_bridge.h:36-43`. |
| Lifetime: stack pctx outlives bridge call | Bridge is synchronous; pctx is a stack local in the same function. Verified by inspection. |

## Tests

New test file: `tests/test_persona_ctx_threading.c` (or extends an existing W9 bridge test).

| Test | Assertion |
|---|---|
| `test_render_with_persona_ctx_emits_interaction_style` | Build persona with channel overlay → render with `persona_ctx` non-NULL → output contains the overlay's `interaction_style` string |
| `test_render_without_persona_ctx_omits_interaction_style` | Same persona, same channel → render with `persona_ctx = NULL` → output does NOT contain the overlay string (regression guard for back-compat) |
| `test_loader_set_persona_context_threads_into_render` | Build loader, call `hu_memory_loader_set_persona_context`, then `hu_memory_loader_load` → loader's internal `hu_w7_render_world_model` call passes the ctx → output contains overlay string |
| `test_render_with_persona_no_channel_overlay_falls_back_identity_only` | Persona has no overlay for channel "discord" → render with channel="discord" → no crash, no overlay text, identity merge still happens |
| `test_render_with_persona_ctx_null_persona_skips_merge` | `persona_ctx` non-NULL but `pctx.persona = NULL` → bridge treats as NULL ctx (sanity guard) |

## Companion CI fix in same PR

`hu-directive-telemetry-tile` is not registered in `ui/src/catalog/catalog.html` or `ui/src/catalog/components.test.ts`. The component exists (`ui/src/components/hu-directive-telemetry-tile.ts`) and has a test file (`hu-directive-telemetry-tile.test.ts`), but the registration gap breaks the `ui` job on every PR against `sprint-4-m2-measurement`. Adding two small entries fixes it.

## Out of scope (separate sprint)

- `build-ios` regression (`HumaniOSFleetUITests.swift:160` — "Primary shell should expose Overview"). Native iOS, base-branch regression.
- `ui-e2e` axe accessibility failures. Needs UI accessibility review.
- `visual-regression` Playwright snapshot drift. May need a regeneration pass after the sprint-2b style commits.

## Acceptance criteria

- [ ] Full `human_tests` fleet: 10228+/all pass, 0 ASan errors (no regression)
- [ ] 5 new tests in `tests/test_persona_ctx_threading.c` all PASS
- [ ] Manual smoke: build, run `human` with a persona configured + a starter overlay, observe the world-model block in the prompt now contains the channel-appropriate `interaction_style` line. (Test-mode tooling will be sufficient — no live model call needed.)
- [ ] `ui` CI job is GREEN on this branch (companion fix lands)
- [ ] PR opened against `origin/sprint-4-m2-measurement`
