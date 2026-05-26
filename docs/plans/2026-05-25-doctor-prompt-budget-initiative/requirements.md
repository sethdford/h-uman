# `human doctor prompt_budget` + init_outcome → dpo_pairs bridge — Requirements

## Context (REVISED 2026-05-25 after verification)

Originally this spec covered "human doctor" extensions for both `prompt_budget`
AND `initiative`. Code-level verification surfaced that:

1. **`prompt_budget` is in-memory-only**: B3 Phase 1 (commits fe6dd1e2 /
   07596628) added the accumulator + 27 named fields. No flush, no UI, no
   operator visibility. **Real gap.**

2. **`initiative` already has a human CLI**: `human initiative log [--last N]`
   and `human initiative status` already exist (`src/agent/init_outcome.c:766`,
   registered in `src/main.c:559`). They show verdict counts, mean confidence,
   last-fire time, and pretty-printed last-N entries. Adding `human doctor
   initiative` would mostly duplicate this. **Not the gap.**

3. **T8 outcome detection is already wired**: `hu_init_outcome_resolve_pending`
   (`src/agent/init_outcome.c:713`) walks the JSONL, queries chat.db for
   replies, and appends `{outcome:"replied"|"ignored"}` lines. Called every
   proposer tick from `src/daemon.c:13865`. Tests at
   `tests/test_init_outcome.c`. **Not the gap.**

4. **What IS missing — the learning-loop bridge**: resolution lines never reach
   `dpo_pairs`. Two dpo writers exist (`src/ml/dpo.c:106`,
   `src/ml/training_data_extractor.c:488`); neither references init_outcome.
   So the daemon RECORDS that Seth ignored or replied, but the model never
   LEARNS from those signals. This is the actual "actually learns who you
   are" thesis gap.

The revised spec covers ONLY the two real gaps:
- **A: prompt_budget operator visibility** (doctor section + flush)
- **B: init_outcome → dpo_pairs bridge** (close the learning loop)

## User stories

- **As an operator**, I want to run `human doctor prompt_budget` and see a
  per-field byte breakdown plus dead-field candidates, so I can decide whether
  to enable the Phase 2 trim gate and which fields it will affect.
- **As an operator**, I want `human doctor prompt_budget` to FAIL LOUD when
  the subsystem is config-gated off (naming the config key) OR when enabled
  but never observed, so I catch silent-no-op failures per
  `rules/silent-config-gated-subsystems.md`.
- **As an operator**, I want `human doctor prompt_budget --json` to emit
  machine-parseable output, so the dashboard / scripts can consume it
  without parsing human text.
- **As the proposer's model**, I want every "replied" / "ignored" outcome to
  land in `dpo_pairs` (single-sided pattern, mirroring the reaction_handler
  precedent), so future LoRA / DPO training can use this signal — closing the
  loop from "we recorded an outcome" to "the model can train on it".
- **As a developer**, I want every new contract pinned by a test, so the
  doctor output and the dpo bridge can't silently drift away from
  underlying state.

## Acceptance criteria

### A. prompt_budget operator visibility

- [ ] **AC-1**: Running `human doctor prompt_budget` with the subsystem
  ENABLED and ≥1 observation prints one diag item per non-zero field, in
  descending mean-bytes order, with `field_name`, `mean_bytes`, `samples`,
  `non_empty_count`.

- [ ] **AC-2**: Running `human doctor prompt_budget` with the subsystem
  DISABLED produces a single WARN diag item naming `cfg->prompt_budget.enabled`
  and instructing the operator how to enable. Exit code non-zero.

- [ ] **AC-3**: Running `human doctor prompt_budget` with the subsystem
  ENABLED but `observation_count == 0` produces a WARN diag item
  ("enabled but never observed — daemon may not be invoking the appender")
  distinct from AC-2. Exit code non-zero.

- [ ] **AC-4**: The daemon flushes the current `hu_prompt_budget_t` snapshot
  to `~/.human/prompt_budget.snapshot.json` every 60 seconds via an atomic
  tmp + fsync + rename write. Pinned by a test that pre-blocks the tmp slot
  with a directory and confirms a failed save preserves the prior snapshot
  (mirrors `test_personal_model_atomic_save.c`).

- [ ] **AC-5**: Running `human doctor prompt_budget --json` emits a single
  JSON object on stdout with `check`, `status` (`ok`|`disabled`|`quiet`|
  `error`), `summary` (`observation_count`, `field_count`,
  `snapshot_age_seconds`), `fields[]` (sorted by `mean_bytes` desc),
  `warnings[]`. Schema pinned by a contract test that parses the output.

- [ ] **AC-6**: Both human and JSON renderers consume the same internal
  `hu_prompt_budget_doctor_data_t` struct; the JSON renderer does NOT
  re-parse human-formatted strings. Test pins this by populating the struct
  programmatically and asserting both renderers produce consistent
  field counts / values.

- [ ] **AC-7**: `human doctor` (no subcommand) includes the prompt_budget
  section among its default registered checks AND `human doctor prompt_budget`
  works as a focused subcommand. Both code paths route through the same
  `hu_doctor_check_prompt_budget` function.

### B. init_outcome → dpo_pairs bridge

- [ ] **AC-8**: When `hu_init_outcome_append_resolution` writes a "replied"
  outcome AND `HU_ENABLE_ML` is defined AND the dpo_collector is available,
  a row is inserted into `dpo_pairs` with `chosen=draft`, `rejected=""`,
  `source="init_proposer_v1"`, `timestamp=resolution_ts_unix`,
  `prompt="<generic proactive-proposal template referencing target_handle>"`.

- [ ] **AC-9**: When the outcome is "ignored", the same path inserts with
  `chosen=""`, `rejected=draft`, same `source`/`prompt`/`timestamp`. Mirrors
  the existing single-sided reaction_handler pattern (see comment at
  `src/ml/dpo.c:88-99`).

- [ ] **AC-10**: The bridge is fully gated behind `HU_ENABLE_ML`. Builds
  with the flag OFF compile clean, run with init_outcome resolution
  unchanged, and the only observable difference is that no dpo row is
  written. No silent failure: the resolver logs once via `hu_log_info_once`
  on first resolution after start that ML is disabled.

- [ ] **AC-11**: Contract tests pin both single-sided shapes (replied →
  chosen-set, ignored → rejected-set) plus the ML-disabled fallthrough
  using the existing `tests/test_init_outcome.c` fixture pattern. Bridge
  write is verified by a SQL count: post-bridge `SELECT COUNT(*) FROM
  dpo_pairs WHERE source = 'init_proposer_v1'` equals the number of
  non-PENDING resolutions written in the test.

- [ ] **AC-12**: Bridge write does NOT block the resolution path on failure
  — a failed dpo insert logs warn-once and the resolution line is still
  appended to the JSONL (separation of concerns: outcome capture is the
  authoritative record; dpo is a derived signal).

### Cross-cutting

- [ ] **AC-13**: Each new code path has at least one contract test exercising
  both the happy path and one failure mode. Tests use the HU_IS_TEST-guarded
  pattern; no real network, no daemon process, no real chat.db.

## Non-goals

- **No initiative doctor section.** `human initiative log` and `human
  initiative status` already cover this.
- **No initiative --json mode in this spec.** Add when a consumer needs it.
- **No B3 Phase 2 trim gate.** This spec gives data for the decision;
  flipping the switch is a separate session.
- **No read-side change to dpo_pairs filtering.** The single-sided rows
  this bridge writes will be filtered out by `hu_dpo_iterate_pairs` (the
  4-byte minimum on both sides) until a future refactor pairs them or
  routes them to a separate signals table. This spec deliberately stops
  at "the data lands in the table"; the read side is out of scope.
- **No init_outcome schema extension** to carry the draft text into
  resolution lines. Draft is carried in-memory from the matching FIRED
  line via the existing `pending_proposal_t` (will be extended internally
  — non-schema-visible change).
- **No persistent storage refactor.** prompt_budget stays in-memory in
  the daemon; doctor reads via flush-to-disk (verifier precedent).

## Constraints

- C11 + `-Wall -Wextra -Wpedantic -Werror`. No new warnings.
- `hu_*` naming; no `SQLITE_TRANSIENT`.
- No new third-party deps. JSON via existing `human/json_util.h`.
- No silent failures: file read errors (non-ENOENT) surface as ERROR.
- Tests deterministic; full suite (~11900 tests) at 0 failures, 0 ASan.
- Pre-commit hooks (`check-test-source-gate-symmetry.sh`,
  `check-test-references.sh`) must pass.
- Bridge code respects test/source gate symmetry — gated behind
  `HU_ENABLE_ML` per `rules/test-source-gate-symmetry.md`.
- Snapshot writer is atomic per the Personal Model precedent (NOT the
  verifier_metrics non-atomic precedent — verifier's own write is
  vulnerable to torn reads, which we are not propagating).
- After editing `src/daemon.c`, `src/agent/prompt_budget.c`, or
  `src/agent/init_outcome.c`, run
  `touch <files> && cmake --build build --target human` per
  `rules/cmake-build-stale-binary.md`.

## Sequencing

1. **This spec** — doctor prompt_budget (A) + init_outcome→dpo bridge (B).
2. **Next session candidates** (any one): bridge read-side filtering
   change to actually surface single-sided init_proposer rows in training;
   schema extension of init_outcome lines to carry input context; B3
   Phase 2 trim gate (depends on observation data from this spec).
