# US-3.4 re-open fixes — verifier said FAIL on 4 ACs, now PASS

Prior commit: 3d2ea6e4 (verifier: 4/8 FAIL)
New commit: pending (after this evidence is written)

## Fix 1: AC-3.4.7 — shellcheck clean (was SC2034 on `local pair`)
```
exit=0
```

## Fix 2: AC-3.4.4 — 'Embedded 0 memories' on empty db
```
$ ./scripts/embed-existing-memories.sh --db /tmp/x_empty.db --helper ./build/hu_embed_helper 2>/dev/null
Embedded 0 memories

$ ... | grep '^Embedded 0 memories$'
Embedded 0 memories
```

## Fix 3: AC-3.4.8 — HU_MEMORY_DB env override
```
$ HU_MEMORY_DB=/tmp/x_empty.db ./scripts/embed-existing-memories.sh --helper ./build/hu_embed_helper 2>&1 | grep 'using database'
[embed-backfill] using database: /tmp/x_empty.db

Priority order: --db > HU_MEMORY_DB > default. Verify --db wins:
$ HU_MEMORY_DB=/tmp/should-not-use.db ./scripts/embed-existing-memories.sh --db /tmp/x_empty.db --helper ./build/hu_embed_helper 2>&1 | grep 'using database'
[embed-backfill] using database: /tmp/x_empty.db
```

## Fix 4: AC-3.4.2 — 'Embedded N memories' stdout pattern on populated db
```
$ ./scripts/embed-existing-memories.sh --db $TMPP/pop.db --helper ./build/hu_embed_helper
Embedded 3 memories

stdout-only (drop stderr) — must end with 'Embedded N memories':
$ ... 2>/dev/null
Embedded 3 memories

grep pattern 'Embedded [0-9]+ memories' to stdout:
Embedded 2 memories
MATCH
```

## All 8 ACs
| AC | Status | Notes |
|----|--------|-------|
| 3.4.1 script exists + executable | PASS | unchanged |
| 3.4.2 stdout 'Embedded N memories' | PASS (was FAIL) | printf added after log |
| 3.4.3 idempotent re-run | PASS | unchanged |
| 3.4.4 empty db → 'Embedded 0 memories' + exit 0 | PASS (was FAIL) | printf added before exit 0 |
| 3.4.5 missing db → exit 1 | PASS | unchanged |
| 3.4.6 >1000 rows batched | PASS | unchanged |
| 3.4.7 shellcheck clean | PASS (was FAIL) | removed SC2034 'pair' |
| 3.4.8 HU_MEMORY_DB env override | PASS (was FAIL) | added after DB_PATH default; --db still wins |
