---
title: W7 type collision cleanup
status: deferred (workaround in production)
owner: memory subsystem
created: 2026-05-10
---

# Background

`include/human/memory.h` (legacy v1 vector memory) and
`include/human/memory/memory.h` (W7 dispatching facade, layer 1 of the v2
roadmap) both define `typedef struct hu_memory hu_memory_t;` with the same
struct tag. Including both in one translation unit triggers a redefinition
error.

This blocks the cleanest wire-up paths for W7-dependent features:

- `agent_turn.c` already includes `human/memory.h` transitively via
  `human/agent.h`.
- `human/agent/world_model.h`, `human/agent/self_rag.h`, etc. all include
  `human/memory/memory.h`.
- Any TU that needs both is impossible to compile.

# Workaround in production

FIX 2 introduced `agent->verifier_graph` (a raw `hu_graph_t *` instead of
the W7 facade) so the response verifier could run without paying the
include cost.

FIX 12 generalized the workaround: a thin "bridge" translation unit
(`src/agent/world_model_bridge.c`) is the ONE place W7 + W9 + W11 headers
are visible. Everyone else interacts with W7 features through:

- An opaque tagged forward declaration: `struct hu_w7_facade;`
- A wrapper header (`include/human/agent/world_model_bridge.h`) that
  exposes only allocator / graph types.
- Bridge functions that render W7 / W9 / W11 outputs into prompt-ready
  text or scalar enums (`hu_w11_outcome_t`).

The agent struct holds `struct hu_w7_facade *w7_facade`. The opaque tag is
unique, so neither the legacy `hu_memory_t` nor the W7 `hu_memory_t` is
visible in the same TU as `agent.h`.

This works. The bridge TU is small (~200 LOC), and adding another bridged
operation is a 30-line wrapper. If you find yourself adding more than 4-5
operations, consider promoting the bridge to a real "agent memory layer"
abstraction.

# What a real fix would look like

Three viable paths, in increasing order of risk:

## Option A — Rename the W7 type (least churn)

```c
// include/human/memory/memory.h
typedef struct hu_memory_v2 hu_memory_v2_t;  // new name
```

Rename the struct + every `hu_memory_*` API to `hu_memory_v2_*`. Update
all callers (currently ~60 references in src/ + tests). Delete the bridge.

**Risk:** medium. ~60 mechanical renames; CI will catch missed sites.
**Estimated effort:** 4-6 hours.
**Blocker:** the W7 spec says "the v1 backend wraps existing graph APIs",
so the names live in a deliberately neutral namespace; renaming reveals
that v2 isn't really "memory" — it's a dispatching facade. The rename
should probably go to `hu_memory_facade_t` to match the docs.

## Option B — Rename the legacy type

```c
// include/human/memory.h
typedef struct hu_memory_v1 hu_memory_v1_t;
```

Rename every legacy `hu_memory_*` symbol to `hu_memory_v1_*`. Larger
blast radius (~200 references) but matches the W7 design intent: the
facade IS memory, the vector store is one backend.

**Risk:** high. The legacy struct is part of `hu_agent_t` directly, so any
header transitively pulls it in. This is a real refactor that will touch
hundreds of files.
**Estimated effort:** 1-2 weeks if done carefully.

## Option C — Delete the legacy (if W7 fully covers v1)

Walk every legacy `hu_memory_*` call site, decide whether the W7 facade
covers the equivalent operation, replace each call. When the legacy struct
has zero references, delete it.

**Risk:** very high. The legacy struct carries embedder bindings,
session-store hooks, and many other hooks the W7 facade does NOT yet
expose. This is a multi-month migration that has to land in lockstep with
W7 backend work for embedded, vector, and session state.
**Estimated effort:** 2-3 months.

# Decision

Stay with the bridge workaround (FIX 2 / FIX 12 pattern). When v2 is the
primary surface for new features, revisit Option A.

# Migration triggers

Pick up the rename when ANY of these become true:

- More than 6 bridged operations exist (mounting maintenance burden).
- A new W7 backend (W10 neural memory tier, W15 crypto envelope) needs to
  be wired into agent_turn.c directly.
- The legacy embedder / session_store / kv_cache_manager are themselves
  refactored to a new vtable — at that point we can rename in one sweep.

# Tracking

| Date | Bridged op count | Notes |
|------|-----------------:|-------|
| 2026-05-10 | 2 | FIX 12 bridge: world model render + self-RAG verify |
