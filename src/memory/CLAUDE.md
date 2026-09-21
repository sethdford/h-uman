# src/memory/ — Memory Subsystem

The memory system manages how the agent stores, retrieves, and evolves knowledge over time.

## Architecture

```
factory.c           Engine registry and creation (hu_memory_t vtable)
  engines/           Backend implementations (10 engines)
    sqlite.c         SQLite-backed persistent memory
    markdown.c       Markdown file memory
    memory_lru.c     In-memory LRU cache
    sqlite_lucid.c   SQLite + optional lucid CLI backend
    sqlite_fts.c     SQLite + FTS5 full-text search backend
    postgres.c       PostgreSQL-backed memory (HU_ENABLE_POSTGRES)
    redis.c          Redis-backed memory (HU_ENABLE_REDIS_ENGINE)
    api.c            Remote API memory backend
    none.c           No-op backend

retrieval/           Query and retrieval pipeline
  engine.c           Main retrieval engine (coordinates hybrid search)
  hybrid.c           Combines vector + keyword results
  keyword.c          Keyword/BM25 retrieval
  reranker.c         Result reranking
  qmd.c             Query-Memory Dispatch (routes queries to best retrieval strategy)

vector/              Vector search and embeddings
  embeddings.c       Embedding generation (via provider)

lifecycle/           Memory maintenance
  cache.c            Semantic cache
```

## Key Concepts

- **Engines** implement `hu_memory_t` vtable (store, retrieve, search, delete)
- **Retrieval** orchestrates hybrid search: vector similarity + keyword match + reranking
- **QMD** classifies incoming queries to pick the optimal retrieval strategy
- **Lifecycle** caches memory entries and semantic query results

## Cognitive/Emotional Layer

```
emotional_graph.c       Tracks emotional patterns across conversations
emotional_residue.c     Persists emotional context between sessions
emotional_moments.c     Identifies emotionally significant memories
comfort_patterns.c      Learned comfort/coping patterns
episodic.c              Episodic memory formation
stm.c                   Short-term memory buffer
deep_memory.c           Deep memory retrieval and analysis
deep_extract.c          Extraction from deep memory stores
graph.c / fast_capture.c  Knowledge graph with entity/relation extraction
```

## Memory Evolution

```
consolidation.c         Merges and deduplicates memories
consolidation_engine.c  Background consolidation processing
forgetting.c            Spaced-repetition based forgetting
forgetting_curve.c      Ebbinghaus forgetting curve implementation
promotion.c             Promotes important short-term to long-term
compression.c           Compresses verbose memories
connections.c           Cross-memory connection discovery
```

## Rules

- All engines must implement the `hu_memory_t` vtable fully
- Use `SQLITE_STATIC` (null), never `SQLITE_TRANSIENT`
- Free all allocations — embeddings and search results allocate heavily
- Vector operations can be expensive — test with realistic sizes
- QMD route selection must be deterministic for the same input
