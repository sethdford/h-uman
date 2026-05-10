---
title: Memory Lifecycle Standard
---

# Memory Lifecycle Standard

Standards for memory system design, including storage tiers, consolidation, forgetting, RAG safety, and vector search configuration.

**Cross-references:** [../ai/hallucination-prevention.md](../ai/hallucination-prevention.md), [../security/data-privacy.md](../security/data-privacy.md), [config-schema.md](config-schema.md)

---

## Overview

The memory system preserves agent knowledge across sessions. This standard covers memory tiers, consolidation triggers, the forgetting curve, promotion heuristics, RAG safety guards, and vector search tuning.

**Related code:** `src/memory/` (consolidation.c, engine.c, deep_memory.c), `include/human/memory/`.

---

## Memory Tiers (Hot → Cold)

Memory is stratified by access patterns and retention:

### Tier 1: Short-Term (Working Memory)

**Duration:** Session lifetime (minutes to hours)  
**Storage:** In-memory LRU or session cache  
**Size limit:** 1–10 MB (configurable)  
**Content:** Current conversation, immediate context  

**Example:**
```
[Session 42]
- Last 50 messages
- Active tool state
- Current agent turn
```

**Eviction:** LRU when capacity exceeded. Survivor events promoted to Tier 2.

### Tier 2: Working Memory

**Duration:** 7 days (default, configurable)  
**Storage:** SQLite (local) or PostgreSQL (shared)  
**Size limit:** 100 MB–1 GB per agent  
**Content:** Recent conversations, patterns, learned context  

**Example:**
```
[SQLite]
- Last 500 messages (7-day window)
- Extracted facts
- User preferences (topic interest, tone, style)
- Tool call patterns
```

**Promotion trigger:** After 5+ accesses or age > 1 day.  
**Demotion trigger:** After 30 days, move to Tier 3.

### Tier 3: Long-Term (Reference Memory)

**Duration:** Indefinite (archive)  
**Storage:** SQLite archive partition or separate DB  
**Size limit:** Unlimited (disk-constrained)  
**Content:** Historical conversations, life events, relationships  

**Example:**
```
[SQLite archive]
- All conversations older than 30 days
- Biographical facts (birthday, family, pets)
- Historical decision precedents
- Learned decision patterns
```

**Access:** Retrieved only on explicit recall or RAG query.  
**Retention policy:** Delete if requested by user; redact PII.

### Tier 4: Archival (Backup)

**Duration:** Retention policy (e.g., 90 days)  
**Storage:** Encrypted backup (S3, tape archive)  
**Content:** Snapshot of Tiers 2 + 3  

**Use case:** Recovery from data loss; compliance/audit.

---

## Consolidation: Deduplication and Summarization

### Trigger Conditions

Consolidation runs when ANY of these conditions are met:

1. **Topic switch detected:** Agent or user changes topic (configurable; default: 5+ min silence then new topic)
2. **Time interval:** Every 1 hour OR every 24 hours (configurable)
3. **Entry threshold:** > 500 entries in working memory
4. **Manual trigger:** User invokes `memory.consolidate` command

**Debounce logic (prevent thrashing):**

```
Min interval: 60 seconds between consolidations
Min entries: 5 new entries since last consolidation
If both NOT met, defer consolidation
```

### Consolidation Algorithm

**Input:** Working memory (short-term entries)  
**Output:** Deduplicated, summarized entries ready for promotion

**Steps:**

1. **Deduplication:** 
   - Compute similarity scores between all entries (token overlap %)
   - If similarity > threshold (default 85%), mark for merge
   - Keep highest-quality entry; discard duplicates

2. **Summarization (if enabled):**
   - Group entries by topic/session
   - Invoke LLM to summarize group → single entry
   - Preserve timestamps (earliest, latest)
   - Estimate confidence (0.0–1.0)

3. **Fact extraction (if enabled):**
   - Run `hu_deep_extract()` on surviving entries
   - Extract atomic facts (e.g., "User is engineer at Acme Corp")
   - Store as propositions with confidence ≥ threshold

4. **Promotion:**
   - Move deduplicated entries to Tier 2 (working memory)
   - Move oldest entries to Tier 3 (long-term) if > 7 days

### Configuration

```json
{
  "memory": {
    "consolidation": {
      "enabled": true,
      "decay_days": 30,
      "decay_factor": 0.9,
      "dedup_threshold": 85,
      "max_entries": 10000,
      "extract_facts": true,
      "fact_confidence_threshold": 0.5
    }
  }
}
```

| Field | Default | Purpose |
|-------|---------|---------|
| `decay_days` | 30 | Time for entry to decay to 0 importance |
| `decay_factor` | 0.9 | Multiplicative decay per day |
| `dedup_threshold` | 85 | Token overlap % to consider duplicate |
| `max_entries` | 10000 | Hard cap on working memory |
| `extract_facts` | true | Run fact extraction on consolidation |
| `fact_confidence_threshold` | 0.5 | Min confidence to store fact |

---

## Forgetting Curve Implementation

Memory follows **Ebbinghaus forgetting curve**: importance decays exponentially over time.

### Importance Decay Formula

```
importance(t) = base_importance * (decay_factor ^ (t / decay_days))
```

**Example (decay_days=30, decay_factor=0.9):**

| Days | Factor | Importance |
|------|--------|------------|
| 0 | 0.9^0 | 1.0 (100%) |
| 7 | 0.9^0.23 | 0.978 (98%) |
| 14 | 0.9^0.47 | 0.957 (96%) |
| 30 | 0.9^1.0 | 0.9 (90%) |
| 60 | 0.9^2.0 | 0.81 (81%) |
| 90 | 0.9^3.0 | 0.729 (73%) |

### Refresh on Access

When entry is accessed (via RAG query or agent recall):

```
last_accessed = now
importance *= 1.1  // 10% boost for accessing
```

This simulates "spaced repetition" — accessing information extends its memory.

### Automatic Forgetting

Entries with importance < 0.1 (10%) are candidates for deletion:

```c
if (entry->importance < 0.1 && age_days > 90) {
    hu_memory_delete(memory, entry->key);
}
```

**User control:** `hu_memory_forget(memory, query)` forces deletion regardless of importance.

---

## Promotion Heuristics

Entries are promoted from Tier 1 → Tier 2 when:

1. **Access frequency:** ≥ 5 accesses since creation
2. **Age:** ≥ 1 day old (ensures not ephemeral)
3. **Importance:** > 0.5 (significant to agent)
4. **Type:** Atomic fact or learned pattern (not raw message)

**Example:**

```
User asks: "What's my dog's name?"
Message stored → "Your dog's name is Buddy"
Access count: 1
[After 3 more accesses over 2 days]
Access count: 4
Importance: 0.8
Age: 2 days
✓ Promote to Tier 2 as fact: { subject: "dog", predicate: "name", object: "Buddy" }
```

---

## Demotion to Long-Term

Entries move from Tier 2 → Tier 3 (long-term archive) when:

1. **Age:** ≥ 30 days AND
2. **Recency:** No access in last 14 days AND
3. **Size constraint:** Tier 2 approaching capacity

This keeps working memory lean while preserving historical context.

---

## RAG Pipeline Safety (Hallucination Guards)

When retrieving facts from memory for agent response, validate against memory:

### Hallucination Guard Protocol

1. **Retrieve candidates:** Query memory for matching facts (top-5)
2. **Confidence filter:** Discard candidates with confidence < 0.7 (low confidence)
3. **Cross-reference:** Check if candidate contradicts other high-confidence facts
4. **Cite source:** Attach `[source: memory_id, confidence: 0.95]` tag
5. **Low-confidence marking:** If confidence 0.5–0.7, prefix with **"Possibly:"**

**Example:**

```
Query: "What was our last meeting about?"

Retrieved facts:
- "Q3 budget review" (confidence: 0.92, source: memory_4521) ✓
- "Something about revenue" (confidence: 0.43) ✗ Too low; discard
- "Q4 planning" (confidence: 0.87, source: memory_4520) ✓

Response: "Based on our conversation history, 
we discussed the Q3 budget review and Q4 planning."
[Citations: memory_4521, memory_4520]
```

### Fact Verification Workflow

Before storing extracted facts from consolidation:

1. **LLM extracts:** "User is software engineer"
2. **Confidence scoring:** 0.85
3. **Against KB:** Check for contradictions in existing facts
4. **Store:** If confidence ≥ threshold (0.5) and no contradictions
5. **Update:** If contradicts high-confidence fact, log warning and discard

---

## Vector Search Configuration

Vector embeddings enable semantic search (find similar memories even with different wording).

### Embedding Model

```json
{
  "memory": {
    "vector": {
      "enabled": true,
      "embedding_model": "sentence-transformers/all-mpnet-base-v2",
      "embedding_dim": 768,
      "similarity_threshold": 0.75,
      "max_cached_embeddings": 10000
    }
  }
}
```

### Chunk Strategy

Memory is chunked for embedding (prevent embedding entire conversations):

**Chunk size:** 512 tokens  
**Overlap:** 256 tokens (50% overlap to maintain context)

**Example:**

```
Message: "I love hiking in Colorado. The Rocky Mountains are amazing.
Last summer I climbed Mount Elbert. It was challenging but rewarding."

Chunk 1: "I love hiking in Colorado. The Rocky Mountains are amazing." (28 tokens)
Chunk 2: "The Rocky Mountains are amazing. Last summer I climbed Mount Elbert." (30 tokens)
Chunk 3: "Mount Elbert. It was challenging but rewarding." (20 tokens)
```

### Vector Storage

Vectors are stored alongside memory entries in SQLite or PostgreSQL:

```sql
CREATE TABLE memory_vectors (
  id INTEGER PRIMARY KEY,
  memory_id TEXT,
  chunk_idx INTEGER,
  embedding BLOB,  -- 768 floats stored as binary
  similarity REAL,
  FOREIGN KEY(memory_id) REFERENCES memory(id)
);
```

### Similarity Computation

When querying, compute cosine similarity between query embedding and stored embeddings:

```
cosine_sim(A, B) = (A · B) / (||A|| * ||B||)
Range: -1 to +1 (typically 0 to 1 for positive similarity)
Threshold: 0.75 (75% similarity = relevant result)
```

### Embedding Caching

Cache embeddings in memory to avoid recomputation:

```c
typedef struct hu_embedding_cache {
    hu_hash_table_t *cache;  // key: text_hash, value: embedding_vector
    size_t max_cached;
    size_t current_size;
} hu_embedding_cache_t;
```

Cache miss handling:
1. Compute embedding (via API or local model)
2. Store in cache (LRU eviction if full)
3. Return result

**Re-embed triggers:**
- Memory content changed
- Embedding model updated
- Embedding age > 30 days (stale)

---

## Testing Expectations

Memory system must pass test suite:

```bash
./human_tests --suite=Memory             # Core memory ops
./human_tests --suite=MemoryConsolidation # Consolidation
./human_tests --suite=MemoryVector        # Vector search
./human_tests --suite=RAG                 # RAG safety
```

Required tests:

- Tier promotion (5 accesses, 1+ day age)
- Forgetting curve decay (importance over time)
- Consolidation deduplication (85% threshold)
- Fact extraction (confidence scoring)
- Hallucination guard (low-confidence filtering)
- Vector similarity search (0.75 threshold)
- Embedding cache (LRU eviction)
- Cross-reference (detect contradictions)
- Re-embedding trigger (stale embeddings)

---

## Memory Privacy and Retention

See `docs/standards/security/data-privacy.md` for:

- PII redaction rules
- Retention policy per channel
- User right-to-be-forgotten (GDPR/CCPA)
- Encrypted backups
- Audit logging of memory access

---

## Anti-Patterns

```
WRONG -- Store raw API responses in memory
RIGHT -- Extract facts; store with confidence score

WRONG -- Use memory without hallucination guard
RIGHT -- Filter low-confidence; cite sources

WRONG -- Recompute embeddings every query
RIGHT -- Cache embeddings; invalidate on content change

WRONG -- Store all memories forever
RIGHT -- Implement forgetting curve; delete old low-importance entries

WRONG -- Mix PII with general facts
RIGHT -- Separate PII tier; apply redaction policy
```

---

## Key Paths

- Memory engine: `src/memory/engine.c`
- Consolidation: `src/memory/consolidation.c`
- Deep memory (facts): `src/memory/deep_memory.c`
- Vector search: `src/memory/vector_search.c`
- Hallucination guard: `src/memory/hallucination_guard.c`
- Headers: `include/human/memory/`
- Tests: `tests/test_memory*.c`
- Privacy policy: `docs/standards/security/data-privacy.md`
