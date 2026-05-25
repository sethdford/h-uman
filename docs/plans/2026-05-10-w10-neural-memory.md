---
title: "W10 — Neural Memory Tier: KV-cache reuse, reasoning-trace memory, multimodal blobs"
created: 2026-05-10
status: deferred
parent: 2026-05-10-memory-v2-roadmap-overview.md
risk: medium
scope: include/human/memory/, src/memory/, src/providers/, gated behind HU_ENABLE_NEURAL_MEMORY
last_audit: 2026-05-25
---

# W10 — Neural Memory Tier

## Goal

Stop re-encoding the same context every turn. Persist three new memory kinds — **KV-cache** (compressed activations for past prompt prefixes), **reasoning-trace** (chain-of-thought for past plans), **multimodal blob** (image / audio / video bytes) — as W7 backends. Gated behind `HU_ENABLE_NEURAL_MEMORY` so the v1 deployment surface is unchanged for users without acceleration.

## Motivation

Every turn re-encodes the system prompt + persona + recent graph. Anthropic's prompt caching shows 3-10x latency improvement when this is reused. h-uman has zero of it today. Worse, the agent re-derives the same chain-of-thought across turns ("ok let me think about Alice's relationships again..."). Reasoning-trace memory eliminates the repeat.

Multimodal: `src/memory/multimodal_index.c` exists but is unfinished. Image/audio bytes have nowhere to live as first-class memory entries.

## Prior art

- Anthropic prompt caching API.
- Memory³ / RETRO — KV-cache as memory.
- MemoryLLM, MELODI — compressed-activation memory.
- Compressive Transformer — fixed-window summary KV.

## Design

### Public API (W7 backend extensions)

Three new kinds in `hu_memory_kind_t`: `HU_MEM_KV_CACHE`, `HU_MEM_REASONING_TRACE`, `HU_MEM_BLOB`.

```c
/* include/human/memory/neural_memory.h */

typedef struct hu_kv_cache_entry {
    char prompt_hash[64];      /* sha256 hex of the prompt prefix */
    char model_version[64];    /* invalidates when model upgrades */
    int64_t prompt_token_count;
    void *blob;                /* opaque KV bytes; provider-specific */
    size_t blob_len;
    int64_t created_at;
} hu_kv_cache_entry_t;

typedef struct hu_reasoning_trace {
    int64_t id;
    char goal_verb[64];
    int64_t *anchor_entity_ids;
    size_t anchors_count;
    char *cot_text;            /* the chain-of-thought */
    size_t cot_len;
    char *outcome;
    int64_t recorded_at;
    hu_belief_t belief;        /* W8: how confident the trace was correct */
} hu_reasoning_trace_t;

typedef struct hu_memory_blob {
    int64_t id;
    char mime_type[64];        /* "image/png", "audio/wav", ... */
    void *bytes;
    size_t bytes_len;
    char *caption;             /* lazy text description, nullable */
    size_t caption_len;
    int64_t created_at;
} hu_memory_blob_t;

hu_error_t hu_kv_cache_get(hu_memory_t *m, const char *prompt_hash, const char *model_version,
                           hu_allocator_t *alloc, hu_kv_cache_entry_t **out);
hu_error_t hu_kv_cache_put(hu_memory_t *m, const hu_kv_cache_entry_t *entry);
hu_error_t hu_kv_cache_invalidate_for_model(hu_memory_t *m, const char *model_version);

hu_error_t hu_reasoning_trace_record(hu_memory_t *m, const char *contact_id, size_t cid_len,
                                      const hu_reasoning_trace_t *trace, int64_t *out_id);
hu_error_t hu_reasoning_trace_recall(hu_memory_t *m, hu_allocator_t *alloc,
                                      const char *contact_id, size_t cid_len,
                                      const char *goal_verb, size_t goal_len,
                                      const int64_t *anchors, size_t anchors_count,
                                      size_t limit, hu_reasoning_trace_t **out, size_t *out_count);

hu_error_t hu_memory_blob_put(hu_memory_t *m, const char *contact_id, size_t cid_len,
                               const hu_memory_blob_t *blob, int64_t *out_id);
hu_error_t hu_memory_blob_get(hu_memory_t *m, hu_allocator_t *alloc,
                               int64_t blob_id, hu_memory_blob_t **out);
```

### Schema

```sql
CREATE TABLE IF NOT EXISTS neural_kv_cache (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    prompt_hash TEXT NOT NULL,
    model_version TEXT NOT NULL,
    prompt_token_count INTEGER NOT NULL,
    blob BLOB NOT NULL,
    created_at INTEGER NOT NULL,
    UNIQUE(prompt_hash, model_version)
);

CREATE TABLE IF NOT EXISTS neural_reasoning_traces (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL DEFAULT '',
    goal_verb TEXT NOT NULL,
    anchors TEXT NOT NULL DEFAULT '',
    cot_text TEXT NOT NULL,
    outcome TEXT,
    confidence_mean REAL NOT NULL DEFAULT 1.0,
    confidence_variance REAL NOT NULL DEFAULT 0.0,
    recorded_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_traces_goal ON neural_reasoning_traces(contact_id, goal_verb);

CREATE TABLE IF NOT EXISTS neural_blobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    contact_id TEXT NOT NULL DEFAULT '',
    mime_type TEXT NOT NULL,
    bytes BLOB NOT NULL,
    caption TEXT,
    created_at INTEGER NOT NULL
);
```

### Provider integration

`hu_provider_t` gains an optional method:
```c
hu_error_t (*chat_with_kv_cache)(void *ctx, const hu_chat_request_t *req,
                                  hu_kv_cache_entry_t *cache /* in/out */,
                                  hu_chat_response_t *resp);
```
Providers that don't support KV reuse leave it NULL; the memory tier silently skips them.

### Eviction

W14 sleep scheduler runs eviction:
- KV-cache entries older than 7 days OR whose `model_version` differs from current → drop.
- Reasoning traces capped at 1000 per contact (LRU on `recorded_at`).
- Blobs capped at 100 MB total per user; oldest evicted on overflow.

## Phases

1. Schema + DDL.
2. KV-cache backend + tests.
3. Reasoning-trace backend + tests.
4. Multimodal blob backend + tests.
5. Provider extension method (optional, providers opt in).
6. Eviction hooks (consumed by W14 scheduler).

## Test plan

- `test_w10_kv_cache_round_trip`.
- `test_w10_kv_cache_invalidates_on_model_version_change`.
- `test_w10_reasoning_trace_recall_by_goal_and_anchors`.
- `test_w10_blob_round_trip_image_bytes`.
- `test_w10_adversarial_oversized_blob_rejected`: 200 MB blob → returns `HU_ERR_TOO_LARGE`.
- `test_w10_adversarial_kv_cache_poisoning_via_hash_collision`: synthetic-collision blob has different content → integrity tag detects.
- `test_w10_eviction_respects_per_user_quota`.

## Success metric

- KV-cache hit rate > 60% on a 100-query benchmark of repeated-prefix prompts.
- p99 cache lookup ≤ 2 ms.
- Multimodal round-trip: 2 MB image stored + recovered byte-for-byte.
- Binary size delta ≤ +80 KB (gated; users without `HU_ENABLE_NEURAL_MEMORY` see 0).

## Risks

| Risk | Mitigation |
|------|------------|
| KV-cache blobs are model-format-specific | `model_version` tag invalidates on upgrade; entries carry a `format` field for future provider switches |
| Multimodal blobs balloon DB size | Per-user 100 MB quota; W14 evicts |
| Reasoning-trace leaks chain-of-thought to wrong contact | Per-contact key on every read; cross-contact recall blocked at SQL layer |

## Out of scope

- Generating new multimodal output. (Storage only.)
- Cross-process KV sharing.
- Differential KV updates (cache deltas).

## Binary size budget: +80 KB (gated behind HU_ENABLE_NEURAL_MEMORY).
