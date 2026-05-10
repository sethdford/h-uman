---
title: "W9 — World Model: hu_world_model_t per-contact unified snapshot"
created: 2026-05-10
status: proposed
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: medium
scope: include/human/agent/, src/agent/, src/persona/
---

# W9 — World Model

## Goal

Replace four parallel calls (`hu_persona_load`, `hu_graph_neighbors`, `hu_emotion_state_load`, `hu_contact_get`, `hu_persona_delta_list`) with one: `hu_world_model_load(memory, contact, &out)`. The result is a single, lazily computed, cached snapshot the planner consults every turn. Adds two new cells the planner has never had access to: **theory-of-mind** (what does the user believe about *me*?) and **negative memory** (what should I never say or do here?).

## Motivation

`src/agent/context.c` builds the prompt by calling persona, graph, emotional state, contact, persona deltas, and case recall in sequence. Each adds latency, has its own error model, and yields a different shape. The W11 inline Self-RAG and the W12 planner each need this same set of inputs — without W9, both would re-implement the same fan-out, doubling code.

Theory-of-mind and negative memory exist as data (persona, deltas, persona avoid_vocab) but are not assembled into a single artifact the agent can read. This is the gap between "knows facts about you" and "models you."

## Prior art

- Letta `MemoryModule` aggregate state.
- Pi (Inflection) — explicit user-belief-about-me model.
- Mem0g — unified per-user state object.
- `src/memory/personal_model.c` (existing in repo as M2 mission anchor) — half-built; W9 finishes it.

## Design

### `hu_world_model_t`

```c
/* include/human/agent/world_model.h */

typedef struct hu_active_goal {
    char text[160];
    float salience;        /* 0..1, how prominent right now */
    int64_t expressed_at;
    int64_t expires_at;    /* 0 = persistent */
} hu_active_goal_t;

typedef struct hu_negative_memory {
    char text[200];        /* "do not mention X around Y" */
    char scope[64];        /* topic / contact / channel */
    char reason[120];
    hu_belief_t belief;    /* W8: how sure are we this is a hard rule */
} hu_negative_memory_t;

typedef struct hu_theory_of_mind {
    /* What the user appears to believe about us: capabilities, persona, values. */
    char user_thinks_we_are[160];
    char user_expects_we_can[200];
    char user_expects_we_cannot[200];
    hu_belief_t confidence;
} hu_theory_of_mind_t;

typedef struct hu_world_model {
    /* Identity */
    char contact_id[64];

    /* Persona snapshot (read-only copy from persona.c) */
    hu_persona_t persona;

    /* Recent entities + 1-hop relations */
    hu_graph_entity_t *entities;
    size_t entities_count;
    hu_graph_relation_t *relations;
    size_t relations_count;

    /* Emotional state (latest) */
    char dominant_emotion[32];
    float arousal;
    float valence;

    /* Active goals (top-K by salience) */
    hu_active_goal_t goals[8];
    size_t goals_count;

    /* Negative memory ("don't say X" facts) */
    hu_negative_memory_t *negatives;
    size_t negatives_count;

    /* Theory-of-mind */
    hu_theory_of_mind_t tom;

    /* Recent topics (last 10) */
    char recent_topics[10][64];
    size_t recent_topics_count;

    /* Cache metadata */
    int64_t built_at;
    int64_t valid_until;
} hu_world_model_t;

hu_error_t hu_world_model_load(hu_memory_t *m, hu_allocator_t *alloc,
                                const char *contact_id, size_t cid_len,
                                hu_world_model_t **out);
void hu_world_model_free(hu_allocator_t *alloc, hu_world_model_t *wm);
hu_error_t hu_world_model_invalidate(hu_memory_t *m, const char *contact_id, size_t cid_len);
```

### Cache

Per-process LRU keyed by `contact_id`, max 32 entries. TTL 60s. Invalidated by:
- Any write to `hu_memory_t` for that contact (writers call `_invalidate` after success).
- Explicit `hu_world_model_invalidate`.

### Negative memory storage

New table:
```sql
CREATE TABLE IF NOT EXISTS negative_memory (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL DEFAULT '',
    text TEXT NOT NULL,
    scope TEXT NOT NULL,
    reason TEXT,
    confidence_mean REAL NOT NULL DEFAULT 1.0,
    confidence_variance REAL NOT NULL DEFAULT 0.0,
    created_at INTEGER NOT NULL
);
```

### Theory-of-mind storage

Computed lazily from existing data (persona expectations, emotional reactions, deltas) by a deterministic synthesizer in `world_model.c`. No new schema.

## Phases

1. Author `world_model.h` + `world_model.c` deterministic synthesizer (no LLM yet).
2. Author `negative_memory.c` (CRUD).
3. Wire `hu_world_model_load` into `src/agent/context.c` (replace 4 calls with 1).
4. Add LRU cache + invalidation hooks on every `hu_memory_write`.
5. Add ToM synthesizer (heuristic over persona + recent deltas).
6. Adversarial tests.

## Test plan

- `test_w9_world_model_round_trip`: loads, writes, invalidates, reloads — fresh data appears.
- `test_w9_world_model_cache_hit_within_ttl`: repeated load within 60s returns cached.
- `test_w9_world_model_invalidates_on_write`: any `hu_memory_write` for contact → next load is fresh.
- `test_w9_negative_memory_blocks_response_path`: when a negative memory matches the draft topic, the verifier (W11) flags it.
- `test_w9_tom_synthesizer_grounded_in_persona`: ToM fields are reproducible from inputs.
- `test_w9_adversarial_negative_memory_poisoning`: attacker tries to inject "do not warn user about X" → write_trust catches it before W9 can read it.
- `test_w9_world_model_p99_latency_under_5ms`: benchmark.

## Success metric

- Single `hu_world_model_load` p99 ≤ 5 ms (vs ~12 ms for the 4 separate calls today).
- ToM synthesis stable across ≥10 reload cycles (deterministic).
- ≥3 measurable behavior changes in agent output when negative memory is set (proven by A/B test).
- Binary size delta ≤ +50 KB.

## Risks

| Risk | Mitigation |
|------|------------|
| Cache staleness leaks across contacts | Per-contact key; explicit eviction on every write |
| ToM synthesizer encodes false confidence | All ToM fields carry `hu_belief_t`; UI surfaces uncertainty |
| Negative memory exploited by attacker to silence the agent | Negative memory writes go through W1 write_trust; high bar from open channels |

## Out of scope

- Active disambiguation ("am I right that you prefer X?"). That's a future workstream.
- Multimodal world model (image of user's room as part of ToM).
- Cross-contact world model (group conversations).

## Binary size budget: +50 KB.
