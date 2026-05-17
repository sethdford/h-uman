# Shell script tests

## AC-3.4.5 — missing memory.db → exit 1
```
[embed-backfill] ERROR: memory.db not found at: /var/folders/57/0gs_mdl104q8vk054nz3wp3h0000gn/T/tmp.4QlmJP0AE7/nope.db
[embed-backfill] ERROR: Run h-uman at least once to create the memory store.
[exit=1]
```

## AC-3.4.4 — empty memory.db → exit 0 with message
```
[embed-backfill] using helper: ./build/hu_embed_helper
[embed-backfill] using database: /var/folders/57/0gs_mdl104q8vk054nz3wp3h0000gn/T/tmp.4QlmJP0AE7/empty.db
[embed-backfill] source: memories(key, content)
[embed-backfill] nothing to embed: memories has 0 rows with non-empty content
[exit=0]
```

## AC-3.4.2/3 — populated db, helper invoked, embeddings table populated
```
[embed-backfill] using helper: ./build/hu_embed_helper
[embed-backfill] using database: /var/folders/57/0gs_mdl104q8vk054nz3wp3h0000gn/T/tmp.4QlmJP0AE7/pop.db
[embed-backfill] source: memories(key, content)
[embed-backfill] found 3 row(s); existing embeddings will be skipped (idempotent)
[embed-backfill] done: processed=3 inserted=3 skipped=0 failed=0
[exit=0]

embeddings table contents:
memory_key  dimensions  embedder_version  created            
----------  ----------  ----------------  -------------------
m1          384         tfidf-local-v1    2026-05-15T14:26:39
m2          384         tfidf-local-v1    2026-05-15T14:26:39
m3          384         tfidf-local-v1    2026-05-15T14:26:39
```

## Idempotency — re-run, all skipped, inserted=0
```
[embed-backfill] using helper: ./build/hu_embed_helper
[embed-backfill] using database: /var/folders/57/0gs_mdl104q8vk054nz3wp3h0000gn/T/tmp.4QlmJP0AE7/pop.db
[embed-backfill] source: memories(key, content)
[embed-backfill] found 3 row(s); existing embeddings will be skipped (idempotent)
[embed-backfill] done: processed=3 inserted=0 skipped=3 failed=0
[exit=0]

row count unchanged: 3
```

## AC-3.4.6 — >1000 rows → batched progress
```
[embed-backfill] source: memories(key, content)
[embed-backfill] found 1100 row(s); existing embeddings will be skipped (idempotent)
[embed-backfill] progress: 250 / 1100 (inserted=250 skipped=0 failed=0)
[embed-backfill] progress: 500 / 1100 (inserted=500 skipped=0 failed=0)
[embed-backfill] progress: 750 / 1100 (inserted=750 skipped=0 failed=0)
[embed-backfill] progress: 1000 / 1100 (inserted=1000 skipped=0 failed=0)
[embed-backfill] done: processed=1100 inserted=1100 skipped=0 failed=0
embeddings row count: 1100
```
