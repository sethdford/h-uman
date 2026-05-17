---
title: "Sprint 3 — Hybrid recall: wire, test, embed, backfill"
created: 2026-05-15
status: planning
sprint: 3
program: /Users/sethford/.claude/plans/quirky-gliding-octopus.md
branch: sprint-3-hybrid-recall
worktree: /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-1-hybrid-recall
---

# Sprint 3 Backlog

## Goal

Per-contact recall returns semantically-relevant memories (not just keyword-matched) by wiring the existing `hu_hybrid_retrieve` RRF pipeline into the contact-memory path, establishing the daemon-driven integration test template all future sprints reuse, and backfilling existing memories into the vector store on day one.

---

## Per-Story Definition of Done

Every story must satisfy ALL of the following before the implementer may report DONE:

1. Verifier returned `RESULT_verifier=PASS` with evidence captured in `sprints/sprint-3/evidence/<story>/`.
2. Critic returned CLEAN, or every HIGH+ finding is converted into a follow-up story with its own US-ID and filed in `sprints/sprint-3/followups.md`.
3. Aspect-Panel 5/5 pass (disagreement escalates to Tech Lead).
4. Tests exist: happy path + error paths + edge cases (see per-story AC for specifics).
5. Compiles `-Wall -Wextra -Wpedantic -Werror` clean.
6. ASan clean under `cmake --preset dev`.
7. **Implementer committed changes to branch `sprint-3-hybrid-recall` BEFORE reporting DONE.** Working-tree-only DONE reports are rejected — this is non-negotiable. The Sprint 1 audit (`sprints/sprint-1/audit.md`) caught this exact failure mode: 4/4 stories failed because work existed only in `git stash` or the working tree, not in HEAD.

## Per-Sprint Definition of Done

1. All four stories satisfy their per-story DoD above.
2. `tests/integration/test_sprint3_hybrid_recall.c` exists and passes: `./build/human_tests --suite=sprint3_hybrid_recall` shows 0 failures, 0 ASan errors.
3. Production binary rebuilt clean: `cmake --build --preset dev --target human` exits 0.
4. Installed: `scripts/install-human-daemon.sh` exits 0; `human-daemon doctor` reports green.
5. `sprints/sprint-3/verify.sh` runs without error and all assertions pass:
   - Semantic-recall probe: store "she's planning a Utah hike", query "outdoor plans", result contains Utah-hike entry.
   - `sqlite3 ~/.human/memory.db "SELECT COUNT(*) FROM memories WHERE session_id LIKE '%18018285260%';"` returns a growing (non-zero) count.
   - Recall latency: `time human-daemon memory recall --contact "+18018285260" --query "outdoor plans"` p50 < 50 ms (5-run median).
6. Full test suite: 10000+ tests pass, 0 failures, 0 ASan errors: `./build/human_tests`.
7. Sprint Auditor returns `RESULT_sprint-auditor=PASS` or `PASS_WITH_NOTES`.
8. PR opened against `main`, Code-Reviewer approved, squash-merged.
9. Retro: `/mine-transcripts` run over sprint window; any agent with 2+ verifier failures queued for `/tune-agent`.

---

## User Stories (in priority order)

---

### US-3.2 (P0): Integration test harness — daemon-driven, real SQLite, mock provider

**As a** developer on any sprint in this program,
**I want** a runnable integration test at `tests/integration/test_sprint3_hybrid_recall.c` that stands up a real SQLite memory backend, drives `hu_agent_turn` with a deterministic mock provider, and asserts observable state in the database and recall results,
**So that** every current and future sprint has a concrete, self-contained proof of end-to-end memory behaviour — not just unit tests of isolated functions.

**Why P0:** This story is the verification foundation for US-3.1, US-3.3, and US-3.4. Without it, no other story can prove its ACs in the e2e integration sense required by the per-sprint DoD. It is also the template the plan (`quirky-gliding-octopus.md`) designates as "FU-3 from the prior follow-up spec."

**Acceptance criteria:**

- AC-3.2.1: `tests/integration/test_sprint3_hybrid_recall.c` exists, is registered in `CMakeLists.txt` (or the project's equivalent test-registration mechanism), and `cmake --build --preset dev` compiles it with zero errors and zero warnings under `-Wall -Wextra -Wpedantic -Werror`. Verified by: `cmake --build --preset dev 2>&1 | grep -c "error:"` prints `0`.

- AC-3.2.2: The test creates a real (non-`:memory:`) SQLite database file in a system tempdir (e.g., via `mkdtemp`), initialises a `hu_memory_t` SQLite backend against it, and tears down + deletes the tempdir at test exit — leaving no artifacts on disk. Verified by: running the test twice in succession; `ls /tmp/hu_sprint3_*` (or whatever prefix the test uses) returns empty after each run.

- AC-3.2.3: The test pre-populates at least 10 memory entries across at least 3 distinct `session_id` values using `hu_memory_store_for_contact` (`src/memory/contact_memory.c:11`), with deliberately varied content that uses no shared keywords between groups (e.g., one group about outdoor activities, one about cooking, one about finance). Verified by: `sqlite3 <tempdir>/memory.db "SELECT COUNT(DISTINCT session_id) FROM memories;"` inside the test asserts `>= 3` via `HU_ASSERT_INT_GTE` (or equivalent); the assertion fires before any recall call.

- AC-3.2.4: The mock provider used in `hu_agent_turn` is deterministic: given any input, it returns a fixed canned response (e.g., `"OK"`), never makes a network call, and is guarded by `HU_IS_TEST`. Verified by: `grep -n "HU_IS_TEST" tests/integration/test_sprint3_hybrid_recall.c` returns at least one match; the test passes with no network access (`HU_NO_NETWORK=1 ./build/human_tests --suite=sprint3_hybrid_recall` exits 0).

- AC-3.2.5 (semantic-recall): After storing "she is planning a Utah hiking trip next summer" under contact `+18018285260` and calling `hu_memory_recall_for_contact` with query `"outdoor plans"`, the returned entry set contains an entry whose `content` field includes the substring `"Utah"` — even though `"Utah"` does not appear in the query. Verified by: the test asserts `found_utah == true` after iterating `out_entries`; `./build/human_tests --suite=sprint3_hybrid_recall` passes this assertion.

- AC-3.2.6 (scoping preserved — PR #83 regression guard): After storing memories for two distinct contacts (`+18018285260` and `+15550001234`), a recall call for `+18018285260` returns zero entries whose `key` begins with `"contact:+15550001234:"`. Verified by: the test iterates all returned entries and asserts the cross-contact count is `0`; if any cross-contact entry leaks, the test fails with an explicit message naming the offending key.

- AC-3.2.7 (BM25-only fallback): When `hu_memory_recall_for_contact` is called with `embedder == NULL` and `vector_store == NULL` (the BM25-only path that exists today in `src/memory/contact_memory.c:38`), storing "pasta carbonara recipe" and querying `"carbonara"` returns at least one entry containing `"carbonara"`. Verified by: a dedicated sub-test within the file that explicitly passes NULL embedder/vector_store through to the recall path; assertion fires on result count `>= 1`.

- AC-3.2.8: `./build/human_tests --suite=sprint3_hybrid_recall` runs to completion in under 10 seconds on a laptop-class machine (no sleeps, no real network, no subprocess spawning outside `HU_IS_TEST` guards). Verified by: `time ./build/human_tests --suite=sprint3_hybrid_recall 2>&1 | tail -3` shows elapsed time `< 10s`.

- AC-3.2.9: The test file's top-of-file comment block documents (a) how to run it in isolation, (b) what tempdir it uses and how cleanup works, and (c) that it is the canonical template for future sprint integration tests. Verified by: `head -20 tests/integration/test_sprint3_hybrid_recall.c` contains the strings `"isolation"`, `"tempdir"`, and `"template"`.

**Estimate:** M
**Dependencies:** none (uses only existing `hu_memory_t` SQLite backend and `hu_agent_turn` mock-provider path, both already present)
**DoD:** Per-story DoD above, section-by-section. Evidence committed to `sprints/sprint-3/evidence/3.2/`.

---

### US-3.1 (P0): Wire `hu_hybrid_retrieve` into `hu_memory_recall_for_contact`

**As a** person whose messages Mindy reads,
**I want** Mindy's per-contact memory recall to return memories that are semantically relevant to what I'm asking about — even when I use different words than the stored memory used,
**So that** Mindy can reference the right context from past conversations rather than drawing a blank because the keyword didn't match.

**Acceptance criteria:**

- AC-3.1.1: `src/memory/contact_memory.c:38` (`hu_memory_recall_for_contact`) is modified to call `hu_hybrid_retrieve` (`src/memory/retrieval/hybrid.c:77`) when both an `embedder` and a `vector_store` are non-NULL. The function signature of `hu_memory_recall_for_contact` must gain two new parameters: `hu_embedder_t *embedder` (nullable) and `hu_vector_store_t *vector_store` (nullable), inserted before the existing `hu_memory_entry_t **out` parameter. Verified by: `grep -n "hu_hybrid_retrieve" src/memory/contact_memory.c` returns at least one match after the change.

- AC-3.1.2 (backwards-compatible fallback): When `embedder == NULL` OR `vector_store == NULL`, `hu_memory_recall_for_contact` falls back to the existing BM25/FTS path (i.e., the code path that was in `contact_memory.c:38` before this change) with no behaviour change. No existing caller breaks. Verified by: `cmake --build --preset dev 2>&1 | grep -c "error:"` prints `0`; `./build/human_tests` (full suite, including all pre-existing memory tests) passes with 0 failures.

- AC-3.1.3 (RRF wiring): When `embedder` and `vector_store` are both non-NULL, the function delegates to `hu_hybrid_retrieve` with `HU_RRF_K = 60.0f` (the constant at `src/memory/retrieval/hybrid.c:13`), the contact-scoped memory backend, the provided embedder and vector store, the caller's query, and the caller's limit. The contact-prefix filter (lines 69-116 of `contact_memory.c`) is applied to `hu_hybrid_retrieve`'s output, not to the BM25 intermediate. Verified by: AC-3.2.5 (the integration test's semantic-recall assertion) passes.

- AC-3.1.4 (graph context optional): The `graph` parameter passed to `hu_hybrid_retrieve` from this call site is NULL unless the calling agent has a graph wired (the `hu_agent_bind_sqlite_graph` path at `src/agent/agent.c:1243`). When NULL is passed, `hu_hybrid_retrieve` correctly skips the graph context branch (`hybrid.c:100-120` is `#ifdef HU_ENABLE_SQLITE` guarded). Verified by: the integration test (US-3.2) passes with no graph wired, and `grep -n "graph" src/memory/contact_memory.c` shows only one reference (the parameter thread-through, not a graph initialisation call).

- AC-3.1.5 (error propagation): If `hu_hybrid_retrieve` returns any error code other than `HU_OK`, `hu_memory_recall_for_contact` propagates that error code to its caller without freeing result buffers it did not allocate. Verified by: a unit test in `tests/test_memory_features.c` (or a new test file) that passes a stubbed embedder whose `embed` vtable method returns `HU_ERR_NOT_SUPPORTED` and asserts the recall function returns `HU_ERR_NOT_SUPPORTED` rather than `HU_OK` with a null result.

- AC-3.1.6 (all callers updated): Every call site of `hu_memory_recall_for_contact` in the codebase is updated to pass the two new parameters. Verified by: `grep -rn "hu_memory_recall_for_contact" src/ include/ | grep -v "contact_memory\.c" | grep -v "\.h:"` shows no call site with the old 8-parameter signature; `cmake --build --preset dev` exits 0.

- AC-3.1.7 (header updated): `include/human/memory.h` (or wherever `hu_memory_recall_for_contact` is declared) is updated to reflect the new signature, including a brief comment explaining that `embedder` and `vector_store` are nullable and that NULL triggers BM25-only fallback. Verified by: `grep -A5 "hu_memory_recall_for_contact" include/human/memory.h` shows both `embedder` and `vector_store` parameters and a `/* nullable */` or equivalent annotation.

**Estimate:** M
**Dependencies:** US-3.2 (the integration test must exist so AC-3.1.3 can be verified end-to-end by AC-3.2.5)
**DoD:** Per-story DoD above. Evidence committed to `sprints/sprint-3/evidence/3.1/`.

---

### US-3.3 (P1): Daemon wires embedder and vector store into agent at init

**As a** developer running the h-uman daemon,
**I want** the daemon to initialise a `hu_embedder_t` (TF-IDF local embedder at `src/memory/vector/embedder_local.c:155`) and `hu_vector_store_t` (in-memory store at `src/memory/vector/store_mem.c:288`) and pass them to the agent when `cfg->memory.hybrid_recall` is `true`,
**So that** the hybrid recall path (US-3.1) is actually exercised in production without requiring manual wiring per call site.

**Acceptance criteria:**

- AC-3.3.1 (config field added): `include/human/config.h`'s `hu_memory_config_t` struct (`line 478`) gains a new `bool hybrid_recall` field. The field defaults to `false` in `src/config_merge.c` (the `cfg->memory.*` defaults block around line 282). Verified by: `grep "hybrid_recall" include/human/config.h` returns one match; `grep "hybrid_recall" src/config_merge.c` returns one match setting it to `false`.

- AC-3.3.2 (config parsed): `src/config_parse.c` reads `memory.hybrid_recall` from the JSON/TOML config file and sets `cfg->memory.hybrid_recall` accordingly. When the key is absent from the config file, the value remains `false` (the default from AC-3.3.1). Verified by: a unit test in `tests/test_config_parse.c` (or nearest equivalent) that parses `{"memory":{"hybrid_recall":true}}` and asserts `cfg.memory.hybrid_recall == true`; and a second assertion for an empty config object yielding `false`.

- AC-3.3.3 (daemon initialises embedder and vector store when enabled): In the daemon initialisation path (identified by `grep -n "agent_init\|hu_agent_init\|embedder_local_create\|vector_store_mem_create" src/main.c`), when `cfg->memory.hybrid_recall == true`, the daemon calls `hu_embedder_local_create` and `hu_vector_store_mem_create` and passes the results through to `hu_memory_recall_for_contact`'s call sites (or stores them on the agent for retrieval at recall time). Verified by: `grep -n "hu_embedder_local_create\|hu_vector_store_mem_create" src/main.c` (or the daemon entry-point file) returns at least one match after the change.

- AC-3.3.4 (daemon does NOT initialise when disabled): When `cfg->memory.hybrid_recall == false` (the default), neither `hu_embedder_local_create` nor `hu_vector_store_mem_create` is called during daemon init. The daemon starts, processes a turn, and shuts down cleanly without touching embedding code. Verified by: a test or a valgrind/ASan run with `hybrid_recall=false` in the config shows zero calls to `embed_wrapper` (the TF-IDF implementation function in `embedder_local.c`). Acceptable proxy: `./build/human_tests --suite=sprint3_hybrid_recall` has a sub-test that constructs a config with `hybrid_recall=false`, calls the daemon-init equivalent, and asserts the embedder pointer is NULL.

- AC-3.3.5 (embedder/vector-store init failure is non-fatal): If `hu_embedder_local_create` or `hu_vector_store_mem_create` returns a zero-context value (i.e., `.ctx == NULL`, indicating allocation failure), the daemon logs a warning and continues with `hybrid_recall` effectively disabled (falls back to BM25) rather than crashing. Verified by: a unit test that forces `alloc->alloc` to return NULL (via a failing-allocator test double) for the embedder create call, then asserts (a) the daemon does not abort/segfault and (b) the config field `hybrid_recall` is treated as effectively false for subsequent recall calls.

- AC-3.3.6 (memory ownership documented): The embedder and vector store created at daemon init are owned by the daemon, not the agent. `hu_agent_deinit` (`src/agent/agent.c:1000`) must NOT call their deinit methods. Verified by: `grep -n "embedder.*deinit\|vector_store.*deinit" src/agent/agent.c` returns zero matches for the new instances; the daemon's shutdown path calls `embedder.vtable->deinit` and `vector_store.vtable->deinit` explicitly. Confirmed by ASan showing no leaks on clean shutdown.

- AC-3.3.7 (no regression): `./build/human_tests` (full suite, all 10000+ tests) passes with 0 failures and 0 ASan errors with `hybrid_recall` defaulting to `false`. Verified by: CI run or local full-suite run with evidence log at `sprints/sprint-3/evidence/3.3/full-suite.log`.

**Estimate:** M
**Dependencies:** US-3.1 (the daemon needs the updated `hu_memory_recall_for_contact` signature to pass embedder/vector_store through)
**DoD:** Per-story DoD above. Evidence committed to `sprints/sprint-3/evidence/3.3/`.

---

### US-3.4 (P2): One-shot backfill script for existing scoped memories

**As a** user who already has memories in `~/.human/memory.db`,
**I want** a script `scripts/embed-existing-memories.sh` that reads every scoped memory, embeds its content via the local TF-IDF embedder, and inserts it into the vector store so that the semantic-recall path returns useful results immediately,
**So that** I don't have to wait for new conversations to accumulate before hybrid recall becomes useful.

**Acceptance criteria:**

- AC-3.4.1 (script exists and is executable): `scripts/embed-existing-memories.sh` exists and `test -x scripts/embed-existing-memories.sh` exits 0. Verified by that command.

- AC-3.4.2 (happy path — populated db): Given a `~/.human/memory.db` (or `$HU_MEMORY_DB` when set) with at least 5 scoped memories (rows where `session_id` is non-empty), running `scripts/embed-existing-memories.sh` exits 0 and prints a summary line matching `"Embedded [0-9]+ memories"` to stdout. Verified by: creating a test db with 5 rows in a tempdir, running `HU_MEMORY_DB=<tempdir>/memory.db scripts/embed-existing-memories.sh`, and asserting `$?` is 0 and stdout matches the pattern.

- AC-3.4.3 (idempotent): Running `scripts/embed-existing-memories.sh` twice in sequence on the same `memory.db` produces the same summary count on the second run (no duplicates inserted, no error). Specifically: the second run prints `"Embedded 0 memories (all already embedded)"` OR prints the same count as the first run with a note that entries were skipped. Verified by: running the script twice with `HU_MEMORY_DB=<tempdir>/memory.db` and asserting the second run exits 0 without error output on stderr.

- AC-3.4.4 (empty db — no crash): Running `scripts/embed-existing-memories.sh` against a valid but empty `memory.db` (zero rows in `memories` table) exits 0 and prints `"Embedded 0 memories"`. Verified by: `sqlite3 /tmp/hu_test_empty.db "CREATE TABLE IF NOT EXISTS memories (session_id TEXT, content TEXT);" && HU_MEMORY_DB=/tmp/hu_test_empty.db scripts/embed-existing-memories.sh` exits 0 and stdout contains `"0 memories"`.

- AC-3.4.5 (missing db — clear error, no crash): Running `scripts/embed-existing-memories.sh` when `$HU_MEMORY_DB` points to a non-existent file exits non-zero (exit code 1) and prints a human-readable error to stderr naming the missing path. Verified by: `HU_MEMORY_DB=/tmp/does_not_exist_hu_test.db scripts/embed-existing-memories.sh; echo "exit:$?"` prints `"exit:1"` and stderr contains the path `/tmp/does_not_exist_hu_test.db`.

- AC-3.4.6 (large db — no crash): Running the script against a `memory.db` with 1001 rows (generated by a helper fixture or `INSERT` loop) completes without segfault, OOM kill, or error exit. Elapsed time may be slow (TF-IDF is O(n)) but must complete. Verified by: generating 1001 rows in a tempdir db with `sqlite3`, running the script, and asserting `$? == 0`.

- AC-3.4.7 (shellcheck clean): `shellcheck scripts/embed-existing-memories.sh` exits 0 with no warnings (SC2034, SC2086, SC2155, etc. all clean). Verified by that command.

- AC-3.4.8 (uses $HU_MEMORY_DB env override): When `HU_MEMORY_DB=/custom/path/memory.db` is set in the environment, the script reads from that path rather than `~/.human/memory.db`. Verified by: running the script with `HU_MEMORY_DB=/tmp/hu_test_custom.db` (a pre-created db) and asserting the script does not touch `~/.human/memory.db` (e.g., its mtime does not change if it exists, or the script outputs the custom path in its summary).

**Estimate:** S
**Dependencies:** US-3.3 (the script calls into the same embedder the daemon uses; it may shell out to a purpose-built binary or invoke the daemon's embed subcommand — implementer chooses, but must not bypass the `hu_embedder_local_create` path)
**DoD:** Per-story DoD above. Evidence committed to `sprints/sprint-3/evidence/3.4/`.

---

## Non-goals

- We will NOT train or update any LoRA adapter in this sprint (that is Sprint 3 of the master plan, which is the NEXT sprint in the 6-sprint sequence).
- We will NOT add new AI provider integrations or change `hu_provider_t` vtable signatures.
- We will NOT implement persistent vector storage (the `hu_vector_store_mem_create` store at `src/memory/vector/store_mem.c:288` is in-memory and lives only for the daemon's lifetime; durability across restarts is deferred).
- We will NOT modify `hu_memory_config_t` fields beyond the single `hybrid_recall` bool (no new backend strings, no new URL fields).
- We will NOT touch the graph retrieval path beyond threading a NULL graph pointer through; `hu_agent_bind_sqlite_graph` wiring is out of scope.
- We will NOT backfill orphan `experience:*` rows (FU-7 from prior plan); the recall filter neutralises them and deletion is deferred.

---

## Open questions for stakeholder

1. **Embedder call site in backfill script (US-3.4):** The script needs to embed strings. The cleanest path is shelling out to `human-daemon embed --text "..."` (if such a subcommand exists or is cheap to add) rather than linking the C library from shell. Is a daemon `embed` subcommand acceptable scope for this sprint, or should the script use a minimal compiled helper? If neither exists, the implementer should build a `scripts/hu-embed-helper` binary as part of US-3.4.

2. **`hu_memory_recall_for_contact` signature change scope (US-3.1, AC-3.1.6):** There are likely callers in `src/agent/agent_turn.c` and `src/main.c` (modified files per git status at session start). Confirm the implementer has authority to update all call sites in a single commit, even if `agent_turn.c` changes are large — or should that be broken into a separate preparatory refactor story?

3. **Vector store persistence between backfill and daemon start (US-3.3 + US-3.4):** The in-memory vector store is populated at daemon start and lost on shutdown. The backfill script populates a different store (or a file). How should the daemon consume the backfill output — re-embed at startup from `memory.db`, or load a persisted vector store file? The current stories assume re-embed at startup (the daemon calls `hu_embedder_local_create` and populates from SQLite on init when `hybrid_recall=true`). If that is not the intended design, US-3.3 needs a revised AC-3.3.3.

---

`RESULT_product-owner=READY`
