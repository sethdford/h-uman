---
title: "Sprint 52 — Architectural design (cross-cutting)"
created: 2026-05-24
sprint: 52
status: design-only
---

# Sprint 52 cross-cutting design

Per-story acceptance criteria live in `../stories.md`. This file
captures the cross-cutting design decisions.

## 1. The four-event vocabulary (LOCKED)

```c
/* include/human/telemetry/events.h */

typedef enum hu_telemetry_event_type {
    HU_TELEMETRY_INSTALL_COMPLETED = 0,
    HU_TELEMETRY_ONBOARDING_STEP_COMPLETED = 1,
    HU_TELEMETRY_DAEMON_CRASH = 2,
    HU_TELEMETRY_TICK_FIRED = 3,
} hu_telemetry_event_type_t;
```

Anything not in this enum CANNOT be recorded. The collector validates
the enum value and rejects unknowns (with a loud log — silent drop
would hide a coding error).

## 2. Pipeline contract (defense in depth)

```
┌─────────────────────────────────────────────────────────────┐
│  Caller (daemon tick, onboarding step, crash handler)        │
└─────────────────────────────┬───────────────────────────────┘
                              ↓
              hu_telemetry_record(event_type, fields)
                              ↓
            ┌─────────────────────────────────────┐
            │  Layer 1: Schema validator          │ ← drops unknown event types
            └─────────────────────────────────────┘
                              ↓
            ┌─────────────────────────────────────┐
            │  Layer 2: Field allowlist enforcer  │ ← drops disallowed fields
            └─────────────────────────────────────┘
                              ↓
            ┌─────────────────────────────────────┐
            │  Layer 3: Redactor (US-C4.4)        │ ← PII scrub on every string field
            └─────────────────────────────────────┘
                              ↓
            ┌─────────────────────────────────────┐
            │  Layer 4: Local JSONL buffer        │ ← ~/.human/telemetry.jsonl (0600)
            └─────────────────────────────────────┘
                              ↓
              (daily tick fires hu_telemetry_uploader_run)
                              ↓
            ┌─────────────────────────────────────┐
            │  Layer 5: Opt-in re-check           │ ← refuses if user revoked since
            │                                     │   last upload
            └─────────────────────────────────────┘
                              ↓
            ┌─────────────────────────────────────┐
            │  Layer 6: Redactor RE-RUN           │ ← defense in depth
            └─────────────────────────────────────┘
                              ↓
                  HTTPS POST → upload_url
                              ↓
                  Truncate local buffer
```

Layers 3 AND 6 BOTH run the redactor. Yes, this is redundant. Yes, we
do it anyway. If a code change accidentally bypasses the collector and
writes directly to the buffer file, the uploader's second pass catches
it. The cost is microseconds per event; the cost of being wrong is the
product's trust story.

## 3. Field allowlist (mechanical enforcement)

```c
/* Static per-event-type allowlist. Enforced by hu_telemetry_record. */
static const char *const ALLOWED_FIELDS_INSTALL_COMPLETED[] = {
    "version", "os_release", NULL,
};
static const char *const ALLOWED_FIELDS_ONBOARDING_STEP[] = {
    "step_name", "duration_seconds", NULL,
};
static const char *const ALLOWED_FIELDS_DAEMON_CRASH[] = {
    "signal", "log_tail", NULL,
};
static const char *const ALLOWED_FIELDS_TICK_FIRED[] = {
    "tick_name", "result", NULL,
};
```

`hu_telemetry_record` walks the caller's field map and drops any key
not in the allowlist for the event_type. Drop is LOUD (logs the
attempted key) — silently dropping a misnamed field would hide bugs.

## 4. Redactor profile

```c
/* include/human/telemetry/redact.h */

typedef enum hu_telemetry_redact_profile {
    HU_TELEMETRY_REDACT_STANDARD = 0,   /* default — all PII patterns */
    HU_TELEMETRY_REDACT_STRICT = 1,     /* + paths, even short tokens */
} hu_telemetry_redact_profile_t;

/* In-place redaction. Modifies the string buffer. Returns the new
 * length (always <= original). */
size_t hu_telemetry_redact(char *buf, size_t len,
                           hu_telemetry_redact_profile_t profile);
```

Crash log_tail uses STRICT profile — crash dumps contain whatever was
in memory at fault time, which can include the in-flight message that
triggered the crash. STANDARD profile is the default for everything
else.

## 5. Schema version pin

`schema_version=1` is locked at sprint-close. Future additions:

- Adding a NEW event type is a v2 change (requires a separate sprint
  and a fresh aspect-panel security review).
- Adding a NEW field to an existing event type is a v2 change.
- Removing a field from an event type is a v2 change (consumer side
  needs migration).
- Bug fixes to redactor / collector are v1-compatible.

## 6. Test fixtures

- `tests/fixtures/telemetry_pii_corpus.json` (NEW) — table of 50+
  known-PII strings used by US-C4.4 adversarial tests. Each entry
  has the input string + the patterns the redactor MUST scrub.
- `tests/fixtures/fake_telemetry_endpoint/` (NEW) — local fake HTTPS
  server used by US-C4.5.
- `tests/fixtures/synthetic_pii_events_1000.jsonl` — generated at
  test time (deterministic seed) for the integration adversarial
  test.

## 7. Build wire-up pattern

```cmake
list(APPEND HU_CORE_SOURCES
    src/telemetry/collector.c
    src/telemetry/consent.c
    src/telemetry/redact.c
    src/telemetry/uploader.c
    src/telemetry/cli.c
)

list(APPEND HU_TEST_SOURCES
    tests/test_telemetry_collector.c
    tests/test_telemetry_consent.c
    tests/test_telemetry_no_pii.c
    tests/test_telemetry_no_pii_integration.c
    tests/test_telemetry_uploader.c
    tests/test_telemetry_cli.c
)
```

`tests/test_main.c` declares + calls each runner. Test-source-gate-
symmetry rule applies — fixtures land in the same commit.

## 8. Sizing recheck

| Story    | Source LoC | Test LoC | Total |
|----------|-----------|----------|-------|
| US-C4.1  | ~150 (header + docs) | ~0       | 150   |
| US-C4.2  | ~400      | ~350     | 750   |
| US-C4.3  | ~200      | ~250     | 450   |
| US-C4.4  | ~250      | ~700     | 950   |
| US-C4.5  | ~420      | ~450     | 870   |
| US-C4.6  | ~400      | ~350     | 750   |
| **Total**| **~1820** | **~2100**| **~3920** |

Source LoC tracks backlog "~1800". Test density 1.15× — slightly
above baseline because the redactor adversarial corpus is
intentionally over-built (better to over-test the privacy contract).

## 9. Pre-commit gates (NEW, this sprint)

This sprint adds two new pre-commit checks:

1. `scripts/check-telemetry-schema.sh` — ensures
   `include/human/telemetry/events.h` enum matches
   `docs/specs/telemetry-schema-v1.md` field tables. Drift fails the
   commit.
2. `scripts/check-telemetry-uploader-needs-redactor-tests.sh` —
   refuses commits that modify `src/telemetry/uploader.c` if
   `tests/test_telemetry_no_pii.c` has no diff and no recently-added
   tests. (Enforces the US-C4.4 GATE.)

Both follow the established pre-commit pattern from `.githooks/pre-
commit`. The second is unusual but justified — the gate is high-
stakes enough to warrant a structural enforcement.

## 10. Where this design intentionally STOPS

- No metric aggregation on-device. We don't show the user a
  dashboard of their own telemetry — that would be a different
  surface (`human stats` perhaps in Sprint D).
- No event sampling. v1 is "all four event types, every fire."
  Volume is low (install_completed = 1 per install; onboarding
  steps = 5 per install; daemon_crash = rare; tick_fired = bounded
  by tick count). Sampling would add complexity without value at
  this volume.
- No client-side encryption of in-flight payloads beyond TLS. The
  static endpoint operator can read what they receive. Documented
  in the schema doc.
- No federated learning, no on-device aggregation that gets sent.
  Telemetry events are events, not derived analytics.
