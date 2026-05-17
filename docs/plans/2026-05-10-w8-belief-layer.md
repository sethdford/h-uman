---
title: "W8 — Belief Layer: hu_belief_t posteriors, semantic conflict via LLM-judge, hyperedges"
created: 2026-05-10
status: complete
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: high
scope: include/human/memory/, src/memory/, every consumer of `confidence: float`
last_audit: 2026-05-17
---

# W8 — Belief Layer

## Goal

Stop treating beliefs as scalar floats. Replace every `confidence: float` field with `hu_belief_t` (mean, variance, provenance). Add an LLM-judge backend so paraphrased contradictions are caught (v1 deterministic resolver only catches exact key matches). Add hyperedges so n-ary facts ("Alice met Bob at Acme on Friday about funding") are stored as one edge.

## Motivation

`hu_graph_relation_t.confidence` is a single float. Two problems:
1. We can't represent uncertainty about uncertainty. "I'm 0.8 sure but with high variance" and "I'm 0.8 sure with low variance" look identical.
2. v1's conflict resolver does string-key matching. "Alice works at Acme" and "Alice is employed by Acme Inc." are the same fact paraphrased; v1 sees them as independent.

`hu_relation_type_t` is binary (source, target, type). "Alice met Bob at Acme on Friday about funding" requires 4 separate edges that lose the conjunction. Multi-hop queries ("when did Alice and Bob last collaborate on funding?") have to re-stitch on the fly.

## Prior art

- Zep / Graphiti — Bayesian update on belief edges with explicit prior + posterior.
- HippoRAG / GraphRAG — n-ary hyperedges as first-class.
- Mem0g LLM-judge — embedding similarity + LLM tie-break for paraphrase detection.

## Design

### `hu_belief_t`

```c
/* include/human/memory/belief.h */

typedef struct hu_provenance_atom {
    char source[64];    /* "imessage", "user-explicit", "feed-web", ... */
    int64_t observed_at;
    float weight;       /* this source's contribution to the posterior */
} hu_provenance_atom_t;

typedef struct hu_belief {
    float mean;                             /* point estimate, 0..1 */
    float variance;                         /* uncertainty about the mean */
    hu_provenance_atom_t prov[4];           /* up to 4 supporting sources */
    uint8_t prov_count;
    int64_t last_updated;
} hu_belief_t;

hu_belief_t hu_belief_init(float mean, const char *source, int64_t now);
hu_belief_t hu_belief_update(const hu_belief_t *prior, float observation,
                              const char *source, int64_t now);
hu_belief_t hu_belief_combine(const hu_belief_t *a, const hu_belief_t *b);
bool hu_belief_significantly_disagrees(const hu_belief_t *a, const hu_belief_t *b,
                                        float sigma_threshold);
```

Update rule: a Beta-distribution posterior approximated by tracking (mean, variance) and rolling a Welford-style online update. Variance shrinks with consistent evidence and grows on contradiction.

### Hyperedges

```c
/* include/human/memory/hyperedge.h */

typedef struct hu_hyperedge_member {
    int64_t entity_id;
    char role[32];     /* "subject", "object", "location", "time", "topic" */
} hu_hyperedge_member_t;

typedef struct hu_hyperedge {
    int64_t id;
    char relation_label[64];                /* "met_at", "discussed", ... */
    hu_hyperedge_member_t *members;
    size_t members_count;
    hu_belief_t belief;
    int64_t event_start;
    int64_t event_end;
    char *provenance;
} hu_hyperedge_t;

hu_error_t hu_hyperedge_upsert(hu_memory_t *m, const char *contact_id, size_t cid_len,
                               const hu_hyperedge_t *he, int64_t *out_id);
hu_error_t hu_hyperedge_query_by_member(hu_memory_t *m, int64_t entity_id,
                                         hu_hyperedge_t **out, size_t *out_count);
```

### Semantic conflict detector

```c
/* include/human/memory/semantic_conflict.h */

typedef struct hu_semantic_judge_vtable {
    /* Returns true if `existing` and `candidate` describe the same fact, paraphrased. */
    bool (*same_fact)(void *ctx, const char *existing, const char *candidate);
    /* Returns true if they actively contradict. */
    bool (*contradict)(void *ctx, const char *existing, const char *candidate);
    void (*deinit)(void *ctx);
} hu_semantic_judge_vtable_t;

typedef struct hu_semantic_judge {
    hu_semantic_judge_vtable_t *vt;
    void *ctx;
} hu_semantic_judge_t;

hu_error_t hu_semantic_judge_embedding(hu_provider_t *embedder, float threshold,
                                        hu_semantic_judge_t *out);
hu_error_t hu_semantic_judge_llm(hu_provider_t *judge_provider, hu_semantic_judge_t *out);
hu_error_t hu_semantic_judge_heuristic(hu_semantic_judge_t *out); /* fallback for tests */
```

The conflict resolver from v1 W1 gains a hook: when the deterministic key match returns `HU_CONFLICT_NONE`, run the semantic judge. If it says `same_fact`, treat as supersession; if `contradict`, treat as flag.

## Schema

```sql
ALTER TABLE relations ADD COLUMN confidence_mean REAL DEFAULT 1.0;
ALTER TABLE relations ADD COLUMN confidence_variance REAL DEFAULT 0.0;
UPDATE relations SET confidence_mean = confidence WHERE confidence_mean IS NULL;
ALTER TABLE relations DROP COLUMN confidence; /* via SQLite 3.35+ ALTER */

CREATE TABLE IF NOT EXISTS hyperedges (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL DEFAULT '',
    relation_label TEXT NOT NULL,
    confidence_mean REAL NOT NULL DEFAULT 1.0,
    confidence_variance REAL NOT NULL DEFAULT 0.0,
    event_start INTEGER NOT NULL DEFAULT 0,
    event_end INTEGER NOT NULL DEFAULT 0,
    provenance TEXT,
    created_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS hyperedge_members (
    hyperedge_id INTEGER NOT NULL REFERENCES hyperedges(id),
    entity_id INTEGER NOT NULL,
    role TEXT NOT NULL,
    PRIMARY KEY (hyperedge_id, entity_id, role)
);
```

## Phases

1. Author `belief.h` + `belief.c` with deterministic update math + tests.
2. Migrate `confidence` column repo-wide (sed-then-review pass; rebuild).
3. Author `hyperedge.h` + `hyperedge.c` + W7 backend.
4. Author `semantic_conflict.h` + 3 backends (heuristic, embedding, LLM).
5. Wire semantic judge into v1 conflict resolver as fallback layer.
6. Adversarial tests.

## Test plan

- `test_w8_belief_update_converges`: 100 corroborating observations push mean → 1.0, variance → 0.
- `test_w8_belief_diverges_on_contradiction`: alternating observations grow variance.
- `test_w8_hyperedge_query_by_any_member`: 5-member hyperedge findable from any of the 5.
- `test_w8_semantic_judge_paraphrase_detected`: "works at Acme" ≡ "employed by Acme Inc" via heuristic.
- `test_w8_adversarial_belief_poisoning_grows_variance`: attacker provides 50 conflicting observations → variance crosses threshold → quarantine.
- `test_w8_legacy_confidence_migrates_to_mean_zero_variance`: round-trip on v1 DB.

## Success metric

- Paraphrase-conflict recall on annotated 100-pair suite: +30% vs v1 deterministic resolver.
- Hyperedge query latency: < 5 ms p99 for 1k-hyperedge graphs.
- Binary size delta ≤ +60 KB.

## Risks

| Risk | Mitigation |
|------|------------|
| Migrating `confidence` column breaks tests across the repo | Keep `confidence` as a generated alias (SQLite VIEW) for one release; tests pass with no changes |
| LLM-judge adds round-trip latency | Only invoked when deterministic resolver returns NONE; capped at 1 call per write; cached on prompt-hash |
| Hyperedges get abused as "every relation is a 5-way edge" | Style guide + lint: prefer binary edges unless ≥3 entities are inherently bound |

## Out of scope

- Probabilistic logic programming. (Posterior tracking only.)
- Federated belief aggregation across multiple users.
- Temporal conditional probability (P(X | Y observed at t)).

## Binary size budget: +60 KB.
