---
paths: src/memory/**/*.c, src/memory/**/*.h, include/human/memory/**/*.h
---

# Memory Subsystem Rules

The memory system stores, retrieves, and evolves agent knowledge. Read `src/memory/CLAUDE.md` for architecture and `AGENTS.md` sections 5.2 and 7.5 before modifying.

## Engine Vtable Contract

All engines implement `hu_memory_t`. Required methods:

- `store(key, value, metadata)` → error code
- `retrieve(key)` → value + metadata
- `search(query, limit)` → ranked results with scores
- `delete(key)` → error code
- `stat(key)` → metadata (created, updated, access_count)
- `flush()` → clear engine state
- `close()` → cleanup and shutdown

Optional methods may be NULL: `update`, `get_name`, `get_provider_config`.

## Consolidation & Lifecycle

- Consolidation must be idempotent (safe to re-run)
- Deduplication uses `behavior.dedup_threshold` (0–100 token-overlap score, default 70); the older
  entry of a same-contact pair is deleted. Every caller gets its settings from
  `hu_agent_consolidation_config()`; never build a `hu_consolidation_config_t` by hand.
- Promotion: short-term → long-term happens after `promotion_threshold` accesses
- Forgetting follows Ebbinghaus curve; parameters are configurable per policy
- RAG pipeline must verify facts before returning (no naked hallucinations)

## Vector Search Safety

- Embedding operations are expensive; always test with realistic corpus (1M+ items)
- Circuit breaker on embedding provider failures; fallback to keyword search
- Vector stores must validate dimensions match embedder config
- Similarity scores must be normalized [0, 1] consistently across providers
- TTL on cached embeddings; re-embed if corpus changes

## Hallucination Prevention

- `hallucination_guard.c` checks all generated facts against knowledge base
- Confidence threshold (default 0.7) gates responses; low-confidence facts marked
- Claim verification must cross-reference multiple memory sources
- RAG pipeline must include provenance (cite sources)

## Memory Policies

- Per-engine privacy policy (PII redaction, retention limits)
- Per-channel retention (ephemeral memory vs. persistent)
- Memory tiering: hot (LRU/Redis) → warm (SQLite) → cold (archive)
- QMD strategy selection must be deterministic for same query

## Validation

```bash
cd build && cmake --build . -j$(nproc)
./human_tests --suite=Memory
./human_tests --suite=Vector
./human_tests --suite=RAG
```

## Standards

- Read `docs/standards/engineering/memory-management.md` for allocation rules
- Read `docs/standards/ai/hallucination-prevention.md` for RAG safety
- Read `docs/standards/security/privacy.md` for PII handling
