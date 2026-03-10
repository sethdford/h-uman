---
title: SQLite/Database Code Audit Report
---
# SQLite/Database Code Audit Report

**Date:** 2025-03-08  
**Scope:** All SQLite usage in human C codebase (`src/`, `include/`)

---

## 1. SQLite Usage Map

| File                                  | Purpose                                                                                     |
| ------------------------------------- | ------------------------------------------------------------------------------------------- |
| `src/channels/imessage.c`             | Read-only access to macOS `~/Library/Messages/chat.db` for conversation history and polling |
| `src/memory/engines/sqlite.c`         | Primary memory backend: memories, messages, kv tables, FTS5                                 |
| `src/memory/engines/lucid.c`          | Lucid memory backend (SQLite + workspace)                                                   |
| `src/memory/engines/lancedb.c`        | LanceDB memory backend (SQLite metadata)                                                    |
| `src/memory/graph.c`                  | GraphRAG: entities, relations (GraphRAG knowledge graph)                                    |
| `src/memory/lifecycle/migrate.c`      | Migration: read legacy brain.db for import                                                  |
| `src/tools/database.c`                | Database tool: execute user-provided SQL (query/execute/tables)                             |
| `src/persona/cli.c`                   | iMessage query for persona sampling                                                         |
| `include/human/memory/sql_common.h` | Shared DDL and pragma definitions                                                           |
| `include/human/memory/retrieval.h`  | Declares `hu_graph_t` for retrieval engine                                                  |
| `include/human/memory/graph.h`      | Graph API (entities, relations)                                                             |

---

## 2. Schema Audit

### 2.1 Tables by File

| Table              | File                         | Indexes                   | Foreign Keys                           | Notes                 |
| ------------------ | ---------------------------- | ------------------------- | -------------------------------------- | --------------------- |
| `memories`         | sqlite.c, lucid.c, lancedb.c | category, key, session_id | None                                   | FTS5 in sqlite.c only |
| `memories_fts`     | sqlite.c                     | (FTS5 internal)           | N/A                                    | Virtual table         |
| `messages`         | sqlite.c                     | None                      | None                                   | Session store         |
| `kv`               | sqlite.c                     | (PRIMARY KEY)             | None                                   | Key-value             |
| `entities`         | graph.c                      | name                      | None                                   | GraphRAG              |
| `relations`        | graph.c                      | source_id, target_id      | source_id→entities, target_id→entities | GraphRAG              |
| `lancedb_memories` | lancedb.c                    | category, session_id      | None                                   | Different schema      |

### 2.2 Schema Issues

| Severity | File      | Line  | Description                                                                     |
| -------- | --------- | ----- | ------------------------------------------------------------------------------- |
| **Low**  | sqlite.c  | 43-46 | `messages` table has no index on `session_id`; list/load by session may be slow |
| **Low**  | graph.c   | 55-76 | No schema version or migration path; `IF NOT EXISTS` only                       |
| **Info** | All       | —     | No `user_version` PRAGMA for schema versioning                                  |
| **Info** | migrate.c | —     | Reads legacy schemas; column detection is heuristic-based                       |

### 2.3 Unused Tables

None identified. All tables are actively used.

---

## 3. Query Safety

### 3.1 Parameterization (SQL Injection)

| File       | Status    | Notes                                              |
| ---------- | --------- | -------------------------------------------------- |
| imessage.c | Safe      | All user/contact values via `sqlite3_bind_*`       |
| sqlite.c   | Safe      | All values parameterized                           |
| lucid.c    | Safe      | All values parameterized                           |
| lancedb.c  | Safe      | All values parameterized                           |
| graph.c    | Safe      | All values parameterized                           |
| migrate.c  | Low risk  | SQL built from schema metadata (hardcoded strings) |
| database.c | By design | User-provided SQL; no concatenation                |

**migrate.c (lines 131-133):** SQL built with `snprintf` using expressions from `detect_columns()` which assigns hardcoded strings. No user input. Low risk for trusted legacy brain.db.

### 3.2 Statement Finalization

| Severity | File       | Line          | Description                                                                                                               |
| -------- | ---------- | ------------- | ------------------------------------------------------------------------------------------------------------------------- |
| **HIGH** | graph.c    | 390, 421, 447 | `hu_graph_neighbors`: `goto fail` without `sqlite3_finalize(stmt)` — statement leak on prepare failure or realloc failure |
| OK       | All others | —             | All paths finalize before return                                                                                          |

### 3.3 sqlite3_step Handling

All files correctly use `SQLITE_ROW` for SELECT and `SQLITE_DONE` for INSERT/UPDATE/DELETE.

### 3.4 Transactions

No explicit `BEGIN`/`COMMIT`. SQLite auto-commits each statement. graph.c `hu_graph_upsert_entity` uses three statements without a transaction; rare race if concurrent writers.

---

## 4. SQLITE_TRANSIENT Check

**Result: None found.** All `sqlite3_bind_text` use `SQLITE_STATIC` or `NULL`. Compliant with project rule.

---

## 5. Memory Management

### 5.1 sqlite3_open / sqlite3_close

All files: paired correctly; close on all paths.

### 5.2 sqlite3_prepare / sqlite3_finalize

| File       | Issue                                                             |
| ---------- | ----------------------------------------------------------------- |
| graph.c    | **Leak:** `hu_graph_neighbors` fail path does not finalize `stmt` |
| All others | Finalize on all paths                                             |

### 5.3 sqlite3_malloc / sqlite3_free

Only `sqlite3_free(errmsg)` for error strings. All usages correct.

---

## 6. Concurrency

| Aspect                            | Status                                                                       |
| --------------------------------- | ---------------------------------------------------------------------------- |
| Multi-threaded access             | No shared SQLite db across threads. Each channel/engine owns its connection. |
| WAL mode                          | Set via `HU_SQL_PRAGMA_INIT` in sqlite.c, lucid.c, lancedb.c                 |
| graph.c                           | **Missing:** No `journal_mode=WAL`, `busy_timeout`, or `foreign_keys`        |
| imessage.c, migrate.c, database.c | Read-only or short-lived; no pragmas (acceptable)                            |

---

## 7. GraphRAG Module (src/memory/graph.c)

### 7.1 Schema Design

Adequate. entities + relations with proper indexes and foreign keys.

### 7.2 Query Safety

All queries parameterized. No string concatenation.

### 7.3 Statement Finalization — BUG

In `hu_graph_neighbors`, three `goto fail` paths do not call `sqlite3_finalize(stmt)`:

- After prepare failure (line 390)
- After entity realloc failure (line 421)
- After relation realloc failure (line 447)

### 7.4 Error Handling

Returns `HU_ERR_IO`, `HU_ERR_INVALID_ARGUMENT`, `HU_ERR_NOT_FOUND`. No silent swallow.

### 7.5 NULL Checks

All public APIs check `g`, `g->db`, `alloc`, and output pointers.

### 7.6 Memory Leaks

stmt leak (see 7.3). Entity/relation arrays correctly freed in fail block.

---

## 8. Error Handling

All modules return appropriate `hu_error_t` codes. database.c uses `hu_tool_result_fail` (tool convention).

---

## 9. Summary of Issues

| Severity   | Count | Items                                                                                        |
| ---------- | ----- | -------------------------------------------------------------------------------------------- |
| **High**   | 1     | graph.c: stmt leak on fail path in `hu_graph_neighbors`                                      |
| **Medium** | 1     | graph.c: missing WAL/busy_timeout/foreign_keys pragmas                                       |
| **Low**    | 3     | messages missing session_id index; migrate.c SQL from schema; graph upsert not transactional |

---

## 10. Recommendations

1. ~~**Fix graph.c stmt leak**~~ **DONE:** `hu_graph_neighbors` now declares `stmt` at function scope and finalizes it in the fail block.

2. ~~**Add pragmas to graph.c**~~ **DONE:** After `sqlite3_open`, now runs `HU_SQL_PRAGMA_INIT` and `sqlite3_busy_timeout(g->db, HU_SQLITE_BUSY_TIMEOUT_MS)`.

3. **Optional:** Add index on `messages(session_id)` in sqlite.c.

4. **Optional:** Wrap `hu_graph_upsert_entity` in `BEGIN`/`COMMIT` for atomicity.
