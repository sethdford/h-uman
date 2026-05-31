# Design for US-1: GraphRAG memory-grounding activation and measurement

## Approach

US-1 activates h-uman's existing GraphRAG memory-grounding subsystem by verifying two pieces: (1) that autodream.c's background Leiden community-detection scheduler actually writes the `community_summaries` table on a daemon tick, and (2) that the graph_grounding module correctly loads those summaries and injects them into the prompt in SHADOW mode (logged but not injected) to measure the quality of the grounding context without risking production replies.

The design is **minimal and verification-first**: all three acceptance criteria are satisfied without new logic — they verify the existing wiring works end-to-end. The flag remains OFF by default, and a gate comment prevents accidental activation until Story D (blind A/B measurement) completes.

**Why this design, not alternatives:**

- **No new code**: The GraphRAG pipeline (autodream.c, graph_grounding.c, world_model_bridge.c) is already wired and functional. AC-1.1 through AC-1.3 are *verification* tasks, not feature builds. The only new artifact is a gate comment at agent_turn.c:1471 and SHADOW-mode measurement harness.
- **Fail-open, fail-safe**: Both graph_grounding.c (`hu_graph_ground_load`) and autodream_runner.c return `HU_OK` silently on missing data or config errors. This preserves production robustness while the SHADOW mode measurement captures ground truth about what summaries exist.
- **Measurement without risk**: SHADOW mode logs the graph_context_bytes that *would* be injected, then discards the loaded summary (sets `graph_ctx = NULL, graph_ctx_len = 0` at agent_turn.c:1480–1482). This decouples measurement from activation — we see the value without changing reply behavior.

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| `src/agent/agent_turn.c` | Add gate comment at line 1471; no behavior change | +2 |
| `tests/test_graph_grounding.c` | New test: `test_autodream_tick_populates_community_summaries` verifying ≥1 rows written | +45 |
| `tests/test_autodream.c` or new file | Fixture contact + synthetic message; assert row count > 0 after tick | +40 |
| `scripts/graph-grounding-shadow-capture.sh` | SHADOW-mode daemon runner; captures metrics to JSON | +80 |
| `sprints/sprint-1/evidence/US-1/shadow-run-metrics.json` | Output of shadow-capture script (populated by test) | +10 |

## Implementation steps (for the implementer agent)

### 1. Verify autodream writes community_summaries on a daemon tick (AC-1.1)

**File:** `tests/test_graph_grounding.c` (extend existing file)

The existing test `test_graph_ground_load_returns_contact_summaries` seeds the table and reads from it. **Add a new test** that:

- Creates an in-memory graph database with autodream schema
- Creates a fixture contact + one synthetic iMessage message in chat.db
- Calls `hu_autodream_runner()` with `HU_JOB_AUTODREAM_COMMUNITY` spec (which gates `enable_community_summaries = true`)
- Runs with a real `graph_handle` passed through the `hu_w7_facade_t`
- Asserts `SELECT COUNT(*) FROM community_summaries > 0` after the tick returns
- Confirms the inserted contact_id matches the fixture contact
- **ASan-clean**: all allocations freed via the test's `hu_allocator_t`

Naming: `test_autodream_tick_populates_community_summaries_for_contact`

Contract assertion: After calling the autodream runner with COMMUNITY phase enabled and a fixture contact present in chat.db, the database MUST contain ≥1 community_summary row with that contact's ID.

### 2. Run daemon in SHADOW mode and capture metrics (AC-1.2)

**File:** `scripts/graph-grounding-shadow-capture.sh` (new)

A shell script that:

- Sets `HU_GRAPH_GROUNDING=shadow` and `HU_AUTODREAM_TICK_ENABLED=true` in the environment
- Starts the daemon (via `./build/human` or `human-daemon service-loop`)
- Runs for ≥10 turns (simulates ≥10 message receive cycles OR uses a test harness with synthetic messages)
- Tails the daemon log for lines matching `shadow: \d+ graph_context bytes (not injected)` (the pattern from agent_turn.c:1477)
- Counts community_summaries rows in the running daemon's `~/.human/chat.db`
- Calculates `avg_bytes_per_summary = total_graph_context_bytes / summary_count`
- Outputs JSON to `sprints/sprint-1/evidence/US-1/shadow-run-metrics.json` with keys:
  ```json
  {
    "test_run_ts": "2026-05-30T...",
    "turns_completed": 10,
    "graph_context_bytes_total": 1250,
    "graph_context_bytes_per_turn": [124, 0, 200, ...],
    "community_summary_count": 5,
    "avg_bytes_per_summary": 250.0,
    "contacts_with_summaries": ["alice", "bob"]
  }
  ```

**For test harness (no live sends required):**

Use `HU_IS_TEST` guards in agent.c to skip actual iMessage send logic and inject synthetic test messages directly into the turn context. The SHADOW measurement will still capture graph_context metrics without needing real message sends or chat.db data.

### 3. Add gate comment at agent_turn.c:1471 (AC-1.3)

**File:** `src/agent/agent_turn.c` at line 1471, immediately before the `hu_graph_grounding_mode_t graph_mode = ...` line

```c
        /* GraphRAG activation gated on Story D blind A/B measurement.
         * SHADOW mode logs metrics; do not flip to ON without confirmed
         * improvement in blind-A/B human ratings of reply quality. */
        hu_graph_grounding_mode_t graph_mode = hu_graph_grounding_mode();
```

The comment is load-bearing: it documents the policy that prevents a future implementer from `s/HU_GRAPH_GROUNDING_OFF/HU_GRAPH_GROUNDING_ON/` without understanding the gate dependency.

### 4. Run full test suite (AC-1.4)

```bash
touch src/agent/agent_turn.c src/agent/graph_grounding.c tests/test_graph_grounding.c
cmake --build build --target human_tests -j8
./build/human_tests --suite=graph 2>&1 | tail -3
```

Expected output:
```
Results: N/N passed, 0 failed
```

Confirm **no ASan errors** in stderr.

## Risks

### Risk 1: Shared agent_turn.c collision with US-2 / US-3 (HIGH / MEDIUM)

**What could go wrong:** US-1 adds a comment at agent_turn.c:1471; US-3 adds flag handling around lines 1471–1485 (SALIENCE_LIVE trichotomy); US-2 may add wiring calls to agent_turn.c if contextual_bandit audit (AC-2.1) finds the module unwired. Parallel edits to the same ~15-line region produce merge conflicts or silently mis-apply the comment.

**Probability:** HIGH — the sprint's wave-planning rule explicitly calls this out (`stories.md` line 163–169).

**Impact:** MEDIUM — a merge conflict is discoverable and fixable before commit, but a silent mis-merge (comment placed in the wrong half-line) could ship a broken gate comment.

**Mitigation:**
- **Sequence strictly:** US-1 → US-3 → US-2 (or work in isolation with careful merge planning).
- **Merge test before closure:** After each task, `git status` to confirm the gate comment is syntactically intact and on the correct line (`grep -n "GraphRAG activation gated" src/agent/agent_turn.c | head -1`).
- **Artifact pinning:** Add a test assertion that the comment exists at the expected line (see Test Plan below).

### Risk 2: autodream writes nothing because daemon scheduler not running (MEDIUM / MEDIUM)

**What could go wrong:** AC-1.1's test assumes `hu_autodream_runner()` is called with the COMMUNITY job spec wired in. If the daemon scheduler is disabled, no autodream tick fires, `community_summaries` stays empty, and the test fails silently with "0 rows in table" rather than a clear "scheduler didn't run" signal.

**Probability:** MEDIUM — autodream_runner.c is wired to the scheduler (scheduler.c line 86 sets `cfg.enable_community_summaries = communities`), but if the scheduler itself is gated off or the job spec is never enqueued, the test sees empty results without diagnosing why.

**Impact:** MEDIUM — the test would fail, forcing investigation. But the investigation might happen during implementation, not verification.

**Mitigation:**
- **Test harness:** The test explicitly calls `hu_autodream_runner(&facade, &spec, ...)` with `spec.kind = HU_JOB_AUTODREAM_COMMUNITY`. This bypasses the scheduler entirely — the runner is invoked directly in the test, so the scheduler-not-running failure mode is impossible.
- **Observability:** If a future AC-1.2 daemon run sees 0 community_summaries, the SHADOW log will show `shadow: 0 graph_context bytes` repeatedly, signaling that autodream produced no data. The metrics JSON will have `community_summary_count: 0`, making the diagnosis obvious.

### Risk 3: graph_context_bytes logged but data isn't actually usable (LOW / MEDIUM)

**What could go wrong:** SHADOW mode logs `graph_context_bytes` (the size of the loaded summary buffer), but if the autodream summary-generation logic is corrupted or producing garbage text, the logged bytes don't indicate quality. A measurement showing "X bytes loaded" doesn't prove the summaries are semantically useful.

**Probability:** LOW — the community_summaries schema is pinned by tests, and `hu_graph_ground_load` has existing test coverage (test_graph_grounding.c lines 31–54).

**Impact:** MEDIUM — if the summaries are garbage, we'd measure fake humanness improvement in Story D and activate a broken subsystem.

**Mitigation:**
- **Visual inspection of shadow-run-metrics.json:** The script captures `contacts_with_summaries` (which contacts had summaries loaded). A future implementer can manually inspect `chat.db` to see what communities autodream actually generated and whether they're coherent.
- **Spot test:** AC-1.1's test seeds synthetic rows and confirms the load function retrieves them. AC-1.2's live run will reveal whether autodream *generated* good summaries via the human-in-the-loop blind A/B in Story D — that's the actual quality gate.
- **Forward-signal in test:** The new test in step 1 asserts that **loaded summaries match the contact_id of the query** (not that they're good, just that they exist and are correctly indexed). If the summaries are corrupted or null, this assertion fails.

### Risk 4: Reading from real chat.db during AC-1.2 test run (MEDIUM / SMALL)

**What could go wrong:** AC-1.2's daemon shadow-capture script reads from the user's live `~/.human/chat.db` (or a test fixture in a temporary directory). If the script is run against the user's real database, and autodream tries to consolidate chat edges, it could corrupt the user's actual graph with test-generated artifact nodes.

**Probability:** MEDIUM — the risk depends on whether autodream's consolidation is idempotent and safe to run against a real database.

**Impact:** SMALL — graph corruption is recoverable (the graph schema is versioned and erasure.c has a cleanup path), but user trust is damaged.

**Mitigation:**
- **Use test fixture, not live chat.db:** The shadow-capture script will use `HU_IS_TEST` environment or a hardcoded test-database path (`/tmp/hu-test-shadow-$$.db`) rather than `~/.human/chat.db`.
- **Document the path in the script:** The script's header comments clearly state that it operates on a test database, not the user's production data.
- **Dry-run mode:** autodream has `cfg.dry_run = false` by default; the script sets it to `true` (no writes) so the daemon can be observed without side effects. (If autodream doesn't support dry-run yet, this risk becomes MEDIUM/MEDIUM and requires a feature gate addition.)

### Risk 5: SHADOW log line parsing is fragile (LOW / SMALL)

**What could go wrong:** The shadow-capture script greps for `shadow: \d+ graph_context bytes (not injected)` from the daemon log. If the log format changes or is disabled, the script collects zero metrics even though the daemon is running correctly.

**Probability:** LOW — the log line is baked into agent_turn.c:1477 and unlikely to change without explicit intent.

**Impact:** SMALL — the script would fail loudly (no metrics collected) rather than silently lying about results.

**Mitigation:**
- **Exact log-line assertion in test:** A unit test will assert that the log line is emitted exactly as expected (see Test Plan, "Artifact pinning" section).
- **Fallback SQL query:** If the log parse fails, the script can fall back to `SELECT COUNT(*) FROM community_summaries` to at least confirm non-zero row count, even if per-turn bytes are unavailable.

## Test strategy

### Unit tests (for AC-1.1 and AC-1.4)

**File:** `tests/test_graph_grounding.c` and/or `tests/test_autodream.c`

**New test cases:**

1. **`test_autodream_tick_populates_community_summaries_for_contact`** — Core AC-1.1
   - Setup: in-memory graph DB, schema created, fixture contact with synthetic message
   - Act: call `hu_autodream_runner(&facade, &spec)` with `HU_JOB_AUTODREAM_COMMUNITY`
   - Assert: `SELECT COUNT(*) FROM community_summaries > 0` AND first row's `contact_id` matches fixture
   - Verify: ASan-clean exit

2. **`test_graph_ground_load_shadows_bytes_when_in_shadow_mode`** — Indirect AC-1.2
   - Setup: SHADOW mode env var set, graph DB with 2 community_summaries for contact "test"
   - Act: call the full agent_turn path (or mock it) that loads graph_context
   - Assert: graph_ctx starts non-null, then is set to NULL (matching agent_turn.c:1481)
   - This test proves the SHADOW discard logic works without measuring live metrics

3. **`test_gate_comment_exists_at_agent_turn_1471`** — AC-1.3 artifact pinning
   - This is a "compliance test" that reads the source file
   - Assert: line 1471 of agent_turn.c contains "GraphRAG activation gated"
   - Prevents accidental comment deletion

### Integration / measurement tests (for AC-1.2)

**File:** `scripts/graph-grounding-shadow-capture.sh` + `sprints/sprint-1/evidence/US-1/shadow-run-metrics.json`

**Test harness execution (no live daemon required):**

- Compile with `HU_IS_TEST=1` (already default in CMake dev preset)
- Inject 10 synthetic messages into agent context without triggering real iMessage sends
- Each turn calls `hu_graph_grounding_mode()`, loads summaries, logs SHADOW line
- At end, query the in-memory graph for summary count
- Write metrics JSON

**Acceptance criteria for the measurement:**

- `graph_context_bytes_total > 0` (at least one summary was loaded in at least one turn)
- `community_summary_count >= 1` (the table has rows)
- `avg_bytes_per_summary = graph_context_bytes_total / community_summary_count` computes without divide-by-zero
- JSON is valid (validated with `jq . < shadow-run-metrics.json` exits 0)

### Full suite closure (AC-1.4)

```bash
./build/human_tests --suite=graph 2>&1 | tail -3
```

Expected: `Results: N/N passed, 0 failed` with no ASan reports.

## Acceptance criteria mapping

| AC | Coverage |
|---|---|
| AC-1.1 | `test_autodream_tick_populates_community_summaries_for_contact` — verifies ≥1 row in table after tick |
| AC-1.2 | `scripts/graph-grounding-shadow-capture.sh` generates `sprints/sprint-1/evidence/US-1/shadow-run-metrics.json` with all three fields (`graph_context_bytes`, `community_summary_count`, `avg_bytes_per_summary`) > 0 |
| AC-1.3 | Gate comment added at agent_turn.c:1471; `test_gate_comment_exists_at_agent_turn_1471` pins it |
| AC-1.4 | `./build/human_tests --suite=graph` returns 0 failures, 0 ASan errors |

## Sequence & dependencies

1. **Create test case (AC-1.1)** — write `test_autodream_tick_populates_community_summaries_for_contact` — should fail initially because test is new but autodream infrastructure already exists
2. **Implement test fixture** — seed graph schema, add synthetic contact/message to in-memory chat.db, call autodream runner
3. **Confirm test passes** — autodream writes ≥1 community_summary row ✓
4. **Add gate comment (AC-1.3)** — insert comment at agent_turn.c:1471
5. **Create shadow-capture script (AC-1.2)** — writes metrics JSON
6. **Run full graph suite (AC-1.4)** — confirm all tests pass, no ASan

**Rollback:** If any step fails:
- Remove gate comment (2 lines)
- Remove test cases (85 lines)
- Delete shadow-capture script
- Revert to previous commit

Production remains OFF (default `HU_GRAPH_GROUNDING=off`), so no risk of shipping broken subsystem.

## Risks summary table

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| agent_turn.c collision (US-1/2/3) | HIGH | MEDIUM | Sequence strictly; merge test confirms comment intact |
| autodream scheduler not running | MEDIUM | MEDIUM | Test calls runner directly; bypasses scheduler |
| summaries are garbage, not detected | LOW | MEDIUM | Visual inspection + Story D blind A/B gates activation |
| chat.db corruption from test | MEDIUM | SMALL | Use test-fixture DB path, HU_IS_TEST guards |
| SHADOW log parsing fragile | LOW | SMALL | Unit test asserts exact log format; fallback SQL query |

RESULT_tech-lead=DESIGN_READY
