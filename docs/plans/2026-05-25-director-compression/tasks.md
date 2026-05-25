---
title: "Prompt-Budget Compression — Tasks"
created: 2026-05-25
status: draft
sprint: TBD
reads: requirements.md, design.md
---

# Prompt-Budget Compression — Tasks

Each task is a small atomic commit. Tasks are sequenced — earlier
tasks unblock later ones. "Done when" maps back to an AC in
requirements.md.

## Task 1 — Per-field byte instrumentation (AC-1, AC-7)

**Scope:** ~80 LoC across 3 files.

Files:
- `include/human/agent/prompt_budget.h` — NEW. Declare
  `hu_prompt_field_stat_t` struct (name + bytes_contributed).
- `include/human/agent/prompt.h` — extend
  `hu_prompt_build_system` signature to accept optional
  `hu_prompt_field_stat_t *stats` (NULL-safe).
- `src/agent/prompt.c` — wrap each of the 27 appender calls with a
  `before = out_buf->len; ...; stats[i].bytes_contributed = out_buf->len - before;`
  pattern. NULL stats argument bypasses the bookkeeping entirely
  (preserves zero-overhead path for non-instrumented callers).

Tests:
- `tests/test_prompt_field_stats.c` (new) — build prompt from fixture
  context with known field sizes; assert per-field byte counts match.
- `tests/test_prompt_zero_change.c` (new) — build prompt with NULL
  stats; hash output; assert matches a golden hash (preserves AC-7).

**Done when:** both new tests pass; existing prompt tests still pass;
clang-tidy clean.

## Task 2 — Budget object (AC-2)

**Scope:** ~120 LoC across 2 files.

Files:
- `include/human/agent/prompt_budget.h` — declare opaque
  `hu_prompt_budget_t`, init / free / observe / field_is_dead /
  snapshot functions.
- `src/agent/prompt_budget.c` — NEW. The opaque struct holds 27
  rows of (name + counters + last-non-empty-at). Pure-function
  helpers operating on the struct.

Tests:
- `tests/test_prompt_budget_dead.c` (new) — feed synthetic
  observation sequences; assert `is_dead()` returns true iff
  (mean < threshold) AND (count ≥ min_samples). Edge cases:
  zero observations, all-zero observations, single non-zero
  observation in a sea of zeros.

**Done when:** new test passes; pure functions covered by tests;
no leaks reported by ASan.

## Task 3 — Config parsing (AC-4)

**Scope:** ~60 LoC across 3 files.

Files:
- `include/human/config.h` — add `hu_prompt_budget_config_t` struct
  + `prompt_budget` field on `hu_config_t`.
- `src/config_parse.c` — `parse_prompt_budget()` function, called
  from the main parser. Wire string-array parsing for the
  allowlist/denylist.
- `src/config_merge.c` — init the new fields to defaults in
  `hu_config_init_defaults`.

Tests:
- `tests/test_config_prompt_budget.c` (new) — parse fixture JSON
  with all keys; with no keys (assert defaults); with allowlist
  only.

**Done when:** new test passes; round-trip serialization works
(parse → serialize → parse yields identical struct); no schema
breakage to existing config tests.

## Task 4 — Trim gate (AC-3)

**Scope:** ~40 LoC in `src/agent/prompt.c`.

Wires Task 2's `field_is_dead` predicate into Task 1's
instrumented builder. When the config gate is enabled AND the
field is DEAD AND the field is not in the allowlist → skip the
appender entirely.

Tests:
- `tests/test_prompt_budget_trim.c` (new) — build prompt twice
  (gate off / on) against a fixture budget where 3 fields are
  marked DEAD; assert byte difference matches the sum of those
  fields. Confirm an allowlisted DEAD field is NOT skipped.

**Done when:** new test passes; AC-7 golden hash still matches for
gate-off case.

## Task 5 — Doctor check (AC-5)

**Scope:** ~80 LoC across 3 files.

Files:
- `include/human/doctor/check_prompt_budget.h` — NEW. Vtable
  declaration matching `check_provider.h` pattern.
- `src/doctor/check_prompt_budget.c` — NEW. Reads the global
  budget singleton, snapshots, emits detail_json with per-field
  table.
- `src/doctor/registry.c::register_defaults` — add as the 13th
  default check.

Tests:
- `tests/test_doctor_check_prompt_budget.c` (new) — register the
  check; run with synthetic budget; assert detail_json schema:
  `{"fields": [{"name": "...", "mean_bytes": N, "dead": bool}, ...]}`.

**Done when:** new test passes; `./build/human doctor` lists the
new check; `./build/human doctor --json` shows the prompt_budget
entry in the checks array.

## Task 6 — Silent-failure diagnostic (AC-6)

**Scope:** ~20 LoC in `src/agent/prompt.c`.

Adds the one-shot info log when `cfg->prompt_budget.enabled=false`
at first invocation, matching the pattern from commit `48372778`
(LoRA gap diagnostic) and the rule at
`~/.claude/rules/silent-config-gated-subsystems.md`.

Tests:
- `tests/test_prompt_budget_disabled_warn.c` (new) — mock the log
  sink; build prompt with gate off; assert log contains
  "prompt_budget" + "prompt_budget.enabled=true". Build again;
  assert log does NOT fire again (one-shot guard).

**Done when:** new test passes; the static `atomic_bool` guard is
reset-able for the test runner.

## Task 7 — Microbenchmark (R-1 mitigation, optional)

**Scope:** ~50 LoC in `tests/bench_prompt_build.c` (new).

Measures `hu_prompt_build_system` invocation cost in two modes:
- NULL stats (zero-overhead baseline)
- non-NULL stats (per-field bookkeeping)

Asserts the overhead is < 10% on a fixture with realistic context.
Fails the build if the regression budget is exceeded — guards
against future changes that bloat per-field cost.

**Done when:** benchmark runs and reports both numbers; CI wires it
to fail when delta > 10%.

## Sequencing Summary

```
Task 1 (AC-1, AC-7)  ─┐
                      ├─► Task 4 (AC-3) ─► Task 5 (AC-5)
Task 2 (AC-2)        ─┘
Task 3 (AC-4)         ─► Task 4 (depends on cfg field)
Task 6 (AC-6) — anytime after Task 3
Task 7 — anytime after Task 1
```

Tasks 1 + 2 + 3 can run in PARALLEL (no shared files). Task 4
needs all three. Tasks 5 / 6 / 7 can run in parallel after their
deps land.

## Estimated Size

| Task | LoC | Tests | Time |
|---|---|---|---|
| 1 | ~80 | 2 | 1 day |
| 2 | ~120 | 1 | 1 day |
| 3 | ~60 | 1 | 0.5 day |
| 4 | ~40 | 1 | 0.5 day |
| 5 | ~80 | 1 | 0.5 day |
| 6 | ~20 | 1 | 0.25 day |
| 7 | ~50 | n/a | 0.25 day |
| **Total** | **~450** | **7 tests** | **~4 days** |

Single-sprint scope. Right-sized for one implementer or one tech-
lead + one IC pairing.

## Out of Scope (explicit)

- Recency-rearrangement experiment (separate spec).
- LLM-driven compression (Phase 2+).
- Audit B (manual prompt inspection) — pure investigation, no code.
- Audit C (echo-provider A/B) — separate spec.
- Initiative-layer build — has its own spec in
  `docs/plans/2026-05-25-initiative-layer/`.
