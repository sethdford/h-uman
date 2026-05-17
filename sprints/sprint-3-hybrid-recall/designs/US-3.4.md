# Design for US-3.4: One-shot backfill script for existing scoped memories

## Framing — the role of this story has shifted

The story as written assumes the daemon does NOT populate the in-memory vector store from `~/.human/memory.db` at startup, and that a separate backfill pass is therefore necessary to make hybrid recall useful immediately.

That assumption is no longer correct. Parallel investigation for US-3.3 confirmed: **the daemon bootstrap path already creates `hu_embedder_local_create` + `hu_vector_store_mem_create` unconditionally and re-embeds scoped memories at startup** (the design for US-3.3 cements this contract in AC-3.3.3). The vector store has no persistence layer (`store_mem.c`), and there is no sidecar file the daemon reads back. Anything this script writes to disk is therefore not consumed by the daemon.

That means US-3.4's *original* operational value ("first daemon-start is fast / hybrid recall works on day one") is **already delivered for free by US-3.3**. The story is redundant as a population mechanism.

We are deliberately NOT cancelling the story, because two non-redundant values remain:

1. **Pipeline validation outside the daemon.** Today there is no way to invoke the TF-IDF embedder without booting the full daemon. A standalone helper means the embedding pipeline can be smoke-tested by CI, by humans, and by future debugging sessions without a running daemon. This catches link/build regressions on `hu_embedder_local_create` that the test binary alone may miss (test binary links the full archive; a thin helper exposes the minimum public surface).
2. **User-facing CLI for inspecting embeddings.** A user asking "why didn't recall match X?" today has no introspection tool. `hu-embed-helper` + `embed-existing-memories.sh` together produce a deterministic, inspectable view of "here is what TF-IDF thinks your scoped memories look like" — useful for debugging recall misses, for support, and as the empirical evidence that the embedder produces the same vectors the daemon does.

So this story builds the **same artifacts** (helper binary + shell wrapper) and satisfies the **same 8 ACs**, but the framing in user-facing docs and the script's banner message is **diagnostic / verification**, not population. The shell script does NOT need to write embeddings anywhere the daemon will read them. It writes to a sidecar table purely for the user's inspection.

## Approach

Two artifacts, both intentionally minimal:

1. **`scripts/hu-embed-helper.c`** — a ~80-line C program. New CMake executable target `hu_embed_helper`. Links only the symbols it needs from `human_core` (the embedder factory + allocator). Reads UTF-8 text from stdin (or `argv[1]` if given), calls `hu_embedder_local_create(&hu_default_allocator)`, calls the vtable `embed(...)`, prints the 384-float embedding as a single JSON line `{"dim":384,"values":[...]}` to stdout. Exits 0 on success, non-zero on alloc / embed failure with a one-line stderr message. No flags, no options, no batching. Batching belongs in the shell wrapper (one helper invocation per row).

2. **`scripts/embed-existing-memories.sh`** — a ~120-line bash script under `set -euo pipefail`. Resolves `${HU_MEMORY_DB:-$HOME/.human/memory.db}`. Validates existence (AC-3.4.5). Opens via `sqlite3` CLI. Creates an `embeddings` sidecar table inside the SAME `memory.db` if not present. Iterates scoped rows from `memories` where `session_id IS NOT NULL AND session_id != ''` (this matches the schema in `include/human/memory/sql_common.h:14-18`). For each row, checks if an embedding already exists keyed by `memories.id`; if so, skip (idempotency for AC-3.4.3). Otherwise pipes the content to `./build/hu_embed_helper`, captures the JSON output, inserts a row into `embeddings`. Emits a progress dot per 100 rows once total > 1000 (AC-3.4.6). Prints `"Embedded N memories"` or `"Embedded 0 memories (all already embedded)"` to stdout.

**Storage decision: option (b) — a new SQLite table `embeddings` inside the existing `memory.db`.** Rejected: (a) sidecar binary file — opaque, can't be inspected with sqlite3, no schema. Rejected: (c) extending `memories` rows — couples vector data to the production schema and would require coordinating with the engine modules under `src/memory/engines/`, which is out of scope for a P2 story. Option (b) keeps the daemon's `memories` table untouched, lets the user inspect with `sqlite3 ~/.human/memory.db "SELECT memory_id FROM embeddings"`, and is purely additive: if the daemon never reads this table, nothing breaks.

Schema (created idempotently by the shell script via `CREATE TABLE IF NOT EXISTS`):

```sql
CREATE TABLE IF NOT EXISTS embeddings (
  memory_id TEXT PRIMARY KEY,
  dim       INTEGER NOT NULL,
  values_json TEXT NOT NULL,
  embedded_at TEXT NOT NULL DEFAULT (datetime('now')),
  FOREIGN KEY(memory_id) REFERENCES memories(id) ON DELETE CASCADE
);
```

The `values_json` text column is deliberately a JSON array, not a BLOB. Inspectable, diff-able, no endianness concerns. At 384 floats × ~10 chars each ≈ 4 KB per row, 1001 rows is ~4 MB — well under any sane database limit.

## Files to create / modify

| File | Change | Estimated LOC |
|---|---|---|
| `scripts/hu-embed-helper.c` | new C source for helper binary | +90 |
| `CMakeLists.txt` | add `hu_embed_helper` executable target after the `human_tests` target near line ~1328; `target_link_libraries(hu_embed_helper PRIVATE human_core)` | +6 |
| `scripts/embed-existing-memories.sh` | new bash script, `chmod +x` | +120 |
| `tests/test_embed_backfill.sh` | new shell test covering AC-3.4.2 / 3 / 4 / 5 / 6 / 8 (mirrors `tests/test_filler_guard.sh:1-40` style) | +130 |
| `sprints/sprint-3/evidence/3.4/` | evidence dir (created at verification time) | n/a |

No changes to any `src/` C file outside the new helper. No changes to `include/human/`. No changes to existing scripts.

## Implementation steps (for the implementer agent)

1. Add `scripts/hu-embed-helper.c`: include `human/memory/vector.h` and `human/core/allocator.h`, `main()` reads stdin into a heap buffer (cap at 1 MiB; exit 2 with stderr "input too large" if exceeded), calls `hu_embedder_local_create`, calls `vtable->embed`, prints JSON line, frees via `hu_embedding_free` and `vtable->deinit`. Exit 0.
2. Add the CMake target. Build with `cmake --preset dev && cmake --build --preset dev --target hu_embed_helper`. Verify the binary exists at `build/hu_embed_helper` and `echo "hello world" | build/hu_embed_helper | head -c 80` shows a JSON line beginning with `{"dim":384`.
3. Add `scripts/embed-existing-memories.sh` skeleton (shebang, `set -euo pipefail`, env resolution, existence check, exit-1 path for AC-3.4.5, exit-0 banner for empty db).
4. Verify AC-3.4.5 and AC-3.4.7 (shellcheck) at this point — they are pure shell-shape gates and can pass before any embedding logic exists.
5. Add the `CREATE TABLE IF NOT EXISTS embeddings` step and the row-iteration loop. Per-row: `SELECT 1 FROM embeddings WHERE memory_id = ?` for idempotency; `sqlite3` invocation via `-cmd` with quoted ID. Emit JSON output from helper into a temp file, then `INSERT INTO embeddings VALUES(...)` using `readfile()` or shell-quoted JSON.
6. Add progress reporting when row count > 1000 (count via `SELECT COUNT(*) FROM memories WHERE session_id IS NOT NULL AND session_id != ''`).
7. Add `tests/test_embed_backfill.sh` — fixture builders create temp dbs with 0 / 5 / 1001 rows; assertions on exit code and stdout patterns.
8. Run `shellcheck scripts/embed-existing-memories.sh` and resolve all warnings.
9. Run `bash tests/test_embed_backfill.sh` end-to-end on the dev build.
10. `/verify` — capture stdout transcripts as evidence under `sprints/sprint-3/evidence/3.4/`.

## Risks

- **HIGHEST: scope creep into vector store persistence (HIGH probability, LARGE impact).** The natural temptation is "well, since we're writing embeddings to SQLite anyway, why not have the daemon read them on startup?" Doing that would (a) require modifying `src/memory/vector/store_mem.c` or adding a load path in `src/memory/factory.c`, (b) introduce a schema contract the daemon must honor forever, (c) blow past the P2 estimate, and (d) couple US-3.4 to US-3.3 in a way the story explicitly does not require. **Mitigation:** the design states explicitly that the `embeddings` table is diagnostic-only and is NOT read by the daemon. Implementer must not add any read path. The critic agent should flag any change under `src/memory/` in the US-3.4 PR as out-of-scope.

- **Helper binary build coupling (MED / SMALL).** `hu_embed_helper` links `human_core`, which transitively pulls in libcurl, sqlite3, libsodium depending on cmake flags. The binary will be larger than its 90 lines suggest (~1+ MB). **Mitigation:** acceptable for a dev/diagnostic tool; document in the script banner that this is not a shipped artifact. Do NOT attempt to extract a `human_embedder_minimal` library — that's a refactor, not this story.

- **Idempotency race when run concurrently with the daemon (LOW / MED).** The daemon holds a WAL connection to `memory.db` (per `HU_SQL_PRAGMA_INIT` in `sql_common.h:9-12`). If the user runs the script while the daemon is up, `CREATE TABLE` and `INSERT` succeed under WAL but the daemon will not pick up the new table until next restart. **Mitigation:** acceptable. Script banner notes "safe to run with daemon up; daemon ignores this table." No file locking needed.

- **JSON output parsing in shell (LOW / SMALL).** Bash + sqlite3 + JSON is error-prone. **Mitigation:** never parse the JSON in shell — treat the helper's stdout as an opaque blob and store it verbatim. Only the helper produces JSON; the shell only forwards it.

- **Memory size for 1001-row case (LOW / SMALL).** 384 floats × 1001 rows ≈ 1.5 MB of float math in the helper, invoked 1001 times in separate processes. Each invocation is short-lived; no aggregate memory pressure. ASan-clean if `hu_embedding_free` and `vtable->deinit` are paired.

- **Observability (LOW).** Script logs row counts and skipped counts to stdout. On embed failure for a single row, log the row's `id` to stderr and continue (do not abort the whole run); summary line includes `"(N failures, see stderr)"`. Acceptable for a diagnostic tool.

## Test strategy

Shell-test pattern, mirroring `tests/test_filler_guard.sh`. New file `tests/test_embed_backfill.sh`:

| Case | Fixture | Assertion |
|---|---|---|
| empty db | `sqlite3` creates db with `memories` table but 0 rows | exit 0, stdout matches `0 memories` |
| missing db path | no file at `$HU_MEMORY_DB` | exit 1, stderr contains the path |
| 5 scoped rows | insert 5 rows with non-empty `session_id` | exit 0, stdout matches `Embedded [0-9]+ memories`, `SELECT COUNT(*) FROM embeddings` == 5 |
| re-run on same db | run twice | second run exit 0, stdout matches `all already embedded` or count repeats; `SELECT COUNT(*) FROM embeddings` unchanged |
| 1001 rows | seed via `INSERT INTO memories SELECT ...` loop | exit 0, completes; stdout shows progress markers |
| HU_MEMORY_DB override | set env to custom path, default path's mtime checked | script touches only the env-pointed db |
| shellcheck | `shellcheck scripts/embed-existing-memories.sh` | exit 0 |

This shell test is **not** registered in `human_tests` (matches the precedent set by `tests/test_filler_guard.sh:5-8`). It is invoked manually and by CI via a new entry in the `ci.yml` workflow (out of scope for this design — add a follow-up task if not already covered by the shell-tests step).

No C unit tests are added for `hu-embed-helper.c` directly; its correctness is bounded by `hu_embedder_local_create`, which is already covered by existing tests under `tests/` (the helper is a thin CLI wrapper).

## Acceptance criteria mapping

| AC | Role under reframing | Covered by |
|---|---|---|
| AC-3.4.1 (script exists, executable) | Still required | `test -x scripts/embed-existing-memories.sh` in `test_embed_backfill.sh` |
| AC-3.4.2 (happy path, 5 rows, `Embedded N memories`) | **Diagnostic** — proves embedder pipeline runs end-to-end outside daemon | `test_embed_backfill.sh` "5 scoped rows" case |
| AC-3.4.3 (idempotent) | Still required for re-running the diagnostic tool | `test_embed_backfill.sh` "re-run on same db" case |
| AC-3.4.4 (empty db, exit 0) | Still required | `test_embed_backfill.sh` "empty db" case |
| AC-3.4.5 (missing db, exit 1 with path) | Still required | `test_embed_backfill.sh` "missing db path" case |
| AC-3.4.6 (1001 rows, completes) | **Diagnostic** — proves the pipeline scales without OOM/segfault | `test_embed_backfill.sh` "1001 rows" case |
| AC-3.4.7 (shellcheck clean) | Still required | shellcheck step |
| AC-3.4.8 (`$HU_MEMORY_DB` override) | Still required | `test_embed_backfill.sh` "HU_MEMORY_DB override" case |

All 8 ACs remain testable and meaningful. ACs 3.4.2 and 3.4.6 are reframed as diagnostic evidence (the embedding pipeline works on real data at realistic scale) rather than as population steps; the verification commands and pass criteria do not change.

## Out of scope (do not implement in this story)

- Loading the `embeddings` table into the daemon's in-memory vector store at startup.
- A daemon `embed` subcommand.
- Any change under `src/memory/` or `src/memory/vector/`.
- Persistence of the vector store (`store_mem.c`) across restarts.
- Postgres / pgvector support.
- Re-embedding when a memory row's content changes (future story if needed).

## Notes for the implementer

- Use `hu_default_allocator` (the same allocator the daemon uses) — do not introduce a new allocator.
- The helper must call `vtable->deinit` and `hu_embedding_free` before exit; ASan-clean is non-negotiable.
- Do not parse JSON in bash. Treat the helper's stdout as an opaque single-line blob.
- The script's banner (`echo` on first line) must include the phrase "diagnostic tool" so future readers understand it does not feed the daemon.
- Evidence goes under `sprints/sprint-3/evidence/3.4/`: include the shellcheck transcript, a sample `embeddings` table dump (`sqlite3 ... ".dump embeddings"` truncated), and the `test_embed_backfill.sh` run log.

`RESULT_tech-lead=DESIGN_READY`
