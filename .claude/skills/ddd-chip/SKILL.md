# DDD Bounded-Context Chip Execution

Execute one behavior-preserving Domain-Driven Design refactor chip. Each chip is independently mergeable, lowers a measured ratchet, and produces ground-truth evidence via `/verify`.

**Scope:** One chip = ~8 mechanical sites, one bounded-context relocation, OR one multi-file function extraction. Larger work belongs in the Phase plan docs, not a chip.

## Pre-Chip Checklist

- [ ] Chip prompt contains: exact file paths, module names, concrete numbers (ratchet baseline before/after)
- [ ] No git operations will be performed (lead commits after merge in main session)
- [ ] All bash commands use **absolute paths** (worktree CWD resets between calls — see `rules/worktree-cwd-resets-in-bash.md`)
- [ ] ASan + all features enabled: `cmake --preset dev` generates `build/compile_commands.json`
- [ ] Original test suite passing: `./build/human_tests 2>/dev/null | grep Results:` recorded as baseline

## The Universal 6-Step Loop (per chip)

### Step 1: Characterization (if first-time refactor of a god-file)

For **carve-function** and **migrate-aggregate** archetypes only:

```bash
# Task 0: Fresh inventory — list all functions/statics in god-file WITHOUT editing
grep -n '^[a-z_].*(' src/daemon.c | head -30  # find all top-level function defs
wc -l src/daemon.c                              # total LOC
```

Record: function names, start lines, LOC, threading shape, static globals reachable from each function. This inventory is read-only — no edits yet.

Then, write a **characterization harness** test that exercises one tick of the god-function in isolation:

- `tests/test_daemon_service_loop_characterization.c` — drives one invocation of `hu_service_run()` against a fixture (mock bus, fixture agent, mock callbacks)
- Assert CURRENT observed side-effects (bus events sent, reply dispatches, checkin emissions, memory state)
- This test MUST PASS on the un-refactored code — it defines "behavior preservation"
- All extraction chips rely on this test staying green

### Step 2: Build & Verify Before Moving a Line

```bash
touch /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/src/daemon.c
cmake --build /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/build --target human -j8
cmake --build /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/build --target human_tests -j8
/Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/build/human_tests 2>/dev/null | grep 'Results:'
```

Capture the `Results: N/N passed` line — this is your baseline evidence.

### Step 3: Apply the Chip (by archetype)

Pick ONE of the three archetypes. Each has concrete steps below.

#### Archetype: Relocate-Cluster (E1)

**When:** Moving N=3-14 cohesive `.c` files from `src/*.c` root into a bounded-context subdirectory.

**Steps:**
1. `mkdir -p /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/src/<context>/`
2. For each file: `git mv /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/src/X.c /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/src/<context>/X.c` (path change only, no edits)
3. Update `CMakeLists.txt`: change `src/X.c` → `src/<context>/X.c` for each file in the group
4. Touch all moved files and rebuild (see Step 2)
5. Record the old `ROOT_BASELINE` (measure with `bash scripts/check-no-new-root-files.sh`) and new baseline (should decrease by group size)
6. Verify tests pass and characterization test (if relevant) still green

**Ratchet:** `ROOT_BASELINE` in `scripts/check-no-new-root-files.sh` lowers by the number of files moved.

#### Archetype: Carve-Function (E2)

**When:** Extracting N~5-8 cohesive sub-functions from a 3,000+ LOC god-file into separate modules while preserving behavior exactly.

**Hard constraint:** Define a **gov-budget mutator API** first (if god-file mutates shared state).

**Steps:**
1. **Task 1 (already done or skip if re-entering a started chip):** Characterization harness written, green on unmodified code.
2. **Task 2:** Establish mutator API — if `daemon.c` mutates `gov_budget`:
   - Define `hu_daemon_gov_*` mutators in `include/human/daemon/common.h`
   - Implement in new `src/daemon/daemon_peripheral_gov.c`
   - Replace all direct `gov_budget.field = X` writes in `daemon.c` with mutator calls
   - Rebuild: should stay green (refactor, no logic change)
3. **Task 3+N:** Extract one sub-function per edit cycle:
   - Declare public function in `include/human/daemon/<unit>.h`
   - Move function + its static local data to `src/daemon/<unit>.c`
   - Register in `CMakeLists.txt` (add `src/daemon/<unit>.c` to source list)
   - Replace direct calls in `daemon.c` with public function calls
   - Touch all files and rebuild
   - Verify characterization test still green (behavior preserved)
   - Commit one per edit: `refactor(daemon): extract <unit> from service loop (E2; daemon.c LOC X→Y)`
4. **Task FINAL:** Verify `daemon.c` residual < 800 LOC (entrypoint + composition only); measure `MAX_BASELINE` drop; commit the ratchet update: `ci(ddd): lower file-size ceiling after daemon carve (E2 complete)`

**Ratchet:** `MAX_BASELINE` in `scripts/check-file-size-ceiling.sh` lowers as daemon.c shrinks.

#### Archetype: Migrate-Aggregate (E3)

**When:** Moving N=1-5 related domain modules from raw SQL + `hu_sqlite_memory_get_db()` calls behind a per-aggregate repository interface.

**Steps:**
1. **Exemplar (first aggregate):** Pick a tightly-scoped aggregate (e.g., `boundaries`).
   - Task 1: Define `include/human/memory/boundary_repo.h` — backend-agnostic vtable + factory
   - Task 2: Implement `src/memory/repos/boundary_repo_sqlite.c` — reproduce EXACT SQL verbatim, use `SQLITE_STATIC` bindings
   - Task 3: Write `tests/test_boundary_repo.c` with `:memory:` fixture; assert pre/post contract
   - Task 4: Register in `CMakeLists.txt` inside `if(HU_ENABLE_SQLITE)` block
   - Task 5: Migrate domain caller(s) (e.g., `src/protective.c`):
     - Replace raw SQL calls with repo interface calls
     - Remove `#include <sqlite3.h>`
     - Add `#include "human/memory/boundary_repo.h"`
   - Task 6: Rebuild and verify tests pass
   - Task 7: Lower `BASELINE` in `scripts/check-sqlite-includer-ratchet.sh` by 1 (fewer SQLite includers)
   - Commit: `refactor(memory): wire boundary_repo interface behind aggregate (E3; sqlite3.h includers 110→109)`
2. **Follow-on aggregates:** Repeat the 6-task pattern for next 3-5 related tables (emotional, relational, opinions, narrative, style, scheduling).
3. **Deferred:** Proactive-checkins cluster (~47 call sites) — blocked on E2 extracting checkins function first.

**Ratchet:** `BASELINE` in `scripts/check-sqlite-includer-ratchet.sh` lowers by 1 per migrated domain caller.

### Step 4: Measure the Ratchet Drop

After applying the chip, measure the **before** and **after** of the relevant ratchet:

```bash
# Relocate-Cluster: root file count
bash /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/scripts/check-no-new-root-files.sh

# Carve-Function: largest file size
bash /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/scripts/check-file-size-ceiling.sh

# Migrate-Aggregate: SQLite includers
bash /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/scripts/check-sqlite-includer-ratchet.sh
```

Record the BEFORE and AFTER numbers. The ratchet script will tell you the current baseline and today's count. They should match or the chip lowers it.

### Step 5: Run Verification Agent (`/verify`)

Dispatch the verifier agent with:
- **Task:** Prove the chip's behavior-preservation contract
- **Evidence contract:** 
  - Code compiles cleanly (no warnings, no errors)
  - Full test suite passes: `./build/human_tests 2>/dev/null | grep 'Results: N/N passed'`
  - Characterization test (if applicable) passes and output behavior unchanged
  - Ratchet script shows the baseline is still satisfied (or improved)

The verifier returns `RESULT_verifier=PASS` or `RESULT_verifier=FAIL`. FAIL → do not close the chip; surface the failure.

### Step 6: Integration-Done Verification

**Before closing the chip, confirm integration:**

For **relocate-cluster** or **carve-function:** Run a real caller grep to confirm the moved/extracted code is actually wired:

```bash
# Relocate: the new directory is referenced in build
grep -rn "src/<context>/" /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/CMakeLists.txt

# Carve: the extracted function has a real caller outside its own file
grep -rn "hu_daemon_<unit>" /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/src/daemon.c
```

For **migrate-aggregate:** The domain caller no longer includes `<sqlite3.h>`:

```bash
grep -rn "#include <sqlite3.h>" /Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/src/protective.c
# should return EMPTY (file deleted its sqlite3.h include, migrated to repo interface)
```

## Gotchas Checklist

- [ ] **cmake-build-stale-binary** — Always `touch` source files BEFORE `cmake --build`, or you're testing old binaries (see `.claude/rules/cmake-build-stale-binary.md`)
- [ ] **SQLITE_STATIC not SQLITE_TRANSIENT** — Every `sqlite3_bind_*` call in repository layer must pass `SQLITE_STATIC` (null), never `SQLITE_TRANSIENT` (mandatory in h-uman)
- [ ] **Absolute paths in Bash** — Each Bash call spawns a fresh shell; `cd` does NOT persist. Use absolute paths: `/Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/path/to/file` (see `.claude/rules/worktree-cwd-resets-in-bash.md`)
- [ ] **ASan pthread false positives** — stack-local cross-thread structs on macOS arm64 may produce stack-use-after-scope false positives; fix by heap-allocating (see `.claude/rules/asan-pthread-stack-aliasing-darwin.md`)
- [ ] **HU_IS_TEST guards must move verbatim** — Daemon carve (E2) has 88 existing `#ifdef HU_IS_TEST` guards; do NOT move or rescope them (moving breaks characterization test)
- [ ] **Ratchets only tighten** — The ratchet script checks today's count ≤ baseline. Do NOT manually edit baseline downward without the corresponding refactor.
- [ ] **Characterization harness BEFORE carve** — E2 requires `tests/test_daemon_service_loop_characterization.c` written and green BEFORE any extraction chip runs
- [ ] **No new root .c files (E1)** — `check-no-new-root-files.sh` blocks any staged .c at `src/` root when baseline=0 (Phase E1 complete)
- [ ] **Test/source gate-symmetry** — Any source gated by `if(HU_ENABLE_X)` in CMakeLists must have test file EITHER gated the same OR wrapped in `#ifdef HU_ENABLE_X` (checked by pre-commit hook)
- [ ] **Credentials never in .c** — Do NOT commit credentials, API keys, tokens into source; pre-commit will reject them

## Parallel Execution Rule

When dispatching multiple file-editing agents for the same phase:

- **Sequential, not parallel:** Chips in the same phase MUST run one at a time, verify after each, then commit. Do NOT fan out N agents editing the same `src/` tree simultaneously (worktree isolation has silently failed in this environment).
- **If fanning out:** PROVE isolation with `git status` canary after first agent completes (main tree should stay clean). Only then launch the rest.
- **Lead commits:** No agent pushes. The lead (main session) merges all agent branches AFTER /verify passes, then commits the ratchet update.

## Closure Checklist

- [ ] `/verify` returned `RESULT_verifier=PASS`
- [ ] Ratchet measurement recorded (BEFORE and AFTER numbers in task description)
- [ ] Integration confirmed (new code is actually called, not dead code)
- [ ] No silent failures (all return values checked, errors propagated or logged)
- [ ] Characterization test green (if applicable)
- [ ] Full test suite green: `Results: N/N passed`
- [ ] No ASan errors in verifier output
- [ ] Ratchet script output shows BASELINE ≥ today's count (improvement locked in)

## Related Artifacts

- **Verifier agent** — Reference via `/verify` after applying the chip
- **Critic agent** — Run after every 2-3 chips to catch cross-chip regressions
- **Ratchet scripts** — `scripts/check-*.sh` family (version control for numeric bounds)
- **Phase plan docs** — `docs/plans/2026-05-29-ddd-bounded-contexts/phase-E*.md` (higher-level roadmap for sequencing chips)
