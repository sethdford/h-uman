# Sprint 32 — Audit script + post-mortem + regression pinning

**Branch:** `sprint-32-audit-script-and-postmortem`
**Sprint goal:** turn the 2026-05-12 incident into permanent muscle.
Promote the chat.db audit from a one-off to a checked-in tool, write
the post-mortem, and pin the two additional leaks the audit found.

## Background

The audit tool we built ad-hoc during Sprint 29/30 (`/tmp/imsg-audit*.sh`)
was the highest-leverage instrument we had:

- It surfaced the 3 historical leaks Sprint 30 pinned (rowids 56055, 56065, 56355).
- Promoting it to `scripts/audit-imessage-leaks.sh` makes future incidents
  detectable in minutes instead of hours.
- Running it again now caught **two more leaks** we had missed
  (rowids 56049, 56063 — both 2026-05-11, both G4 prompt-template
  label patterns). Sprint 30's guard would have caught them; we add
  verbatim regression tests so we never regress.

The post-mortem is a separate but linked deliverable: same incident,
same fix-set, but with the timeline, root cause, and prevention plan
written down so the next on-call doesn't re-derive it.

## Stories

### S1 — `scripts/audit-imessage-leaks.sh` permanent tool

**As an** operator
**I want** a checked-in script that scans `chat.db` for daemon leaks
**so that** I can run it after any leak suspicion or weekly via cron.

**Acceptance:**

- `scripts/audit-imessage-leaks.sh` exists, executable, `set -euo pipefail`.
- Reads `~/Library/Messages/chat.db` (read-only, `writefile` for dumps).
- Scans outbound messages with `attributedBody` since `--since` (default 2026-05-10).
- Optional `--contacts "+1...,+1..."` filter.
- Optional `--out-dir DIR` (default mktemp under `/tmp`).
- Signature scans for G1 (numbered analytical lists), G2 (model self-talk),
  G3 (third-person profile), G4 (prompt-template labels). G5/G6 are
  per-turn-context and not checked here.
- Exit 0 = clean, 1 = leaks found, 2 = setup error.
- `--help` prints usage from the file header.
- Verified live against the real `chat.db`: caught all 5 known leaks
  (56049, 56055, 56063, 56065, 56354+56355) plus the 2 newly found
  ones, with no false positives in 442 other outbound messages.

### S2 — Verbatim regression tests for rowids 56049, 56063

**As a** future maintainer
**I want** tests that pin the two newly-discovered leaks
**so that** any future regression in G4 detection is caught in CI.

**Acceptance:**

- `tests/test_response_guard.c` adds two new tests:
  - `guard_rejects_msg_56049_user_constraints_scene_direction_leak_verbatim`
  - `guard_rejects_msg_56063_persona_block_short_leak_verbatim`
- Each test loads the verbatim string extracted by the audit script
  and asserts `HU_GUARD_REJECT` + `detected_semantic_leak == true`.
- All response_guard tests pass (39 → 41).

### S3 — Post-mortem doc

**As an** engineering team
**I want** a written post-mortem of the 2026-05-12 CoT leak
**so that** the failure modes, fixes, and prevention plan are
captured in one place.

**Acceptance:**

- `docs/postmortems/2026-05-12-cot-leak.md` exists.
- Includes: summary, blast radius (5 leaks across 3 contacts),
  timeline (incident, audit, sprints 29-31), root cause analysis
  (guard returned `REWROTE` not `REJECT`; quality gate "MARGINAL"
  policy permitted send), fix matrix (G1-G6 detectors, opt-in `_ex`
  API), what-went-well, what-went-wrong, action items linking to
  remaining carry-overs (Sprint 33 wiring, persona-derived detector,
  per-turn model-state hygiene, quality gate tightening).
- Privacy: no real recipient identifiers, no real persona PII; uses
  `+1XXXXXXXXXX` redaction and "<recipient A/B/C>" naming.

## Definition of Done

- All response_guard tests pass (41/41).
- Both dev + minimal full builds pass with 0 ASan errors.
- `scripts/audit-imessage-leaks.sh` is executable and `bash -n` clean.
- Live audit run against `chat.db` returns the expected hit count.
- Post-mortem renders without broken links and contains no PII.
- Branch tagged `v-sprint-32-close` and cherry-picked to `h-uman` main.
