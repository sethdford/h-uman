# Sprint 32 — Review

**Branch:** `sprint-32-audit-script-and-postmortem`
**Stories:** S1 (audit script), S2 (regression tests), S3 (post-mortem).

## Demo

```bash
$ scripts/audit-imessage-leaks.sh --since 2026-05-10
audit: scanning 448 outbound messages since 2026-05-10...
2026-05-11 00:31:14|<recipient B>|rowid=56049 size=972  G1long=0 G2=0 G3=0 G4=2
2026-05-11 00:35:13|<recipient B>|rowid=56055 size=1097 G1long=0 G2=0 G3=1 G4=4
2026-05-11 00:43:38|<recipient B>|rowid=56063 size=665  G1long=0 G2=0 G3=0 G4=2
2026-05-11 00:45:21|<recipient B>|rowid=56065 size=2208 G1long=0 G2=0 G3=1 G4=2
2026-05-12 17:04:37|<recipient A>|rowid=56354 size=1908 G1long=4 G2=3 G3=2 G4=0
2026-05-12 17:07:38|<recipient C>|rowid=56355 size=1858 G1long=4 G2=3 G3=2 G4=0
audit: 6 flagged of 448 scanned
```

Six leaks across three recipients, two distinct signature classes
(G1+G2+G3 cluster vs. G4 cluster), 442 benign messages cleanly
unflagged. Two new historical leaks (56049, 56063) surfaced and pinned
as verbatim regression tests.

## Acceptance check

- [x] `scripts/audit-imessage-leaks.sh` exists, executable, `bash -n` clean.
- [x] Reads `chat.db` read-only via `writefile`.
- [x] `--since`, `--contacts`, `--out-dir`, `--help` flags work.
- [x] Exit 0/1/2 semantics verified live (1 with seeded leaks, 0 dry runs).
- [x] Two new verbatim regression tests added and passing
      (`guard_rejects_msg_56049_*`, `guard_rejects_msg_56063_*`).
- [x] All 41 response_guard tests pass (39 pre-32 + 2 new).
- [x] `docs/postmortems/2026-05-12-cot-leak.md` exists, no PII, all 6 leaks
      tabled, action items linked to sprints.

## What's in (Sprint 32)

- `scripts/audit-imessage-leaks.sh` (208 LOC bash, `set -euo pipefail`).
- `tests/test_response_guard.c` +2 tests pinning rowids 56049, 56063.
- `docs/postmortems/2026-05-12-cot-leak.md` (~140 LOC, full timeline,
  fix matrix, action items).
- `sprints/sprint-32/{stories,review,retro}.md`.

## Out of scope (deferred to Sprint 33)

- Wire `hu_response_guard_check_ex(...)` into production callers
  (`agent_stream.c`, `agent_turn.c`, `daemon.c`). Carry-over from Sprint 31.
- Quality gate `MARGINAL → REJECT` policy fix (length-anomaly ≥ 5x).
  Carry-over from Sprint 28.

## Out of scope (future)

- Persona-derived dynamic detector.
- Per-turn model-state hygiene.
- `selection-step` audit.
- CI/cron schedule for the audit script.
- Per-channel length thresholds, director-echo over multi-turn,
  length-anomaly EWMA smoothing.
