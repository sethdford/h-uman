---
title: "W1 — Bitemporal Foundation: edges, write-time conflict resolver, write-trust score, LoCoMo skeleton"
created: 2026-05-10
status: proposed
parent: 2026-05-10-memory-roadmap-overview.md
risk: high
scope: src/memory/, include/human/memory/, src/agent/, tests/, eval_suites/
---

# W1 — Bitemporal Foundation

## Goal

Make every fact in h-uman's graph carry honest temporal semantics: when it was true vs when we noticed, with mutations preserved as supersession rather than overwrite. Wire the existing conflict detector into the per-write path so contradictions resolve immediately. Add a write-time trust score so the system fails closed against memory-poisoning attacks (MINJA / Echoleak / Unit 42 indirect prompt injection). Stand up a thin LoCoMo eval skeleton so we can measure W1's lift before moving on.

## Motivation

Graph today (`include/human/memory/graph.h`):

```c
typedef struct hu_graph_relation {
    int64_t id;
    int64_t source_id;
    int64_t target_id;
    hu_relation_type_t type;
    float weight;
    int64_t first_seen;     /* monotemporal: ingest only */
    int64_t last_seen;      /* monotemporal: ingest only */
    char *context;
    size_t context_len;
} hu_graph_relation_t;
```

`first_seen`/`last_seen` track "when did h-uman observe this." There is no field for "when was the fact true in the world." A user who switches jobs in March and tells us in May is ambiguous: did they `WORKS_AT` the new place from March or May? Today we can't tell.

`hu_graph_detect_conflict` and `hu_graph_reconsolidate` already exist (`src/memory/graph.c`). `rg` shows their callers are limited to `src/daemon.c` (periodic cycle) and `src/memory/graph.c` (internal). They don't fire on the synchronous write path triggered by an agent turn that extracts a new fact. Result: contradictions accumulate until the next daemon cycle and may mislead retrieval in between.

There is no write-side trust score. Any user input, retrieved web content, or tool output that is summarized and persisted is a poisoning vector (Mintmcp 2025; MINJA achieved 95% injection rate on naive stores).

## Prior art

- Zep / Graphiti — bitemporal edges with `event_time` (when true) + `ingestion_time` (when noticed), preserved-not-overwritten on update (arxiv 2501.13956).
- Mem0 / Mem0g — two-phase write: extract → conflict-detect → resolve. 26% accuracy lift, 91% lower P95 latency (arxiv 2504.19413).
- A-MemGuard — composite trust scoring as poisoning defense; standalone LLM-only detection misses 66%.
- LoCoMo (Snap Research) — 35-session, 300-turn, 9000-token conversations; multi-hop and temporal Q categories.

## Design

### 1. Bitemporal edge schema

Add to `hu_graph_relation_t`:

```c
typedef struct hu_graph_relation {
    int64_t id;
    int64_t source_id;
    int64_t target_id;
    hu_relation_type_t type;
    float weight;
    int64_t first_seen;     /* INGEST: when h-uman first observed (kept) */
    int64_t last_seen;      /* INGEST: most recent observation (kept) */
    int64_t event_start;    /* EVENT: when true in the world (NEW; nullable→0) */
    int64_t event_end;      /* EVENT: when ceased being true (NEW; 0 = still true) */
    float confidence;       /* 0.0-1.0 (NEW; default 1.0 for legacy rows) */
    int64_t supersedes_id;  /* old edge replaced by this one (NEW; 0 = none) */
    char *context;
    size_t context_len;
    char *provenance;       /* source URI/channel/turn-id (NEW; nullable) */
    size_t provenance_len;
} hu_graph_relation_t;
```

SQLite table mirrors:

```sql
ALTER TABLE relations ADD COLUMN event_start INTEGER DEFAULT 0;
ALTER TABLE relations ADD COLUMN event_end INTEGER DEFAULT 0;
ALTER TABLE relations ADD COLUMN confidence REAL DEFAULT 1.0;
ALTER TABLE relations ADD COLUMN supersedes_id INTEGER DEFAULT 0;
ALTER TABLE relations ADD COLUMN provenance TEXT;
CREATE INDEX IF NOT EXISTS idx_rel_event_window ON relations(event_start, event_end);
CREATE INDEX IF NOT EXISTS idx_rel_supersedes ON relations(supersedes_id);
```

Same schema for `entities` table where bitemporal makes sense (e.g. an org's name change). Entities also get `event_start`/`event_end`/`confidence`/`supersedes_id`/`provenance`.

Schema version bumps from N → N+1. Old binaries refuse to open new DBs (already supported by `schema_version` row).

### 2. Bitemporal write path

New public API:

```c
hu_error_t hu_graph_upsert_relation_ex(
    hu_graph_t *g, const char *contact_id, size_t contact_id_len,
    int64_t source_id, int64_t target_id,
    hu_relation_type_t type, float weight,
    int64_t event_start,         /* 0 = unknown / now() */
    int64_t event_end,           /* 0 = still true */
    float confidence,
    const char *context, size_t context_len,
    const char *provenance, size_t provenance_len);
```

Existing `hu_graph_upsert_relation` becomes a thin wrapper that forwards `event_start = now()`, `event_end = 0`, `confidence = 1.0`, `provenance = NULL`.

Existing rows get `event_start = first_seen`, `event_end = NULL` on first read after migration.

### 3. Write-time conflict resolver

New module `src/memory/conflict_resolver.c` + `include/human/memory/conflict_resolver.h`.

Public API:

```c
typedef enum {
    HU_CONFLICT_NONE,
    HU_CONFLICT_SUPERSEDE,   /* new fact replaces old; old preserved with event_end set */
    HU_CONFLICT_BRANCH,      /* both true; e.g. multiple jobs */
    HU_CONFLICT_FLAG,        /* unclear; surface to user */
} hu_conflict_resolution_t;

hu_error_t hu_conflict_resolve_relation(
    hu_graph_t *g, hu_allocator_t *alloc,
    const hu_graph_relation_t *new_rel,
    hu_conflict_resolution_t *out_decision,
    int64_t *out_superseded_id);  /* 0 if no supersession */
```

Resolution policy (deterministic, no LLM at write time — keeps P95 low):

- New relation type is single-valued (`WORKS_AT`, `LIVES_IN`) AND existing relation has `event_end == 0` AND `source_id` matches → SUPERSEDE: set old's `event_end = new.event_start`; new gets `supersedes_id = old.id`.
- New relation type is multi-valued (`KNOWS`, `INTERESTED_IN`, `RELATED_TO`) → BRANCH: both kept.
- New `confidence < 0.5` while existing `confidence ≥ 0.8` → FLAG: store new with low confidence, mark for review.
- Otherwise → NONE: just upsert.

LLM-driven resolver (optional, gated behind `HU_ENABLE_LLM_CONFLICT`) lives in W2's AutoDream cycle, not on the synchronous write path.

Wired in `hu_graph_upsert_relation_ex` so every write hits the resolver. `daemon.c`'s periodic call becomes a fallback for relations written before W1 landed.

### 4. Write-time trust score

New module `src/memory/write_trust.c` + `include/human/memory/write_trust.h`.

```c
typedef struct {
    const char *channel;        /* "user_direct", "tool_output", "web_fetch", … */
    size_t channel_len;
    float source_trust;         /* 0.0-1.0; user_direct=1.0, web_fetch=0.4, … */
    int64_t observed_at;
    bool from_indirect_prompt;  /* true if extracted from email/web/tool */
    float novelty;              /* 0.0=well-known, 1.0=never-seen-before */
    float drift_signal;         /* observed agent-output drift in last hour */
} hu_write_trust_input_t;

float hu_write_trust_score(const hu_write_trust_input_t *in);

#define HU_WRITE_TRUST_QUARANTINE_THRESHOLD 0.3f
```

Score formula (linear, deterministic):

```
score = 0.5 * source_trust
      + 0.2 * (1 - novelty)
      + 0.2 * (1 - drift_signal)
      + 0.1 * (from_indirect_prompt ? 0 : 1)
```

Below `HU_WRITE_TRUST_QUARANTINE_THRESHOLD`: write goes to a separate `quarantine_relations` table, not the main `relations` table. Quarantined writes never appear in retrieval until the AutoDream cycle (W2) reviews them. User can also review via W4's memory-view UI.

### 5. LoCoMo eval skeleton

New eval suite `eval_suites/locomo_temporal.json` with 20 hand-authored temporal-Q tasks (fewer than upstream LoCoMo's full set; enough to detect the W1 lift).

New test `tests/test_locomo_eval.c` runs the suite under `HU_IS_TEST` with a fixture conversation history and asserts:

- Pre-W1 baseline score (captured before W1's PR by running on `main`)
- Post-W1 expected score (≥ baseline + 5 pts on temporal subset)

Suite format follows `eval_suites/MANIFEST.md` conventions. Judge: `human_likeness` profile (default).

W6 expands this skeleton to the full LoCoMo + LongMemEval; W1 just lands the harness.

## File map

| File | Role |
|------|------|
| `include/human/memory/graph.h` | Add new fields to `hu_graph_relation_t` and `hu_graph_entity_t`; declare `hu_graph_upsert_relation_ex` |
| `src/memory/graph.c` | Schema migration, new upsert API, bitemporal-aware traversal |
| `include/human/memory/conflict_resolver.h` | New header — public API |
| `src/memory/conflict_resolver.c` | New module — resolution policy + write-path wiring |
| `include/human/memory/write_trust.h` | New header — public API |
| `src/memory/write_trust.c` | New module — trust score formula + quarantine table |
| `src/daemon.c` | Replace periodic-only conflict-detect with "review quarantined writes" pass |
| `tests/test_graph_bitemporal.c` | New — schema migration round-trip; event-window queries; supersession |
| `tests/test_conflict_resolver.c` | New — every resolution branch + LLM-fallback path |
| `tests/test_write_trust.c` | New — score formula; quarantine round-trip; MINJA-style poisoning rejected |
| `tests/test_locomo_eval.c` | New — runs new eval suite; baseline + post-W1 thresholds |
| `eval_suites/locomo_temporal.json` | New — 20-task temporal-Q suite |
| `eval_suites/MANIFEST.md` | Document new suite |
| `CMakeLists.txt` | Wire new sources |
| `tests/test_main.c` | Wire `run_*_tests()` |
| `docs/CONCEPT_INDEX.md` | Add bitemporal / conflict-resolver / write-trust concept entries |

## Test strategy

- Schema round-trip: open old DB, run migration, re-read, assert fields populated correctly.
- Event-window queries: insert 3 facts with overlapping windows; ask "what was true between t1 and t2"; assert correct subset.
- Supersession: insert `WORKS_AT(Acme)` with `event_end=0`, then insert `WORKS_AT(Beta)` with `event_start=t2`; assert old gets `event_end=t2`, new gets `supersedes_id=old.id`.
- Conflict resolver: at least one test per `hu_conflict_resolution_t` enum value.
- Trust score: synthetic MINJA-style indirect-prompt fact; assert quarantined; assert recall ignores quarantine.
- LoCoMo skeleton: locked baseline score in fixture; threshold lift after W1.
- ASan: zero leaks on every test.

## Success criteria

- All 8,500+ existing tests still pass.
- `tests/test_graph_bitemporal.c`, `tests/test_conflict_resolver.c`, `tests/test_write_trust.c`, `tests/test_locomo_eval.c` all green.
- LoCoMo temporal-Q subset: ≥ baseline + 5 points.
- MINJA-shaped synthetic poisoning test: attack success rate < 10%.
- Binary size delta: < 30 KB (W1 is foundation; budget per `docs/standards/engineering/performance.md`).
- Zero ASan errors.

## Risks

| Risk | Mitigation |
|------|------------|
| Schema migration breaks existing user DBs | Round-trip test on real-shaped fixture; idempotent migration; `schema_version` gate; reversible down-migration script |
| Write-time resolver adds latency to every fact extraction | Deterministic-only at write time; LLM resolver deferred to W2 nightly cycle |
| Trust score false positives quarantine legitimate writes | Threshold tunable; W4 UI surfaces quarantine for user review; default threshold conservatively low |
| LoCoMo skeleton's 20 tasks don't generalize | W6 expands to full LoCoMo + LongMemEval before declaring SOTA-comparable |

## Open questions

1. **Should `event_start` default to "ingest time" or "unknown" (NULL)?** Recommendation: default to ingest time (matches today's semantics) but allow explicit NULL via the new `_ex` API. Decision needed before schema lands.
2. **Where does `from_indirect_prompt` get set?** Recommendation: in the agent turn that extracts the fact, not in `write_trust.c`. Caller responsibility, default false.
3. **Should quarantined writes count toward `mention_count`?** Recommendation: no — they shouldn't influence retrieval ranking. Confirm before implementation.

## References

- Zep / Graphiti: arxiv 2501.13956
- Mem0 / Mem0g: arxiv 2504.19413
- Memory poisoning: Unit 42 (Palo Alto Networks 2025); MINJA Dong et al. 2025; A-MemGuard 2025
- LoCoMo: snap-research.github.io/locomo
- Project prior work: `docs/plans/2026-03-08-better-than-human.md`, `docs/plans/2026-03-10-human-fidelity-phase3-superhuman-memory.md`
