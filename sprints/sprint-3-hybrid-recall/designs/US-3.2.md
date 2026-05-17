# Design for US-3.2: Integration test harness — daemon-driven, real SQLite, mock provider

## Approach

US-3.2 is the **verification foundation** for Sprint 3 (and the template for FU-3 in all subsequent sprints). The design treats the integration test as a *first-class artifact* — same C11 toolchain, same `human_tests` binary, same `HU_TEST_SUITE`/`HU_RUN_TEST` framework as the existing unit tests — but distinguished by three properties: (1) real on-disk SQLite, not `:memory:`; (2) end-to-end drive through `hu_agent_turn`; (3) a deterministic `HU_IS_TEST`-guarded mock provider with zero network I/O.

We **reuse existing pieces wherever possible** to minimise risk and keep the new surface small:

1. The mock provider already exists at `tests/test_e2e.c:40-109` (`mock_provider_t` / `mock_provider_vtable` / `mock_provider_create`). The deterministic canned response is `"mock response"`. We do **not** invent a new mock — we lift the same vtable into the integration test file (copy, not shared header, because tests own their fixtures and there is no `tests/support/` infrastructure today). AC-3.2.4 is satisfied because the existing mock makes zero network calls; we just need to also ensure the test asserts `HU_IS_TEST` is defined.
2. The contact-memory storage/recall pattern is established at `tests/test_memory_features.c:128-165` (`contact_memory_cross_contact_isolation`). We extend that pattern by (a) swapping `:memory:` for a `mkdtemp(3)` tempdir-backed file, (b) seeding more entries across multiple sessions, and (c) driving at least one recall **via `hu_agent_turn`** (not just direct `hu_memory_recall_for_contact`) to prove the full agent → memory path.
3. Agent construction follows the pattern at `tests/test_e2e.c:607-625` (`test_agent_from_config_basic`): zero-init `hu_agent_t`, call `hu_agent_from_config(...)` with the mock provider and the SQLite memory backend, run `hu_agent_turn(...)`, then `hu_agent_deinit(...)`. `hu_agent_from_config` already accepts an `hu_memory_t *memory` parameter (`src/agent/agent.c:206-208`) — we pass our tempdir-backed memory in.

The **alternative** would be to build a separate `human_integration_tests` executable with its own `main()`. We reject this: it doubles the CI surface, fragments where tests live, and the `--suite=` filter already gives clean isolation. Single binary, single registration path.

The test file uses `#ifdef HU_ENABLE_SQLITE` to guard the entire suite — same pattern as `test_memory_features.c:14` and `:101`. If SQLite is disabled, `run_sprint3_hybrid_recall_tests()` becomes a no-op that prints a SKIP line, matching how the existing tests behave.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `tests/integration/test_sprint3_hybrid_recall.c` | NEW. Test file with 4–6 test functions, top-of-file docstring template marker, embedded mock provider, tempdir helpers. | +400 |
| `tests/test_main.c` | Add `void run_sprint3_hybrid_recall_tests(void);` declaration (near line ~30) and a single `run_sprint3_hybrid_recall_tests();` call in `main()` (next to existing `run_*` calls). | +2 |
| `CMakeLists.txt` | After line `2827` (`list(APPEND HU_TEST_SOURCES tests/test_memory_features.c)`), add `list(APPEND HU_TEST_SOURCES tests/integration/test_sprint3_hybrid_recall.c)`. | +1 |
| `sprints/sprint-3/evidence/3.2/` | NEW directory. Implementer populates with run logs (filled in by verifier, not designer). | dir |

**No new headers, no new source files in `src/`, no CMake target.** Single new test source.

## Implementation steps (for the implementer agent)

Order matters — ship red first, then make it green.

1. **Create the directory and the file skeleton** with the AC-3.2.9 docstring (see "Template marker" below) and an empty `run_sprint3_hybrid_recall_tests(void)` body. Verify build still succeeds.
2. **Wire CMake**: add the `list(APPEND HU_TEST_SOURCES ...)` line. Add the declaration + call in `tests/test_main.c`. Build with `cmake --build --preset dev` — expect zero warnings under `-Werror`.
3. **Add the tempdir helper** (`make_sprint3_tempdir`) and the cleanup helper (`cleanup_sprint3_tempdir`). Cleanup MUST use `nftw(FTW_DEPTH | FTW_PHYS)` or an equivalent `unlink(memory.db)` + `unlink(memory.db-shm)` + `unlink(memory.db-wal)` + `rmdir(dir)` triple — SQLite WAL mode leaves sidecar files.
4. **Copy the mock provider** verbatim from `tests/test_e2e.c:40-109` into a local `static` block inside the new file. Add a `#ifndef HU_IS_TEST\n#error "..."\n#endif` near the top so AC-3.2.4 can grep `HU_IS_TEST` and find it.
5. **Add a tempdir-lifecycle helper struct** (`sprint3_fixture_t`) and a `fixture_setup`/`fixture_teardown` pair. Teardown runs unconditionally (call it from every test even on failure paths — the framework's `longjmp` on assertion failure means tests cannot easily defer; instead, *order test bodies so that all assertions come AFTER memory/agent are constructed, and teardown is the last call before the implicit return, with teardown also called from a per-test wrapper if practical*). The cleanest pattern: tempdir is **per-test**, not per-suite, so leak on one test cannot poison the next. See "Tempdir lifecycle" risk below.
6. **Write `test_sprint3_seed_and_query_via_recall_direct` (AC-3.2.3, AC-3.2.5, AC-3.2.6, AC-3.2.7)**: pre-populate ≥10 entries across ≥3 `session_id`s. Use distinct keyword clusters (outdoor / cooking / finance). Call `hu_memory_recall_for_contact` directly for the BM25-only path (AC-3.2.7) and for the cross-contact isolation check (AC-3.2.6). For the semantic-recall AC (AC-3.2.5), call recall with the embedder/vector_store wired — but **this requires US-3.1 to be merged for the assertion to pass green**. The implementer ships this assertion red; US-3.1's implementer flips it green.
7. **Write `test_sprint3_drives_agent_turn` (AC-3.2.4 + corroborates AC-3.2.3)**: construct `hu_agent_t` with mock provider + tempdir-backed memory + a `memory_session_id` set to a contact id. Call `hu_agent_turn(&agent, "outdoor plans", ...)`. Assert response is non-NULL (mock returns `"mock response"`). Inspect `agent.memory` post-turn to confirm a memory was stored under the session.
8. **Write `test_sprint3_tempdir_cleanup_is_complete` (AC-3.2.2)**: setup → teardown → assert the dir no longer exists via `stat()`. Belt-and-suspenders proof that cleanup works.
9. **Add the SQL-level pre-seed assertion (AC-3.2.3)**: after seeding, open the same `memory.db` via `sqlite3_open_v2` with `SQLITE_OPEN_READONLY` and run `SELECT COUNT(DISTINCT session_id) FROM memories;` — assert the returned count is ≥3 with `HU_ASSERT_GE` (note: framework has `HU_ASSERT_GE`, not `HU_ASSERT_INT_GTE`; the AC's literal name is a typo — `HU_ASSERT_GE` is the canonical equivalent).
10. **Run the suite** via `./build/human_tests --suite=sprint3_hybrid_recall`. Confirm `< 10s` (AC-3.2.8) and zero ASan leaks. Run twice in succession; confirm no `/tmp/hu_sprint3_*` residue (AC-3.2.2).
11. **Commit** with conventional-commit `test(integration): add sprint3 hybrid recall harness (US-3.2)`.

## Tempdir lifecycle (AC-3.2.2)

Pattern:

```c
/* Sketch only — implementer writes the real C */
static bool make_sprint3_tempdir(char out[PATH_MAX]) {
    snprintf(out, PATH_MAX, "/tmp/hu_sprint3_XXXXXX");
    return mkdtemp(out) != NULL;
}

static void cleanup_sprint3_tempdir(const char *dir) {
    if (!dir || !*dir) return;
    char path[PATH_MAX];
    /* SQLite WAL mode leaves three files; clean all */
    snprintf(path, sizeof(path), "%s/memory.db", dir);     unlink(path);
    snprintf(path, sizeof(path), "%s/memory.db-shm", dir); unlink(path);
    snprintf(path, sizeof(path), "%s/memory.db-wal", dir); unlink(path);
    rmdir(dir);
}
```

**Trap:** the test framework uses `longjmp` (see `tests/test_framework.h:20`) — when an assertion fails inside a test body, control jumps back to `HU_RUN_TEST` and the rest of the test body does **not** run. This means a naive `setup → assertions → teardown` will leak tempdirs on every assertion failure.

**Mitigation:** the test runs in a forked OS process? No — the framework runs in-process. So the contract is: **each test allocates its tempdir into a `static` variable, and the suite-level wrapper `run_sprint3_hybrid_recall_tests()` records the tempdir path before invoking `HU_RUN_TEST` and cleans it up after**. Concretely:

```c
static char g_sprint3_last_tempdir[PATH_MAX];

void run_sprint3_hybrid_recall_tests(void) {
    HU_TEST_SUITE("sprint3_hybrid_recall");
    g_sprint3_last_tempdir[0] = '\0';
    HU_RUN_TEST(test_sprint3_seed_and_query_via_recall_direct);
    cleanup_sprint3_tempdir(g_sprint3_last_tempdir);
    g_sprint3_last_tempdir[0] = '\0';
    HU_RUN_TEST(test_sprint3_drives_agent_turn);
    cleanup_sprint3_tempdir(g_sprint3_last_tempdir);
    /* ... etc ... */
}
```

Each test stores its tempdir path into `g_sprint3_last_tempdir` immediately after `mkdtemp`, so cleanup is guaranteed even if assertions long-jump. This is the cleanest C-equivalent of try/finally available to the framework.

## Mock provider (AC-3.2.4)

Reuse the existing pattern from `tests/test_e2e.c:40-109` — **do not redesign**. The vtable members needed by `hu_agent_turn`:

- `chat` → returns canned `"mock response"`
- `chat_with_system` → same
- `supports_native_tools` → returns `false`
- `get_name` → returns `"mock"`
- `deinit` → no-op

`HU_IS_TEST` guarding: add the following near the top of the integration test file so AC-3.2.4's grep finds the marker and the file is unambiguously test-only:

```c
#ifndef HU_IS_TEST
#error "test_sprint3_hybrid_recall.c is a test-only fixture; HU_IS_TEST must be defined"
#endif
```

The mock vtable itself need not be `#ifdef`-guarded — it is `static` and only reachable through this translation unit, which CMake only compiles into `human_tests` (which defines `HU_IS_TEST`).

## Agent setup

Pattern lifted from `tests/test_e2e.c:649-665` (`test_agent_turn_simple`):

```c
hu_allocator_t alloc = hu_system_allocator();
mock_provider_t mock_ctx;
hu_provider_t prov = mock_provider_create(&alloc, &mock_ctx);

hu_memory_t mem = hu_sqlite_memory_create(&alloc, db_path);  /* not :memory: */
HU_ASSERT_NOT_NULL(mem.vtable);

hu_agent_t agent;
memset(&agent, 0, sizeof(agent));
hu_error_t err = hu_agent_from_config(
    &agent, &alloc, prov,
    NULL, 0,                 /* tools */
    &mem,                    /* memory */
    NULL,                    /* session_store */
    NULL, NULL,              /* extra slots */
    "mock-model", 10, "mock", 4,
    0.7, ".", 1, 25, 50,
    false, 0, NULL, 0, NULL, 0, NULL);
HU_ASSERT_EQ(err, HU_OK);

agent.memory_session_id = "+18018285260";
agent.memory_session_id_len = strlen("+18018285260");

char *response = NULL; size_t response_len = 0;
err = hu_agent_turn(&agent, "outdoor plans", 13, &response, &response_len);
HU_ASSERT_EQ(err, HU_OK);
HU_ASSERT_NOT_NULL(response);
alloc.free(alloc.ctx, response, response_len + 1);

hu_agent_deinit(&agent);
mem.vtable->deinit(mem.ctx);
cleanup_sprint3_tempdir(dir);
```

Caveat: the exact `hu_agent_from_config` parameter list at `src/agent/agent.c:206-208` is long (more than 20 params). The implementer should copy it verbatim from `test_e2e.c:614-616` and only override `prov` and `&mem`. **Do not** invent new parameter semantics.

## Assertion helpers (AC-3.2.5/6/7)

The framework header `tests/test_framework.h:1-200` already provides:

| AC needs | Use |
|---|---|
| Substring check (AC-3.2.5: response contains `"Utah"`) | `HU_ASSERT_STR_CONTAINS(haystack, needle)` (line 102) |
| Cross-contact key prefix non-leak (AC-3.2.6) | iterate `entries[i].key` and `HU_ASSERT_STR_NOT_CONTAINS(key, "contact:+15550001234:")` (line 113) |
| BM25 result count ≥1 (AC-3.2.7) | `HU_ASSERT_GE(count, 1)` (line 81) |
| Count ≥3 distinct sessions (AC-3.2.3) | `HU_ASSERT_GE(distinct_session_count, 3)` — note AC text says `HU_ASSERT_INT_GTE` which does not exist; `HU_ASSERT_GE` is the canonical name |

No new macros required. **Do not invent project-specific macros for this story** — keep dependencies thin.

## Template marker (AC-3.2.9)

Top-of-file docstring (literal — implementer must include these three keywords for AC-3.2.9's `head -20` grep):

```c
/*
 * tests/integration/test_sprint3_hybrid_recall.c — US-3.2 integration test harness
 *
 * # isolation
 * Run this test alone:  ./build/human_tests --suite=sprint3_hybrid_recall
 *
 * # tempdir
 * Each test allocates a fresh tempdir under /tmp/hu_sprint3_XXXXXX via mkdtemp(3).
 * Cleanup runs unconditionally between tests (via the suite-level wrapper) and
 * removes memory.db, memory.db-shm, memory.db-wal, and rmdir's the parent.
 * Verify cleanup:  ls /tmp/hu_sprint3_* 2>/dev/null  (should be empty)
 *
 * # template
 * This file is the canonical template for FU-3 integration tests in all
 * future sprints. To author a new sprint's integration harness, copy this
 * file to tests/integration/test_sprintN_<feature>.c and adapt:
 *   - The seeded memory fixtures (the cluster keywords)
 *   - The recall queries and expected substrings
 *   - The suite name passed to HU_TEST_SUITE
 * Do NOT redesign tempdir lifecycle or the mock provider; reuse this pattern.
 */
```

## Risks

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| **(HIGHEST) Tempdir-cleanup leak under assertion-failure longjmp** — `tests/test_framework.h:20` uses `longjmp` on every failed assertion. If teardown is inline at the bottom of a test body, every flake leaves an artifact on `/tmp`, eventually filling CI disks. This is the *single largest source of integration-test flake in the industry* and AC-3.2.2 catches it explicitly. | HIGH | MEDIUM | The suite-level wrapper pattern (see "Tempdir lifecycle" above) — record the active tempdir in a `static` variable before each test, clean it after `HU_RUN_TEST` returns regardless of pass/fail. This is the same shape as Python's `tmp_path` fixture, expressed in C. Also: `atexit(cleanup_sprint3_tempdir, ...)` as a belt-and-suspenders, registered once per suite invocation. |
| **CI flake from real SQLite on noisy filesystem** — real disk I/O is slower and more variable than `:memory:`; on a loaded CI runner, `fsync` can stall, and the 10-second budget (AC-3.2.8) is tight. | MEDIUM | MEDIUM | Keep the fixture small (10 entries, not 1000). No `sleep()`. No subprocess. Disable SQLite synchronous mode for tests: open the DB and `PRAGMA synchronous=OFF; PRAGMA journal_mode=MEMORY;` (these are safe for ephemeral tempdir DBs that will be deleted seconds later). Avoid WAL mode for the test (`journal_mode=MEMORY` eliminates the `-wal`/`-shm` sidecars too, simplifying cleanup). |
| **Mock provider divergence from real provider behaviour** — `mock_chat` always returns `"mock response"` ignoring the input; if `hu_agent_turn` evolves to require the provider's response to drive memory storage, the mock could give false-green on AC-3.2.5. | LOW | MEDIUM | The AC-3.2.5 assertion deliberately checks the **stored memory content** (`"Utah"`), not the **provider response** — the path under test is memory recall, not provider behaviour. Document this in the test comment so future maintainers understand why the mock's response text is irrelevant. |
| **`hu_agent_from_config` signature drift** — the function takes 20+ parameters (`src/agent/agent.c:206-208`); any future refactor could break this test silently if a parameter is reordered. | LOW | MEDIUM | The test_e2e suite already exercises the same call site with the same args (`tests/test_e2e.c:614`), so a signature change would break both; this test adds no new exposure beyond what already exists. The implementer should copy the exact param list from `test_e2e.c`, not retype it. |
| **AC-3.2.5 cannot pass until US-3.1 ships** — the semantic-recall assertion is the *contract for US-3.1*; if it must be green on US-3.2 merge, the stories are inverted in dependency order. | MEDIUM | SMALL | This is by design (per the story description: "ship the test FIRST with stubbed assertions (red), then US-3.1 implementer can develop against it"). Mitigation: ship AC-3.2.5's assertion behind a `#ifdef HU_SPRINT3_SEMANTIC_RECALL_ACTIVE` guard, defined off by default; US-3.1's PR turns it on. **OR** ship the test with the AC-3.2.5 assertion in red and mark the suite as "expected to fail until US-3.1 merges" in the test docstring. Strongly prefer the second — red tests are visible, ifdef guards rot. The implementer should add a `printf("EXPECTED: this assertion is red until US-3.1 ships\n")` immediately before the assertion. |
| **ASan leak from `entries[]` array on assertion-failure longjmp** — `hu_memory_recall_for_contact` allocates an entries buffer the caller owns. If an assertion fires between recall and the free-loop, the buffer leaks and ASan fails the run. | LOW | SMALL | Order each test so the free-loop happens before assertions on individual entry fields where possible; alternatively, defer detailed inspection by copying the needed field (e.g., `strstr` for `"Utah"`) into a local before the free-loop and assert on the local. The existing pattern at `test_memory_features.c:122-124` does this correctly — copy it. |
| **`HU_ENABLE_SQLITE` off in some build presets** | LOW | SMALL | Wrap the entire test body in `#ifdef HU_ENABLE_SQLITE` exactly as `test_memory_features.c:14` and `:101` do; the suite header prints a SKIP message if disabled. AC verification commands all run on `--preset dev`, where SQLite is on by default (`CMakeLists.txt:8`). |

## Test strategy

This story *is* the test strategy. There is no production code under test in US-3.2 — the deliverable is the harness itself. Verification is meta-level:

- **AC-3.2.1**: `cmake --build --preset dev 2>&1 | tee build.log; grep -c "error:" build.log` returns `0`; `grep -c "warning:" build.log` returns `0`.
- **AC-3.2.2**: run the suite twice; `ls /tmp/hu_sprint3_* 2>/dev/null | wc -l` returns `0` both times.
- **AC-3.2.4**: `grep -n "HU_IS_TEST" tests/integration/test_sprint3_hybrid_recall.c` returns ≥1 line.
- **AC-3.2.8**: `/usr/bin/time -p ./build/human_tests --suite=sprint3_hybrid_recall` shows real time `< 10.00`.
- **AC-3.2.9**: `head -20 tests/integration/test_sprint3_hybrid_recall.c | grep -c -E "isolation|tempdir|template"` returns `3`.

The implementer's `/verify` step runs all the above and captures into `sprints/sprint-3/evidence/3.2/`.

## Acceptance criteria mapping

| AC | Where it's satisfied | How it's verified |
|---|---|---|
| AC-3.2.1 (file exists, compiles -Werror clean) | New file + CMake entry | `cmake --build --preset dev` exit 0; warning grep returns 0 |
| AC-3.2.2 (tempdir lifecycle) | `make_sprint3_tempdir` + suite-level cleanup wrapper | Run-twice + `ls /tmp/hu_sprint3_*` empty |
| AC-3.2.3 (≥10 entries, ≥3 sessions, varied keywords) | `test_sprint3_seed_and_query_via_recall_direct` seeding block | SQL `SELECT COUNT(DISTINCT session_id)` ≥ 3 (HU_ASSERT_GE) |
| AC-3.2.4 (deterministic mock, HU_IS_TEST-guarded) | Embedded `mock_provider_*` from `test_e2e.c:40-109` + `#ifndef HU_IS_TEST #error` guard | `grep HU_IS_TEST` ≥1; `HU_NO_NETWORK=1` run exits 0 |
| AC-3.2.5 (semantic recall returns `"Utah"`) | `test_sprint3_drives_agent_turn` or `_direct` semantic assertion (turns green only after US-3.1) | `HU_ASSERT_STR_CONTAINS(entry.content, "Utah")` passes |
| AC-3.2.6 (cross-contact non-leak) | `test_sprint3_seed_and_query_via_recall_direct` cross-contact loop | `HU_ASSERT_STR_NOT_CONTAINS(entry.key, "contact:+15550001234:")` for every entry |
| AC-3.2.7 (BM25-only fallback) | Dedicated sub-test calling recall with NULL embedder/vector_store | `HU_ASSERT_GE(count, 1)` after querying `"carbonara"` |
| AC-3.2.8 (< 10s total) | Small fixture, no sleeps, `PRAGMA synchronous=OFF`, `journal_mode=MEMORY` | `/usr/bin/time -p` wall < 10s |
| AC-3.2.9 (template marker docstring) | Top-of-file docstring containing `isolation`, `tempdir`, `template` | `head -20 | grep` returns all three keywords |

## Sequencing note for the Sprint 3 fleet

US-3.2 ships **first**, with the AC-3.2.5 semantic-recall assertion **red**. US-3.1's implementer then develops `hu_hybrid_retrieve` wiring against this fixture as the green target. US-3.3 and US-3.4 also gate on this fixture being mergeable.

The implementer should commit US-3.2 in a single PR titled `test(integration): add sprint3 hybrid recall harness (US-3.2, FU-3 template)`, with the body explicitly noting that AC-3.2.5 is expected-red until US-3.1 merges. The Sprint 3 lead must not block US-3.2 merge on AC-3.2.5 passing — that would deadlock the sprint.

---

## Highest risk (called out for user attention)

**Tempdir-cleanup leak under longjmp.** This is the #1 way integration-test harnesses rot in production C codebases that use setjmp/longjmp test frameworks. The suite-level wrapper pattern (record-tempdir-then-clean-after-`HU_RUN_TEST`) is the minimum acceptable mitigation. If the implementer skips it and inlines `cleanup_sprint3_tempdir` at the bottom of each test body, AC-3.2.2 will silently regress the moment any assertion in any test fails — and CI disks will fill. **The reviewer must verify the suite-level wrapper exists before approving the PR.**
