---
title: human doctor — exit code reference
status: active
created: 2026-05-25
last_audit: 2026-05-25
sprint: 54
story: US-C3.9
---

# `human doctor` — exit code reference

`human doctor` reports the health of your installation. The exit code
tells scripts and CI integrations what happened, without parsing the
human-readable output.

## Exit code table

| Code | Symbol | Meaning |
|------|--------|---------|
| 0 | `HU_DOCTOR_EXIT_OK` | All checks passed (or are NA on this platform). Your install is healthy. |
| 1 | `HU_DOCTOR_EXIT_USER_ACTION` | One or more checks failed with a user-correctable cause: FDA denied, credentials missing, MLX server down, etc. Fix the indicated problem and re-run. |
| 2 | `HU_DOCTOR_EXIT_BUG_GRADE` | One or more checks failed with a bug-grade cause: binary corrupted, config unparseable, internal invariant violated. A developer must investigate. File an issue with the doctor output attached. |
| 64 | `HU_DOCTOR_EXIT_CRASH` | The doctor process itself crashed (SIGSEGV, abort, etc.). This means the doctor is broken, not the system it was checking. File an issue with the doctor's stderr attached. |

## Severity classification

How does doctor decide whether a FAIL is user-action (1) vs bug-grade (2)?

Each check returns a result with three fields:
- `verdict` — PASS, FAIL, or NA
- `reason` — human-readable explanation
- `detail_json` — optional structured detail

The exit-code aggregator inspects `detail_json` for the canonical
marker `"category":"bug"`. A check that writes this marker into its
detail_json opts into bug-grade severity. All other FAILs default to
user-action.

**Canonical marker forms** (whitespace tolerant):
- `"category":"bug"`
- `"category": "bug"`
- `"category" : "bug"`

Misspellings (`"bugs"`, `"Bug"`, `"severity":"bug"`) intentionally do
NOT match — this prevents accidental severity inflation. Authors must
opt in with the exact canonical form.

## Examples

### Healthy install (exit 0)

```
$ human doctor
✓ install — OK
✓ config_semantics — OK
✓ security — OK
✓ chatdb_readable — OK
✓ provider_smoke — OK
All checks passed.
$ echo $?
0
```

### User-action failure (exit 1)

```
$ human doctor
✓ install — OK
✗ chatdb_readable — FAIL: permission denied
  Open: System Settings → Privacy & Security → Full Disk Access,
  then enable "human".
$ echo $?
1
```

### Bug-grade failure (exit 2)

```
$ human doctor
✓ install — OK
✗ config_semantics — FAIL: config.json unparseable at line 42
  Run `human config validate` for details.
$ echo $?
2
```

### Crash (exit 64)

```
$ human doctor
Segmentation fault: 11
$ echo $?
64    # (set by the atexit handler; SIGKILL bypasses this)
```

## Stability guarantee

The four code values (0, 1, 2, 64) are **stable** across releases.
Changing any of them is a breaking change requiring a major version
bump. The constants live in `include/human/doctor.h` as
`HU_DOCTOR_EXIT_*` macros; the parity script
`scripts/check-doctor-exit-codes-in-sync.sh` enforces this docs/code
alignment at pre-commit time.

## Scripting recipes

### Block deploy on doctor failure

```bash
if ! human doctor; then
  echo "doctor failed; aborting deploy" >&2
  exit 1
fi
```

### Differentiate user-action vs bug-grade

```bash
human doctor
case $? in
  0)   echo "healthy" ;;
  1)   echo "user can fix" ;;
  2)   echo "page on-call; bug" ;;
  64)  echo "doctor itself crashed" ;;
  *)   echo "unexpected code: $?" ;;
esac
```

### Combine with `--json` (Sprint 50 US-C3.7 — Phase 2)

When `--json` ships, the JSON output's `aggregate` field will be
`"pass"` iff exit code is 0, else `"fail"`. The two channels carry
the same information at different granularities; the exit code is
the binary signal, the JSON is the structured signal.

## Related

- `sprints/sprint-54/designs/US-C3.9.md` — the design that locked
  this contract
- `include/human/doctor.h` — the C constants
- `src/doctor/exit_code.c` — the pure aggregator function
- `scripts/check-doctor-exit-codes-in-sync.sh` — the parity guard
- `tests/test_doctor_exit_codes.c` — the contract tests
