---
title: Modeled-Person Normalization — Phases 4 & 5 (sequenced chips)
description: SQL→repository migration + layer relocation for the human-characteristic surface, scoped as safe chips against the existing DDD Phase-3 program (not a parallel scheme).
status: draft
created: 2026-05-29
---

# Modeled-Person Normalization — Phases 4 & 5

> Phases 1–3 are DONE (layering ratchet + module-shape standard; rel_dynamics
> dedup+relocate; somatic/narrative_self/attachment tests). Plan:
> `~/.claude/plans/curried-discovering-treehouse.md`. Phases 4–5 are the heavy
> lifts. They are intentionally captured here as **chips** rather than executed
> in one pass because (a) Phase 4 IS the existing DDD Phase-3 repository program
> (`docs/plans/2026-05-29-ddd-bounded-contexts/phase-3-memory-query-interface.md`),
> which has a **concurrent writer on this branch** — a parallel migration would
> collide; and (b) each relocation is cross-context churn that interacts with
> the agent-core-boundary + sqlite-includer ratchets. Sequence, don't sprint.

## Verified facts (2026-05-29, by grep)

**Phase 4 — person-modules that DIRECTLY `#include <sqlite3.h>` (6):**
`src/persona/mood.c`, `src/context/emotional_state.c`, `src/agent/theory_of_mind.c`,
`src/agent/self_model.c`, `src/memory/emotional_moments.c`,
`src/memory/emotional_residue.c`. (evolved_opinions, growth_narrative,
commitment_store, channel_trust, pressure_history, personal_model do NOT take a
raw handle — already string-SQL or repo-backed; not Phase-4 targets.)

**Phase 5 — human-characteristic modules in the "wrong" dir:**
- `src/context/{mood,emotional_state,humor,rel_dynamics}.c` → perception → `cognition/`
- `src/agent/{theory_of_mind,self_model}.c` → perception → `cognition/`
- `src/agent/belief_update.c` → decision → `behavior/`
- `src/agent/growth_narrative.c` → expression-of-identity-arc → `persona/`
- `src/agent/intrinsic_drive.c` → drive/perception → `cognition/` (review)
- `src/intelligence/{world_model,trust}.c` → review (world vs person boundary)

## Phase 4 — SQL behind repositories (chips against DDD Phase 3)

Each of the 6 raw-sqlite person-modules becomes one chip following the
established recipe (`src/memory/repos/README.md`; exemplar
`src/memory/repos/boundary_repo_sqlite.c`):

1. Define `hu_<aggregate>_repo_t` vtable + factory in
   `include/human/memory/<aggregate>_repo.h`.
2. Move all SQL into `src/memory/repos/<aggregate>_repo_sqlite.c` (the only
   place `<sqlite3.h>` is legal).
3. Rewire the module to depend on the repo interface; drop its
   `#include <sqlite3.h>` and `hu_sqlite_memory_get_db()` calls.
4. **Lower `sqlite-includer-ratchet` BASELINE by 1** (currently 109) per chip —
   the ratchet tightening is the proof the moat advanced.
5. Verify: full suite green + 0 ASan; the module's suite green.

Sequence simplest first (fewest call sites): `emotional_residue` /
`emotional_moments` (memory-local) → `mood` → `emotional_state` →
`self_model` → `theory_of_mind` (most call sites). **Coordinate with the
concurrent DDD writer**: check `git log`/the ratchet baseline before each chip;
stage by name; never rebase this branch.

## Phase 5 — relocate into the layers (AFTER Phase 4)

Do Phase 4 first so the modules are repo-backed (no `<sqlite3.h>` to drag across
a dir boundary, keeping the sqlite ratchet honest). Then per module, the
mechanical move (same recipe proven in Phase 2 for rel_dynamics):

1. `git mv src/<olddir>/<m>.c src/<newlayer>/<m>.c` +
   `git mv include/human/<olddir>/<m>.h include/human/<newlayer>/<m>.h`.
2. Update the module's own `#include` + header guard (`HU_<NEWLAYER>_*`).
3. Update every caller's include path (`grep -rl 'human/<olddir>/<m>.h'`).
4. Update `CMakeLists.txt` source path.
5. **Build + full suite + ALL ratchets** — especially the Phase-1 layering
   ratchet (the new home must not introduce a forbidden cross-layer include)
   and agent-core-boundary (moving person-modules OUT of `agent/` REDUCES
   orchestration coupling — a win, but re-measure).
6. **Verify `git show HEAD:<file>`** after committing — the PostToolUse
   formatter re-touches files after `git add` and can silently drop content
   edits (cost a broken intermediate commit in Phase 2; fix forward, never
   amend this shared branch).

Relocation order (lowest-caller-count / lowest-risk first):
`context/humor` → `context/rel_dynamics` → `context/mood` →
`context/emotional_state` → `agent/self_model` → `agent/growth_narrative` →
`agent/theory_of_mind` → `agent/belief_update` → `agent/intrinsic_drive`.
Defer `intelligence/{world_model,trust}` pending a world-vs-person boundary
decision (these may legitimately belong to the Learning-Loop context, not
Modeled Person — verify before moving).

## Done criteria

- Phase 4: all 6 raw-sqlite person-modules repo-backed; sqlite-includer
  baseline lowered by 6 (→ ~103) with each drop locked.
- Phase 5: zero human-characteristic `.c` outside `persona/cognition/behavior`
  (except the two aggregate roots + `memory/repos/` persistence); layering
  ratchet still 0/0/0/0; agent-core-boundary baselines unchanged or lower.
- Each chip: full suite green + 0 ASan + committed-blob verified.

## Why chips, not a sprint

The repo migrations are multi-week across the whole codebase (the DDD plan
scopes ~24 remaining repos); the person-subset here is 6 of them. Forcing all
6 + 9 relocations in one pass on a branch with a concurrent writer is how
verified work gets lost (see `ddd_isolated_worktree_commit_between` memory).
One chip per slice, committed + verified between, is the only safe cadence.
