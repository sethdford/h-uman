---
title: "Sprint 52 — Sprint C / C4: Opt-in first-run + activation telemetry"
created: 2026-05-24
sprint: 52
branch: sprint-52-telemetry-optin
spec: docs/plans/2026-05-25-sprint-c-backlog.md (§C4, lines 201-251)
program: docs/plans/2026-05-25-sprint-c-backlog.md
status: planned (PO + design only — implementation deferred to a follow-up multi-session work unit)
---

# Sprint 52 — Opt-in telemetry

## Goal

We can't improve what we can't measure. But the entire product moat is
**"privacy by architecture, not by settings"** (CLAUDE.md M3). Sprint
52 collects activation-and-crash signal that genuinely helps us improve
the install/onboard surface — WITHOUT betraying that thesis.

The design discipline is structural:
- **Opt-in only**, with an unambiguous prompt during onboarding
- **Locally buffered** in plaintext JSONL the user can `cat` before
  anything ever ships
- **Tiny event vocabulary** — install_completed, onboarding_step,
  daemon_crash, tick_fired. Nothing else. Ever.
- **Adversarial PII test** — the privacy contract is enforced by a
  failing test if any PII leaks through the redactor.

If we ship privacy mistakes here, the product's core trust story is
gone. The aspect-panel reviewer for this sprint should run with
`--aspects security` weighted higher than normal.

## Scope discipline

- **In scope:** event collector with redaction, local JSONL buffer,
  opt-in prompt in onboarding (already specced in Sprint 51 §US-C2.2),
  daily batch uploader, `human telemetry status` + `human telemetry
  revoke` CLI verbs, adversarial PII-redaction tests.
- **Out of scope:** message content (NEVER), contact handles (NEVER),
  persona fact text (NEVER), real-time streaming telemetry, metrics
  dashboards on the user's machine (telemetry is for US, not the user),
  advertising / third-party analytics SDK integration.

## Wave plan (deferred to implementation sprint)

```
US-C4.1 (event vocabulary + JSON schema)    serial root
   ↓
US-C4.2 (collector + local JSONL buffer)
   ↓
US-C4.3 (opt-in prompt + persistence in config)
   ↓
US-C4.4 (PII redactor + adversarial tests)  ← MUST PASS before C4.5 lands
   ↓
US-C4.5 (daily batch uploader)
   ↓
US-C4.6 (CLI status + revoke verbs)
```

Strictly serial. US-C4.4 (the redactor) is the structural gate: it MUST
pass before C4.5 ships, because the uploader's safety contract is
"redactor said this is safe." Build the gate before the surface that
depends on it.

Estimate matches backlog: **~1800 LoC, 2 multi-session work units.**

## Stories

### Story US-C4.1 — Event vocabulary + JSON schema (locked)

**As** the telemetry subsystem,
**I want** a fixed, documented, versioned vocabulary of event types
            with explicit field lists,
**So that** any future PR that wants to add a NEW event type has to
            ALSO modify this schema doc — making the privacy
            surface-area review explicit, not implicit.

**Acceptance criteria:**

1. `include/human/telemetry/events.h` (NEW) defines exactly FOUR event
   types: `install_completed`, `onboarding_step_completed`,
   `daemon_crash`, `tick_fired`. No others.
2. `docs/specs/telemetry-schema-v1.md` (NEW) documents each event's
   field list with explicit allowed/disallowed:
   - `install_completed`: `version` (string), `os_release` (string,
     `uname -r`). NO: user-id, hostname, IP, install path.
   - `onboarding_step_completed`: `step_name` (kebab-case enum),
     `duration_seconds` (int). NO: answer values, paste contents.
   - `daemon_crash`: `signal` (int), `log_tail` (string, max 4096
     bytes, MUST have passed through `hu_pii_redact`). NO: stack
     symbols if they contain user file paths, never raw chat content.
   - `tick_fired`: `tick_name` (enum), `result` ("ok"|"failed"|"
     skipped"). NO: timestamps with sub-minute granularity (privacy
     side-channel), no error messages.
3. Each event carries `schema_version=1` and `event_ts` rounded to
   the nearest hour (further privacy hedge — exact timestamps are
   identifying).
4. Schema doc has a "If you want to add an event" section that says:
   "this is a privacy-sensitive surface — adding an event requires a
   `--aspects security` panel review and a PII-redaction test
   covering the new fields. Do not skip these steps."

**Files expected to change:**

- `include/human/telemetry/events.h` (NEW)
- `docs/specs/telemetry-schema-v1.md` (NEW)

---

### Story US-C4.2 — Collector + local JSONL buffer

**As** the telemetry subsystem,
**I want** events to land in `~/.human/telemetry.jsonl` (append-only,
            one event per line, no PII at this layer because §C4.1
            schema already excludes it),
**So that** the user can `cat ~/.human/telemetry.jsonl` and see
            EXACTLY what would be uploaded — before any network code
            runs.

**Acceptance criteria:**

1. `hu_telemetry_record(event_type, fields)` appends a single line of
   JSON to `~/.human/telemetry.jsonl` (creates dir if needed).
2. The write is best-effort — telemetry I/O failure NEVER propagates
   to the caller (e.g. a daemon tick that fails to record telemetry
   does NOT itself fail). Per
   `~/.claude/rules/silent-config-gated-subsystems.md`, this MUST
   emit a one-shot info log when the telemetry layer is opted-OUT,
   so an operator who expected events to land but sees none gets a
   visible signal.
3. File is mode 0600 (user-only read/write). Pinned by a test that
   asserts stat() on the created file.
4. Rotation: file grows to max 10 MB, then rotates to
   `telemetry.jsonl.1` (gz-compressed). At most 5 rotations kept.
5. Test: `tests/test_telemetry_collector.c` — record several events,
   parse the JSONL, assert each event has only the schema-allowed
   fields. Adversarial: try to record an event with a disallowed
   field, assert it's dropped (not just ignored — actively dropped
   with an error log).

**Files expected to change:**

- `src/telemetry/collector.c` (NEW, ~400 LoC)
- `include/human/telemetry/collector.h` (NEW)
- `tests/test_telemetry_collector.c` (NEW, ~350 LoC)

---

### Story US-C4.3 — Opt-in prompt + per-event audit log

**As** a user being asked to share telemetry,
**I want** the opt-in prompt to be unambiguous about WHAT is shared
            and WHAT is NEVER shared, with the four event types
            explicitly named,
**So that** my consent is informed — and the consent decision is
            audit-logged so I can prove later what I agreed to.

**Acceptance criteria:**

1. Opt-in prompt fires during Sprint 51 onboarding US-C2.2 (Welcome +
   privacy). Copy lives at `docs/copy/onboarding-telemetry-optin.md`.
2. Copy explicitly lists: WHAT is shared (4 events, named), WHAT is
   NEVER shared (message content, contacts, fact text), HOW to revoke
   (`human telemetry revoke`).
3. User answers `y` or `n` (default `n` — opt-out is the no-op path).
4. Decision recorded in `~/.human/config.json::telemetry.opt_in` AND
   `~/.human/telemetry-consent-audit.jsonl` (single-line append, ISO
   timestamp + decision + the version of the consent copy file
   they saw).
5. Test: opt-in then opt-out lifecycle; audit log captures both
   decisions; default `n` is exercised by an empty-input test (user
   pressed enter).

**Files expected to change:**

- `docs/copy/onboarding-telemetry-optin.md` (NEW)
- `src/telemetry/consent.c` (NEW, ~200 LoC)
- `include/human/telemetry/consent.h` (NEW)
- `tests/test_telemetry_consent.c` (NEW, ~250 LoC)

---

### Story US-C4.4 — PII redactor + adversarial tests (THE GATE)

**As** the telemetry subsystem's safety contract,
**I want** every string field destined for telemetry to pass through
            `hu_pii_redact` first, AND adversarial tests that feed
            known-PII strings through and assert nothing leaks,
**So that** the privacy thesis is mechanically enforced, not just
            documented.

**Acceptance criteria:**

1. `src/telemetry/redact.c` (NEW) — thin wrapper around the existing
   `hu_pii_redact` that adds telemetry-specific rules: file-path
   redaction (replace `/Users/<name>/` with `/Users/<redacted>/`),
   email redaction, phone-number redaction, URL-query-string
   redaction (replace `?foo=BAR` with `?foo=<redacted>`).
2. `tests/test_telemetry_no_pii.c` (NEW) — adversarial table-driven
   test feeds 50+ known-PII strings through the redactor and asserts
   the output contains NONE of: a known email pattern, a known phone
   number, a sequence of digits that could be an SSN, a file path
   containing a real user-name, a contact-handle string.
3. The test vectors include strings constructed to look like
   pseudo-PII (e.g. "user+test@example.com" — domain-shaped fake
   email). Redactor MUST be conservative: better to over-redact a
   non-PII pattern than to under-redact a real one.
4. `tests/test_telemetry_no_pii_integration.c` (NEW) — generates 1000
   synthetic events with PII embedded in every string field, runs
   them through the COMPLETE collector→redactor→buffer pipeline,
   asserts the output JSONL contains zero matches against the PII
   patterns.
5. **GATE**: this story's tests MUST pass before US-C4.5 (uploader)
   ships. Sprint-level DoD includes this as a hard rule.

**Files expected to change:**

- `src/telemetry/redact.c` (NEW, ~250 LoC)
- `include/human/telemetry/redact.h` (NEW)
- `tests/test_telemetry_no_pii.c` (NEW, ~400 LoC, table-driven)
- `tests/test_telemetry_no_pii_integration.c` (NEW, ~300 LoC)

---

### Story US-C4.5 — Daily batch uploader

**As** the telemetry collection backend,
**I want** the daemon to batch-upload the local JSONL buffer once per
            day to a static endpoint, with the user's opt-in gate
            checked before every transmission,
**So that** we get aggregate signal without continuous network
            chatter.

**Acceptance criteria:**

1. `src/telemetry/uploader.c` (NEW) — daemon tick that fires once
   per day at a configurable hour (default 03:30 local, similar to
   the lora-nightly pattern from N2/G5).
2. BEFORE every upload, re-checks `config.telemetry.opt_in`. If
   FALSE (user revoked since last upload), aborts without
   uploading, clears the local buffer.
3. Reads the local JSONL buffer, re-runs each event through the
   redactor (defense-in-depth — the collector already ran it, but
   the uploader doesn't trust prior pass), gzips, sends via HTTPS
   POST to a configurable endpoint.
4. Endpoint configuration: `config.telemetry.upload_url` — defaults
   to "" which means upload is disabled even when opted in.
   Operator must explicitly set the URL — no hidden default.
5. HTTP failure: keeps the local buffer for next day's retry; logs
   failure once. Does NOT retry within a tick.
6. After successful upload, truncates the local JSONL buffer.
7. Tests use a local fake HTTPS server (`tests/fixtures/fake_telemetry_endpoint/`
   — NEW). NO real network in test mode.
8. Tests cover: opted-in upload happy path, opted-out short-circuit,
   network failure → buffer retained, partial upload (no-op for v1
   — full upload or nothing).

**Files expected to change:**

- `src/telemetry/uploader.c` (NEW, ~400 LoC)
- `src/daemon.c` (~20 LoC — add the daily tick)
- `tests/test_telemetry_uploader.c` (NEW, ~450 LoC)
- `tests/fixtures/fake_telemetry_endpoint/` (NEW)

---

### Story US-C4.6 — `human telemetry status` + `revoke` CLI verbs

**As** a user who opted in three months ago and wants to check what
            we've collected (or revoke),
**I want** `human telemetry status` to show me the local buffer's
            contents, the last upload time, the opt-in state, and
            `human telemetry revoke` to permanently disable AND
            delete the local buffer,
**So that** revocation is an action I can take, not a docs link I
            have to find.

**Acceptance criteria:**

1. `human telemetry status` prints: opt-in state (in/out), last
   upload ISO timestamp (or "never"), buffer event count, buffer
   file size, last 5 events (one per line, full JSON visible to the
   user).
2. `human telemetry revoke` prompts for confirmation (`yes/no`),
   then:
   - Sets `config.telemetry.opt_in = false`
   - Truncates `~/.human/telemetry.jsonl`
   - Truncates rotated `.jsonl.1..jsonl.5` if present
   - Appends a final entry to `telemetry-consent-audit.jsonl`
     documenting the revocation
3. `human telemetry revoke --yes` skips the prompt (for scripts).
4. Both commands have `--json` output for scripting (mirrors Sprint
   50 §C3.7 pattern).
5. Tests: status output schema; revoke happy path (verify
   files truncated, opt_in flipped, audit log appended);
   revoke when already opted out (no-op, no error).

**Files expected to change:**

- `src/cli_commands.c` — telemetry sub-command dispatcher (~150 LoC)
- `src/telemetry/cli.c` (NEW, ~250 LoC)
- `tests/test_telemetry_cli.c` (NEW, ~350 LoC)

---

## Definition of Done (sprint-level)

Identical to Sprint 50 DoD. PLUS the C4-specific gates:

1. **PII redaction GATE.** US-C4.4 tests MUST pass before US-C4.5
   uploader code is merged. CI fail-closed: a PR that touches
   `src/telemetry/uploader.c` but doesn't show US-C4.4 tests passing
   in the same commit is auto-blocked by a new gate script.
2. **Aspect-panel security review.** Pre-merge `/aspect-panel` run
   with `--aspects security correctness regression` weighted higher
   than usual. Sprint cannot close on a SPLIT/FAIL from security.
3. **Adversarial review.** A second engineer (or critic agent) must
   independently try to construct a PII-leak by adding a field to
   one of the four event types. The review FAILS if they can
   construct one — that's the test the redactor was supposed to
   prevent.

## Risk register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| PII slips through redactor in a new event field | High | **CRITICAL — destroys product trust** | US-C4.4 table-driven tests + sprint DoD adversarial review + locked schema in US-C4.1 |
| User revokes but uploader already started a transmission | Medium | High | Revoke happens THEN clears buffer; in-flight transmission can be racing but content was already opt-in at the moment of upload start. Pin this by test. |
| Default upload URL accidentally points at a real endpoint in dev | High | Medium | Default = "" (empty); pre-commit script asserts `config.telemetry.upload_url == ""` in `config.json.example` |
| Daemon crash log_tail contains the in-flight message that crashed it | High | **CRITICAL** | `daemon_crash` event MUST run log_tail through redactor with the strictest profile. Adversarial test feeds known PII through a crash scenario and asserts redaction. |
| Telemetry I/O failure cascades into daemon failure | Medium | High | Per silent-config-gated-subsystems rule, telemetry layer always returns OK; logs at most once per day on persistent failure. |

## Anti-goals (re-stated from backlog §C4)

- **Never collect message content.** Tested adversarially.
- **Never collect contact handles.** Tested adversarially.
- **Never use telemetry for advertising or third-party analytics.**
  Hard rule. If a future PR adds a third-party SDK to this codebase,
  this sprint's DoD review process must catch it.
- **No real-time streaming.** Daily batch only.
- **No on-by-default behavior.** Opt-in is the only path.
