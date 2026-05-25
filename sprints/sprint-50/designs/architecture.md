---
title: "Sprint 50 — Architectural design (cross-cutting)"
created: 2026-05-24
sprint: 50
status: design-only
---

# Sprint 50 cross-cutting design

The per-story acceptance criteria live in `../stories.md`. This file
captures the architectural decisions that EVERY story leans on, so
the implementers don't re-derive them six times.

## 1. Check vtable

```c
/* include/human/doctor/check.h */

typedef enum hu_doctor_verdict {
    HU_DOCTOR_PASS = 0,
    HU_DOCTOR_FAIL = 1,
    HU_DOCTOR_NA   = 2,  /* platform-not-applicable; counts as PASS in aggregate */
} hu_doctor_verdict_t;

typedef struct hu_doctor_check_result {
    hu_doctor_verdict_t verdict;
    /* Borrowed string — points into a per-check static or a buffer the
     * check owns for the lifetime of the call. Registry never frees this. */
    const char *reason;
    /* Optional structured detail for --json. NULL = no detail. */
    const char *detail_json;
} hu_doctor_check_result_t;

typedef struct hu_doctor_check {
    const char *name;          /* stable identifier — used in --json + exit-code tests */
    const char *description;   /* one-line human-readable */
    /* Run the check. ctx is registry-provided (config, allocator, ...). */
    hu_doctor_check_result_t (*run)(struct hu_doctor_check *self, void *ctx);
    /* OPTIONAL — NULL means "no autofix available." Returns true if fix
     * was applied (so the registry can re-run the check). */
    bool (*fix)(struct hu_doctor_check *self, void *ctx, bool interactive);
    /* Per-check user data (cast to whatever the check stores). */
    void *user_data;
} hu_doctor_check_t;
```

### Vtable rules

- `run` MUST be deterministic for the same `ctx` — no hidden timekeeping,
  no PRNG. The `tick_freshness` check IS time-dependent but the time
  source comes through `ctx` so tests can pin it.
- `fix` MUST never touch user data without confirmation. Pinned by an
  adversarial test: the test registers a fake check whose `fix` calls
  `unlink("~/.human/cognition.db")` and asserts the dispatcher refuses
  with the "user-data-touching fix" guard.
- `name` MUST be a stable kebab-case identifier. Renaming breaks the
  `--json` consumer contract.

## 2. Registry

```c
typedef struct hu_doctor_registry hu_doctor_registry_t;

hu_error_t hu_doctor_registry_init(hu_allocator_t *alloc,
                                   hu_doctor_registry_t **out);

hu_error_t hu_doctor_registry_register(hu_doctor_registry_t *r,
                                       const hu_doctor_check_t *check);

/* Run every check sequentially in registration order. Writes
 * out_results[i] for each registered check. Returns OK even if some
 * checks FAILed — the aggregate verdict comes from the caller
 * inspecting out_results. */
hu_error_t hu_doctor_registry_run_all(hu_doctor_registry_t *r,
                                      void *ctx,
                                      hu_doctor_check_result_t *out_results,
                                      size_t *out_count,
                                      size_t cap);

void hu_doctor_registry_free(hu_doctor_registry_t *r);
```

### Registration site

A single `hu_doctor_registry_register_defaults(reg)` fn in
`src/doctor/registry.c` registers every check in a documented order:

```
1. install            (existing — verifies binary + config layout)
2. config_semantics   (existing)
3. security           (existing)
4. chatdb_readable    (NEW — US-C3.2)
5. provider_smoke     (NEW — US-C3.3)
6. mlx_adapter        (NEW — US-C3.4)
7. tick_freshness     (NEW — US-C3.5)
8. memory_health      (existing)
9. persona_blocks     (NEW — US-C3.6)
10. skills            (existing)
11. imessage          (existing)
12. verifier          (existing)
13. scheduler         (existing)
14. response_pipeline (existing)
```

Order matters for the user-facing output (we show foundational checks
first so the user fixes those before debugging downstream ones), but
NOT for correctness — each check is independent.

## 3. JSON output schema (v1)

Locked at sprint close. See US-C3.7 for the shape. Schema additions in
this sprint scope: NONE. v2 is a separate sprint (and a separate
flag — `--json=v2` — so consumers can opt in).

## 4. Exit-code table

| Code | Meaning                                | Triggered by                                |
|------|----------------------------------------|---------------------------------------------|
| 0    | All checks PASS                        | Aggregate "pass"                            |
| 1    | User-action-required FAIL              | At least one check FAIL, NONE are bug-grade |
| 2    | Bug-grade FAIL                         | At least one check FAIL where check returns `detail_json` containing `{"category":"bug"}` |
| 64   | Doctor itself crashed                  | Uncaught error in dispatcher                |

`docs/guides/doctor.md` and `src/doctor.c` constants are kept in sync
by `scripts/check-doctor-exit-codes-in-sync.sh` (US-C3.9).

## 5. `HU_IS_TEST` discipline

The smoke check (US-C3.3) is the highest-risk surface — a forgotten
`HU_IS_TEST` guard means tests bill the real provider. Mitigation:
`scripts/check-doctor-test-guards.sh` greps every
`src/doctor/check_*.c` file for `HU_IS_TEST` and refuses commit if any
NETWORK/SPAWN call site lacks the guard. Same shape as the existing
`check-test-references.sh` pre-commit hook.

## 6. Build wire-up pattern (per check)

Every new check follows the same wire-up:

```cmake
# CMakeLists.txt — added inside the existing src/doctor target
list(APPEND HU_CORE_SOURCES
    src/doctor/registry.c
    src/doctor/check_chatdb.c
    src/doctor/check_provider.c
    src/doctor/check_mlx.c
    src/doctor/check_tick.c
    src/doctor/check_persona.c
)

list(APPEND HU_TEST_SOURCES
    tests/test_doctor_registry.c
    tests/test_doctor_chatdb.c
    tests/test_doctor_provider.c
    tests/test_doctor_mlx.c
    tests/test_doctor_tick_freshness.c
    tests/test_doctor_persona_regression.c
    tests/test_daemon_tick_heartbeat.c
    tests/test_doctor_json.c
    tests/test_doctor_fix_mode.c
    tests/test_doctor_exit_codes.c
)
```

`tests/test_main.c` declares + calls each new runner. Both must be
done atomically with the check landing or
`.claude/rules/test-source-gate-symmetry.md` fires.

## 7. Test fixtures

- `tests/fixtures/doctor_persona_contact.json` — fully-populated
  personal-model JSON for "alice" used by US-C3.6.
- `tests/fixtures/doctor_pass_all/` — minimal `~/.human/` tree where
  every check PASSes; consumed by US-C3.7 and US-C3.9.
- `tests/fixtures/doctor_fail_provider/` — same tree but with
  `config.json::provider.api_key` removed; consumed by US-C3.7.
- `tests/fixtures/mlx_fake/` (existing) — fake-MLX HTTP server used by
  US-C3.4.

## 8. Implementation sizing recheck

| Story    | Source LoC | Test LoC | Total |
|----------|-----------|----------|-------|
| US-C3.1  | ~250      | ~150     | 400   |
| US-C3.2  | ~120      | ~180     | 300   |
| US-C3.3  | ~150      | ~250     | 400   |
| US-C3.4  | ~180      | ~280     | 460   |
| US-C3.5  | ~250      | ~400     | 650   |
| US-C3.6  | ~150      | ~200     | 350   |
| US-C3.7  | ~100      | ~250     | 350   |
| US-C3.8  | ~300      | ~300     | 600   |
| US-C3.9  | ~30       | ~200     | 230   |
| **Total**| **~1530** | **~2210**| **~3740** |

Source LoC tracks the backlog's "~1500 LoC" estimate. Test LoC at ~1.4×
source is consistent with this codebase's test-density baseline.

## 9. Where this design intentionally STOPS

What this sprint design does NOT yet specify (left for the
implementation sprint or for a future sprint):

- The exact wire format for `tick_heartbeat.jsonl` rotation /
  compression policy. Sprint scope: append-only, untrimmed.
- The `--watch` mode (continuous re-run with delta output). YAGNI for
  v2 — every story's "doctor as a one-shot" model is sufficient.
- A `--quick` mode that runs only the local checks (skipping network
  smokes). Likely a Sprint 51 follow-up once we see real-world latency.
- An i18n layer for FAIL messages. English-only is acceptable for the
  user base size at sprint-50 ship time.
