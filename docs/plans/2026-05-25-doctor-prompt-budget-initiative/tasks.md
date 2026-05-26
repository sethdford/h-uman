# `human doctor prompt_budget` + init_outcome → dpo bridge — Tasks

Execution mode: **sequential by lead agent, single session**.
Per `.claude/rules/agent-task-sizing.md`, ~10 file touches is single-session
budget. No fanout.

## Part A — prompt_budget visibility

| # | Task | ACs | Status | Notes |
|---|------|-----|--------|-------|
| T1 | Extend `include/human/agent/prompt_budget.h`: add `hu_prompt_budget_field_stat_ext_t`, `hu_prompt_budget_snapshot_ext`, `hu_prompt_budget_snapshot_path`, `hu_prompt_budget_save_snapshot`, `hu_prompt_budget_load_snapshot`, `hu_prompt_budget_doctor_data_t` and `hu_prompt_budget_doctor_status_t` enum. | AC-1, AC-4, AC-5 | pending | Type defs only; impls follow in T2. |
| T2 | Implement T1's new functions in `src/agent/prompt_budget.c` (or new `prompt_budget_io.c` if size grows). Atomic write per Personal Model precedent. | AC-1, AC-4, AC-5 | pending | Use `human/json_util.h`; reference `hu_personal_model_save` for atomic shape. |
| T3 | Wire daemon flush call site in `src/daemon.c` near the existing verifier_metrics flush (~line 3482). Every 60s call `hu_prompt_budget_save_snapshot` with the agent's `hu_prompt_budget_t`. | AC-4 | pending | `touch src/daemon.c` after edit (D10). |
| T4 | Implement `hu_doctor_check_prompt_budget` + `hu_doctor_render_prompt_budget_json` in new `src/doctor/check_prompt_budget.c`. Populates doctor_data struct from snapshot; derives diag_items for human render; serializes data struct directly for JSON. Stale mtime > 120s → WARN. | AC-1, AC-2, AC-3, AC-5, AC-6 | pending | Both renderers consume the same doctor_data — pinned by AC-6 test. |
| T5 | CLI wiring in `src/main.c::cmd_doctor`: add `prompt_budget` to focused-subcommand dispatch block (~line 737), parse `--json` flag globally, suppress banner/color on stdout when set. Update `--help` text. | AC-5, AC-7 | pending | Errors still go to stderr in JSON mode. |
| T6 | Registry wiring: register `hu_doctor_check_prompt_budget` in `hu_doctor_registry_register_defaults` (`src/doctor/registry.c`). | AC-7 | pending | Default doctor flow includes the section. |

## Part B — init_outcome → dpo bridge

| # | Task | ACs | Status | Notes |
|---|------|-----|--------|-------|
| T7 | Create `include/human/ml/init_dpo_bridge.h` declaring `hu_init_dpo_bridge_record(hu_allocator_t*, hu_init_resolution_t, const char *draft, const char *target, int64_t resolution_ts)`. Gated entire header by `#ifdef HU_ENABLE_ML`. | AC-8, AC-9, AC-10 | pending | Doxygen documents the single-sided write contract + the `init_proposer_v1` source. |
| T8 | Implement bridge in `src/ml/init_dpo_bridge.c`. Looks up the dpo_collector singleton (or accepts it as parameter — TBD by reading dpo.c entry points), builds `hu_preference_pair_t` per D7, calls `hu_dpo_record_pair`. Returns warn-only on failure. | AC-8, AC-9, AC-12 | pending | Generic prompt template: `"Should I proactively message %s?"` with target_handle interpolated. |
| T9 | Extend static `pending_proposal_t` in `src/agent/init_outcome.c` to carry `char draft[1024]`. Populated when walking JSONL FIRED lines; passed to bridge on resolution. Non-schema change (internal struct only). | AC-8, AC-9 | pending | Bound is conservative; truncate beyond. |
| T10 | Wire bridge call site in `hu_init_outcome_resolve_pending` (`src/agent/init_outcome.c` ~line 756), wrapped `#ifdef HU_ENABLE_ML`. `#else` branch calls `hu_log_info_once` per process. | AC-10, AC-12 | pending | Bridge call AFTER successful resolution append (D9 — never block authoritative write). |

## Cross-cutting

| # | Task | ACs | Status | Notes |
|---|------|-----|--------|-------|
| T11 | CMake registration: add new sources to `src/CMakeLists.txt`. Part-A sources unconditional. Part-B bridge inside the existing `if(HU_ENABLE_ML)` block. Add test files to `HU_TEST_SOURCES` symmetrically. Run `bash scripts/check-test-source-gate-symmetry.sh` to confirm. | AC-13 | pending | Per `rules/test-source-gate-symmetry.md`, mismatch is a class of CI failure. |
| T12 | Contract tests for prompt_budget in `tests/test_doctor_prompt_budget.c` + forward-decl in `tests/test_main.c`. Cases: populated snapshot → AC-1 + AC-5 JSON shape; disabled → AC-2; enabled+missing-file → AC-3; AC-6 renderer-consistency (struct populated programmatically). Plus `tests/test_prompt_budget_atomic_save.c` for AC-4 (pre-block tmp slot with directory, verify prior snapshot survives). Reference `hu_doctor_check_prompt_budget` and `hu_prompt_budget_save_snapshot`. | AC-1, AC-2, AC-3, AC-4, AC-5, AC-6, AC-13 | pending | Fixtures via tmp dir + env override gated by `#ifdef HU_IS_TEST`. |
| T13 | Contract tests for bridge in `tests/test_init_dpo_bridge.c` (Part-B gated to match HU_ENABLE_ML). Cases: REPLIED → chosen-set + source check; IGNORED → rejected-set + source check; ML-disabled fallthrough (compile-time test that no symbol is required). Verify via in-memory SQLite COUNT(*). Reference `hu_init_dpo_bridge_record`. | AC-8, AC-9, AC-10, AC-11, AC-13 | pending | Use the existing init_outcome test fixture pattern. |
| T14 | Build + full suite verification: `touch src/daemon.c src/agent/prompt_budget.c src/agent/init_outcome.c && cmake --build build -j8 && ./build/human_tests` — must report 11900+ tests, 0 failures, 0 ASan errors. Confirm "Linking C executable human" + "Signing human binary" appear in build output (D10). Also try a minimal build (no HU_ENABLE_ML) to exercise AC-10. | all | pending | If link lines missing → re-touch + rebuild. |
| T15 | Live smoke test: `./build/human doctor prompt_budget --json` against a fixture snapshot; pipe to `jq .status` and `jq '.fields | length'`. Confirm shape. | AC-1, AC-5 | pending | Human-eyes check; programmatic check is in T12. |
| T16 | Spawn `spec-verifier` agent: pass spec path + impl files, get RESULT_spec-verifier=PASS/FAIL per AC. If any FAIL, file follow-up tasks; do NOT close the umbrella implementation task (#3 in TodoWrite). | all | pending | Per CLAUDE.md "Verify, don't assert". |

## Dependencies

- T2 ← T1
- T3 ← T2
- T4 ← T2
- T5 ← T4
- T6 ← T4
- T8 ← T7
- T9 ← (no deps; internal struct change)
- T10 ← T8, T9
- T11 ← T2, T4, T8, T12, T13
- T12 ← T2, T4
- T13 ← T8, T10
- T14 ← T1–T13
- T15 ← T14
- T16 ← T15

## Linear execution order

T1 → T2 → T3 → T4 → T5 → T6 → T7 → T8 → T9 → T10 → T11 → T12 → T13 → T14 → T15 → T16

Subsystem-by-subsystem (Part A first, then Part B, then cross-cutting).
Tests are written after impl to keep the doctor_data struct shape
settled before pinning.

## Out of scope (deferred)

- Initiative doctor section — `human initiative log`/`status` already
  cover this.
- Initiative `--json` mode — add when consumer needs it.
- B3 Phase 2 trim gate — needs observation data first.
- Pairing single-sided init_proposer rows into true preference pairs.
- Read-side filter change to surface single-sided rows in training.
- `init_outcome` schema extension to carry input context.
