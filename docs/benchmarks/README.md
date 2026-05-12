---
title: "Public benchmark suite — how to run"
created: 2026-05-12
status: active
related:
  - ../plans/2026-05-11-init-14-public-benchmarks.md
  - ../plans/2026-05-11-sota-2026-massive-team-program.md
  - ../../include/human/eval_public_suites.h
---

# Public benchmark suite — how to run

Initiative #14 ships five publishable benchmark adapters wired into the existing
`hu_eval_*` runner. Each adapter ships a tiny held-out smoke fixture under
`tests/fixtures/benchmarks/<name>/smoke.json` (synthetic personas only, no PII)
and a checked-in regression floor that the CI gate enforces.

S1 (this drop) ships smoke gates only. Longitudinal full-mode runs, frontier
comparison rows (Gemini Personal Intelligence vs Claude Cowork), and a
`docs/benchmarks/<date>.md` published report are deferred to the next sprint —
see `docs/plans/2026-05-11-init-14-public-benchmarks.md` §Phase P4–P6.

## The five suites

| Suite | Measures | Reference |
|---|---|---|
| `longmemeval` | Long-term memory recall across temporal, multi-hop, single-hop, abstention, knowledge-update categories | [arXiv:2410.10813](https://arxiv.org/abs/2410.10813) |
| `locomo` | Long-conversation memory: session recall, cross-session, long-horizon, temporal reasoning | [arXiv:2402.17753](https://arxiv.org/abs/2402.17753) |
| `knowu` | Persona-knowledge probes: facts, preferences, relationships, constraints, goals | Provisional — pin-before-publish |
| `empa` | Empathy trajectory composite (directional alignment + cumulative impact + stability) | [arXiv:2603.00552](https://arxiv.org/abs/2603.00552) |
| `proagentbench` | Expected-utility proactivity gate: should-ping vs should-silence decisions | Provisional — pin-before-publish |

## Run a single smoke

The synchronous CLI loads the committed fixture, runs it against the configured
provider (or the test-mock provider when `HU_IS_TEST=1`), prints structured
JSON, and exits non-zero if the checked-in floor is breached.

```bash
./build/human eval public-benchmark longmemeval
./build/human eval public-benchmark locomo
./build/human eval public-benchmark knowu
./build/human eval public-benchmark empa
./build/human eval public-benchmark proagentbench
```

Persist the report next to the fixture for longitudinal tracking:

```bash
./build/human eval public-benchmark longmemeval \
    --output tests/fixtures/benchmarks/longmemeval/results.json
```

The writer is atomic (`tmp + fwrite + fflush + fsync + rename`), matching the
contract pinned by `tests/test_personal_model_atomic_save.c`.

## Run the smoke regression gate (CI)

```bash
./build/human_tests --suite=public-benchmarks
```

This runs every fixture against the mock provider and asserts:

1. Each fixture loads, parses, scores deterministically.
2. The pass rate clears the checked-in floor.
3. No PII anti-patterns leak into committed fixtures (the privacy scanner
   rejects SSN shapes, real email TLDs, and long digit runs).
4. The locked `HU_JOB_KIND_BENCHMARK = 7` ordinal is preserved (cross-initiative
   convention from `docs/plans/2026-05-11-sota-2026-massive-team-program.md`).
5. Five-benchmark fan-out wall-clock is well under the 30 s CI ceiling.

## Fixture privacy contract

Every fixture uses synthetic markers only:

- Users: `user_a`, `user_b`, `agent_a`
- Domains: `example.com`, `example.org`, `example.net`
- Projects/objects: `project_alpha`, `tool_x`, `city_alpha`, `ridge_loop`, …
- No real names, no real emails (anything outside `example.*`), no SSN shapes,
  no 10-consecutive-digit runs (phone numbers).

The privacy scanner (`hu_public_benchmark_check_fixture_privacy`) runs both
before the fixture is parsed and as a stand-alone regression test, so a
fixture that ever drifts toward PII is rejected at the loader before any
score is emitted.

## Methodology — smoke vs full

**Smoke (this drop):** Each fixture is constructed so that an honest scorer
produces a known, deterministic pass rate when paired with a competent
provider. The floor is set a small margin below that, so the gate fires on
real regressions (loader breaks, scorer drift, fixture corruption) while
tolerating harmless rounding.

**Full (deferred):** Full-mode runs against real providers, with category-aware
scoring per the W16 evaluation factories (`hu_evaluation_longmemeval`,
`hu_evaluation_locomo`, …), and frontier comparison rows are written into a
published `docs/benchmarks/<YYYY-MM-DD>.md` report by the
maintainer-dispatched workflow. The reproducibility contract
(`scripts/eval-reproduce.sh`) lands with that drop — see the design doc.

## Adding a new public benchmark

1. Add a new `HU_PUBLIC_BENCHMARK_*` enum value in
   `include/human/eval_public_suites.h`.
2. Add a row to `k_public_benchmark_table` in `src/eval_public_suites.c` with
   the fixture path and floor.
3. Add the fixture: `tests/fixtures/benchmarks/<name>/smoke.json` with ≥ 3 and
   ≤ 10 tasks, synthetic personas only.
4. Add a `subject_expected_behavior` test in `tests/test_eval_public_suites.c`.
5. Extend the corresponding prefix and name in
   `src/eval_benchmarks.c::prefix_for_type` / `::hu_benchmark_type_name`.

## Cross-initiative coordination

This initiative reserves scheduler ordinal `HU_JOB_KIND_BENCHMARK = 7` per
`docs/plans/2026-05-11-sota-2026-massive-team-program.md` §"hu_job_kind_t enum
allocation". Initiative #10 (episode storage + sleep consolidation) owns the
forthcoming `hu_job_kind_t` reform that will fold this ordinal into the shared
scheduler enum; the synchronous CLI does not call the scheduler today.
