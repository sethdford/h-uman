---
title: "W7 — Memory Facade: hu_memory_facade_t read/write/erase surface"
created: 2026-05-10
status: proposed
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: high
scope: include/human/memory/, src/memory/, every consumer of graph/persona/cross_edges/cases/quarantine/deltas
---

# W7 — Memory Facade

**Implementation naming (Phase 0, 2026-05-10):** the dispatch type is `hu_memory_facade_t` with `hu_memory_facade_read` / `hu_memory_facade_write` / … in `include/human/memory/memory.h`. Legacy vector/chat memory remains `hu_memory_t` in `include/human/memory.h`. Design snippets below still show the pre-rename `hu_memory_*` spellings where not yet edited; treat them as pseudocode for the facade API.

**Phase 1 bypass inventory:** [`2026-05-10-w7-phase1-bypass-inventory.md`](2026-05-10-w7-phase1-bypass-inventory.md).

## Goal

Introduce one and only one read/write/erase surface for structured memory kinds. Every consumer (planner, prompt builder, verifier, channels, daemon) goes through **`hu_memory_facade_t`**. Every existing backend (graph, vector, persona deltas, cross_edges, cases, quarantine, neural memory) becomes a vtable implementation behind it. v2 cannot stay clean without this — every later workstream consumes memory and would otherwise reach into 7 different APIs.

## Motivation

Today, `src/agent/context.c` calls `hu_graph_neighbors`, `hu_persona_load`, `hu_emotion_state_load`, `hu_contact_get`, `hu_case_recall`, and `hu_persona_delta_list` directly. Each adds latency, each fails differently, each has its own error model. Some subsystems still reach the backing SQLite connection through **`hu_graph_sqlite_connection`** for operations the facade does not yet expose; the internal `hu_graph__db_handle` name exists only inside `src/memory/graph.c`. The planner has no single seam to swap a backend. Result: every new memory shape (W10 KV-cache, W10 multimodal, W13 LoRA adapters, W15 encrypted blobs) would need to thread through every call site.

## Prior art

- LangChain `BaseRetriever` — single-method abstraction, but read-only.
- Letta `MemoryModule` — write-aware, but Python and stateful.
- Anthropic prompt-caching API — points at the right shape: opaque blobs keyed by prefix hash.

The h-uman precedent is `hu_provider_t`: one vtable with `chat`, `supports_native_tools`, `get_name`, `deinit`; backends slot in via factory. W7 mirrors this for memory.

## Design

### Public API

```c
/* include/human/memory/memory.h */

typedef enum hu_memory_kind {
    HU_MEM_ENTITY = 0,
    HU_MEM_RELATION,
    HU_MEM_HYPEREDGE,        /* W8 */
    HU_MEM_PERSONA_DELTA,    /* v1 W5 */
    HU_MEM_CASE,             /* v1 W3 */
    HU_MEM_CROSS_EDGE,       /* v1 W3 */
    HU_MEM_QUARANTINE,       /* v1 W1 */
    HU_MEM_KV_CACHE,         /* W10 */
    HU_MEM_REASONING_TRACE,  /* W10 */
    HU_MEM_BLOB,             /* W10 multimodal */
    HU_MEM_KIND_MAX
} hu_memory_kind_t;

typedef struct hu_memory_query {
    hu_memory_kind_t kind;
    const char *contact_id;
    size_t contact_id_len;
    /* Kind-specific payload (taggged union below). */
    union {
        struct { int64_t entity_id; size_t hops; size_t limit; } neighbors;
        struct { const char *name; size_t name_len; } by_name;
        struct { int64_t from_ts; int64_t to_ts; size_t limit; } window;
        struct { const char *prompt_hash; size_t hash_len; const char *model_version; } kv;
        struct { const char *goal_verb; size_t goal_len;
                 const int64_t *anchors; size_t anchors_len; size_t limit; } cases;
        /* etc. */
    } as;
} hu_memory_query_t;

typedef struct hu_memory_record {
    hu_memory_kind_t kind;
    int64_t id;
    /* Always populated for safety: */
    hu_belief_t belief;          /* W8 */
    char *provenance;            /* nullable */
    size_t provenance_len;
    int64_t event_start;
    int64_t event_end;
    /* Kind-specific payload via opaque blob. Caller casts via kind. */
    void *payload;
    size_t payload_len;
} hu_memory_record_t;

typedef struct hu_memory hu_memory_t;

typedef struct hu_memory_vtable {
    const char *name;
    hu_error_t (*read)(void *ctx, const hu_memory_query_t *q, hu_allocator_t *alloc,
                       hu_memory_record_t **out, size_t *out_count);
    hu_error_t (*write)(void *ctx, const hu_memory_record_t *rec);
    hu_error_t (*erase)(void *ctx, hu_memory_kind_t kind, int64_t id);
    hu_error_t (*erase_by_provenance)(void *ctx, const char *substring, size_t len);
    void (*records_free)(void *ctx, hu_allocator_t *alloc, hu_memory_record_t *r, size_t n);
    void (*deinit)(void *ctx);
} hu_memory_vtable_t;

hu_error_t hu_memory_open(hu_allocator_t *alloc, hu_graph_t *graph, hu_memory_t **out);
hu_error_t hu_memory_register_backend(hu_memory_t *m, hu_memory_kind_t kind,
                                      hu_memory_vtable_t *vt, void *ctx);
hu_error_t hu_memory_read(hu_memory_t *m, const hu_memory_query_t *q, hu_allocator_t *alloc,
                          hu_memory_record_t **out, size_t *out_count);
hu_error_t hu_memory_write(hu_memory_t *m, const hu_memory_record_t *rec);
hu_error_t hu_memory_erase(hu_memory_t *m, hu_memory_kind_t kind, int64_t id);
hu_error_t hu_memory_erase_by_provenance(hu_memory_t *m, const char *substring, size_t len);
void hu_memory_close(hu_memory_t *m, hu_allocator_t *alloc);
```

### Backend registration

The default open() registers a `hu_memory_v1_backend_t` for every kind that v1 supports (entity, relation, persona_delta, case, cross_edge, quarantine). Later workstreams replace specific kinds:
- W8 swaps RELATION + HYPEREDGE backend.
- W10 registers KV_CACHE, REASONING_TRACE, BLOB.
- W15 wraps the entire facade with an encryption decorator.

### Schema

A small metadata table:
```sql
CREATE TABLE IF NOT EXISTS memory_facade_routes (
    kind INTEGER PRIMARY KEY,
    backend_name TEXT NOT NULL DEFAULT 'v1',
    registered_at INTEGER NOT NULL
);
```
Auto-populated on first open with `v1` for every existing kind.

## Phases

1. **Phase 1: facade types + dispatcher.** Author `include/human/memory/memory.h`, `src/memory/memory.c`. No backends yet — return `HU_ERR_NOT_IMPLEMENTED` for every kind. Tests assert dispatcher routing only.

2. **Phase 2: v1 backend.** Author `src/memory/memory_v1_backend.c` that wraps existing graph + persona + cross_edges + cases + quarantine functions. One translation layer; no rewrites. Tests assert round-trip through facade matches direct calls.

3. **Phase 3: migrate consumers.** Mechanical pass over every direct caller of graph/persona/etc. Produce one PR-shaped commit per subsystem (`src/agent/`, `src/persona/`, `src/feeds/`, channels). Tests verify no behavior change.

4. **Phase 4: deprecate raw access.** Add CMake check that fails if any non-backend file directly includes `human/memory/graph.h` (allow-listed: `memory_v1_backend.c`, tests). Old `hu_graph_*` functions kept as deprecated thin wrappers for one release; logs warning at first call.

## Test plan

- `tests/test_w7_facade_routing.c` — every kind dispatches to its registered backend.
- `tests/test_w7_v1_backend_roundtrip.c` — write/read/erase via facade matches direct v1 calls bit-for-bit.
- `tests/test_w7_unknown_backend_returns_not_supported.c` — calling an unregistered kind returns `HU_ERR_NOT_SUPPORTED`, not crash.
- `tests/test_w7_concurrent_register_and_read.c` — adversarial: registering a backend while another thread reads doesn't tear.
- `tests/test_w7_v1_to_v2_migration_idempotent.c` — reopening with new schema doesn't double-route.
- `tests/test_w7_facade_e2e_persona_call.c` — adversarial: prompt builder uses only the facade; replacing a backend with a stub changes the prompt deterministically.

## Success metric

- ≥80% of v1 direct memory call sites migrated through the facade in a single PR.
- p99 read latency ≤ 1.10× direct-call baseline (facade is dispatch-only, must not add real cost).
- Binary size delta ≤ +30 KB.

## Risks

| Risk | Mitigation |
|------|------------|
| Migrating 60+ call sites in one PR breaks something subtle | Keep v1 functions as wrappers; tests call both paths and compare |
| Vtable indirection adds latency on hot paths | Inline `hu_memory_read` for the v1 backend via `static inline` shim; benchmark before merge |
| "Junk drawer" risk if union grows unbounded | Cap `hu_memory_query_t` union at 64 bytes; new kinds need a struct, not a union add |

## Out of scope

- Replacing SQLite as storage. (W7 is a facade; backends still own storage choice.)
- Cross-process memory (shared memory between agent + daemon).
- Async / streaming reads. (Synchronous only; streaming added later if needed.)

## Binary size budget: +30 KB.
