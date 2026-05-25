---
title: "Sprint 50 — Sprint C / C3: `human doctor` v2"
created: 2026-05-24
sprint: 50
branch: sprint-50-doctor-v2
spec: docs/plans/2026-05-25-sprint-c-backlog.md (§C3, lines 157-198)
program: docs/plans/2026-05-25-sprint-c-backlog.md
status: planned (PO + design only — implementation deferred to a follow-up multi-session work unit)
---

# Sprint 50 — `human doctor` v2

## Goal

Turn `human doctor` from an engineer-facing diagnostic into the user-facing
"is this installation actually working?" check. When a user types `human
doctor` and something's wrong, they get a clear, actionable, human-readable
summary — and an opt-in `--fix` mode that performs the obvious safe
remediations. JSON output makes the same check scriptable for the
dashboard.

This is the user-visible quality gate that pairs with C2 (onboarding):
onboarding sets you up, doctor proves you're set up correctly, and from
that point on doctor is what you re-run whenever something feels off.

## Scope discipline

- **In scope:** registry refactor of `src/doctor.c`, new pipeline checks
  (provider smoke, chat.db FDA, MLX/adapter, tick-freshness, persona
  regression), `--json` flag, `--fix` flag with confirmation, exit-code
  contract.
- **Out of scope:** new remote diagnostics, telemetry (C4 territory),
  GUI wrapper, doctor running as a daemon tick. The existing 8 checks
  (`hu_doctor_check_config_semantics`, `..._security`,
  `..._memory_health`, `..._skills`, `..._imessage`, `..._verifier`,
  `..._scheduler`, `..._response_pipeline`, `..._install`) STAY — the
  registry refactor preserves them as registry entries, doesn't delete.

## Wave plan (deferred to implementation sprint)

Stories sequence:

```
US-C3.1 (registry refactor) ──┬── US-C3.2 (chat.db FDA)
                              ├── US-C3.3 (provider smoke)
                              ├── US-C3.4 (MLX/adapter)
                              ├── US-C3.5 (tick freshness)
                              └── US-C3.6 (persona regression)
                                       │
                                       ↓
                              US-C3.7 (--json output)
                                       │
                                       ↓
                              US-C3.8 (--fix mode)
                                       │
                                       ↓
                              US-C3.9 (exit-code contract)
```

US-C3.1 is the foundation (every other story plugs into the registry).
Once it lands, US-C3.2–C3.6 are parallelizable across implementers
(each check is independent). US-C3.7–C3.9 are serial because each builds
on the union of all checks landing.

Estimate matches backlog: **~1500 LoC total, 1-2 multi-session work
units.** Per backlog §C3 dependencies: "None hard; can ship before or
after C2."

## Stories

### Story US-C3.1 — Check-registry vtable + migrate existing 8 checks

**As** the doctor v2 architecture,
**I want** every check to be a `hu_doctor_check_t` vtable entry
            (`name`, `description`, `run`, `fix_opt`) registered through
            a single registry,
**So that** new checks can land without touching `main()`-style dispatch
            and `--json` / `--fix` get uniform handling for free.

**Acceptance criteria:**

1. New header `include/human/doctor/check.h` defines
   `hu_doctor_check_t` (vtable) and `hu_doctor_registry_t`.
2. New `src/doctor/registry.c` implements `hu_doctor_registry_init`,
   `_register`, `_iter`, `_run_all`, `_free`. Allocator-owned per
   project memory conventions.
3. Every existing check fn in `include/human/doctor.h` is wrapped as a
   vtable entry registered at registry init (no behavior change). Old
   call sites in `src/doctor.c::main` are deleted in favor of
   `hu_doctor_registry_run_all`.
4. `tests/test_doctor_registry.c` (new) — registry init returns empty;
   after registering 3 fake checks, iter yields them in registration
   order; run_all calls each check's `run` once.
5. Full suite passes (≥11803 + 4 new tests).
6. Verifier evidence: `./build/human doctor` produces identical output
   (modulo ordering) before vs after the refactor on a clean
   `~/.human/` fixture.

**Files expected to change:**

- `include/human/doctor/check.h` (NEW)
- `include/human/doctor.h` (no change to public ABI; the existing 8
  fns stay)
- `src/doctor/registry.c` (NEW, ~200 LoC)
- `src/doctor.c` (~50 LoC delta — strip dispatch loop, init registry)
- `tests/test_doctor_registry.c` (NEW, ~150 LoC)
- `tests/test_main.c` (declare + call `run_doctor_registry_tests`)
- `CMakeLists.txt` (add the new source + test files)

---

### Story US-C3.2 — chat.db FDA permission check with System Settings link

**As** a user troubleshooting why iMessage ingest isn't working,
**I want** `human doctor` to specifically tell me whether
            `~/Library/Messages/chat.db` is readable, and if not, give
            me a one-paragraph plain-English explanation + a deep link
            to System Settings → Privacy & Security → Full Disk Access,
**So that** I can fix the most common first-run failure mode (FDA
            denied) without needing to know that "chat.db" is even a
            concept.

**Acceptance criteria:**

1. New check `hu_doctor_check_chatdb_readable` registered through
   US-C3.1 registry.
2. PASS condition: `~/Library/Messages/chat.db` exists AND a
   read-only `fopen` succeeds.
3. FAIL output names the SPECIFIC failure mode: "missing" (file
   doesn't exist — user is on a different OS or chat.db has never
   been initialized) vs "permission denied" (file exists, read fails
   with EACCES — FDA not granted).
4. The "permission denied" message includes the literal text:
   `Open: System Settings → Privacy & Security → Full Disk Access,
   then enable "human"` and a `x-apple.systempreferences:` URL the
   user can click in a terminal that supports OSC-8 hyperlinks.
5. `--fix` mode for THIS check is a no-op (it opens the System
   Settings URL via `open(1)` and exits — no autofix possible for
   permission grants).
6. Test: `tests/test_doctor_chatdb.c::test_chatdb_missing_returns_fail_with_missing_reason`
   uses a temp `HOME` that has no `Library/Messages/chat.db` and
   asserts the check's verdict + reason string.
7. Test: `tests/test_doctor_chatdb.c::test_chatdb_permission_denied_returns_fail_with_fda_link`
   uses a temp file with mode 000 and asserts the FAIL output
   contains the literal System Settings phrase.
8. NO `// allow-silent-pass` opt-outs — both negative tests must
   hard-fail when the check is missing (see
   `.claude/rules/tests-that-pin-bugs.md`).

**Files expected to change:**

- `src/doctor/check_chatdb.c` (NEW, ~120 LoC)
- `include/human/doctor.h` (declare `hu_doctor_check_chatdb_readable`)
- `tests/test_doctor_chatdb.c` (NEW, ~180 LoC)
- `tests/test_main.c` (add `run_doctor_chatdb_tests`)
- `CMakeLists.txt`

---

### Story US-C3.3 — Provider smoke-prompt check

**As** a user whose autoresponder went silent for a week,
**I want** `human doctor` to actually send a 1-token prompt to my
            configured provider and verify the round-trip succeeds,
**So that** a stale API key, a revoked OAuth token, or an unreachable
            provider surfaces BEFORE I send the next 50 voice
            messages it would have silently dropped.

**Acceptance criteria:**

1. New check `hu_doctor_check_provider_smoke` registered through the
   registry.
2. PASS condition: configured provider's `hu_provider_create` returns
   non-NULL AND a `complete()` call with prompt="ok" returns a
   non-empty response within 10s.
3. FAIL surfaces the exact failure mode:
   - "provider not configured" (config has no `provider` block)
   - "credentials missing" (`hu_provider_create` returned NOT_CONFIGURED)
   - "credentials invalid" (provider returned 401/403)
   - "rate limited" (provider returned 429)
   - "unreachable" (network failure)
4. Behind `HU_IS_TEST` the smoke check uses the mock provider — NO
   real network in test mode (project rule).
5. Provider-error tests use the mock provider's failure-injection API
   to exercise each FAIL branch.
6. Test (new): `tests/test_doctor_provider.c` — at least one test per
   FAIL branch + the PASS path.

**Files expected to change:**

- `src/doctor/check_provider.c` (NEW, ~150 LoC)
- `include/human/doctor.h` (declare entry)
- `tests/test_doctor_provider.c` (NEW, ~250 LoC)
- `tests/test_main.c`
- `CMakeLists.txt`

---

### Story US-C3.4 — MLX server + adapter file check

**As** a user running on Apple Silicon with the M3 path enabled,
**I want** `human doctor` to verify the MLX server is up, the active
            adapter file exists on disk, the adapter version matches
            what the daemon expects, and the daemon can swap it,
**So that** a stale adapter from a prior training run doesn't silently
            give me yesterday's persona for weeks.

**Acceptance criteria:**

1. New check `hu_doctor_check_mlx_adapter` registered.
2. PASS conditions ALL hold:
   - MLX HTTP endpoint at the configured `mlx.server_url` returns
     200 on `/health` within 5s
   - Active adapter path (per `~/.human/adapters/active.json`) exists
     on disk
   - Adapter version recorded in `active.json` matches the file's
     SHA-256 (recorded at training time)
3. FAIL surfaces the specific failure mode (mlx down vs adapter
   missing vs version mismatch) so the user knows which subsystem to
   debug.
4. `--fix` mode for MLX-down attempts `launchctl kickstart` for the
   MLX agent (after explicit confirmation). For adapter-missing it's
   a no-op (re-training is non-trivial).
5. Tests use a local fake-MLX HTTP server (already exists in
   `tests/fixtures/mlx_fake/`) — no real MLX process.
6. Test: `tests/test_doctor_mlx.c` covers PASS + 3 FAIL branches +
   the fix path's "user said no" branch.

**Files expected to change:**

- `src/doctor/check_mlx.c` (NEW, ~180 LoC)
- `include/human/doctor.h`
- `tests/test_doctor_mlx.c` (NEW, ~280 LoC)
- `tests/test_main.c`
- `CMakeLists.txt`

---

### Story US-C3.5 — Tick-freshness check (social_tick, autodream, lora_nightly)

**As** a user whose daemon has been up for days but isn't doing the
            background work it should,
**I want** `human doctor` to report when each scheduled tick
            (`social_tick`, `autodream`, `lora_nightly`, `feed_poll`)
            last ran successfully, and flag any tick that hasn't run
            within its expected interval,
**So that** a silently-disabled subsystem (the `silent-config-gated-
            subsystems` failure mode from `~/.claude/rules/`) surfaces
            as a doctor FAIL instead of as months of missing dpo_pairs.

**Acceptance criteria:**

1. New check `hu_doctor_check_tick_freshness` registered.
2. The daemon writes a heartbeat per tick type to
   `~/.human/tick-heartbeat.jsonl` (NEW — append-only, one line per
   tick fire, structured `{ts, tick_name, result}`).
3. PASS condition: every known tick has at least one heartbeat
   within its expected interval × 2 (e.g., a tick that runs hourly
   must have fired in the last 2h).
4. FAIL surfaces the specific stale ticks AND links the user to the
   relevant config block ("`lora_nightly` hasn't fired in 7 days —
   set `lora_nightly.enabled=true` in `~/.human/config.json` or set
   `HU_NIGHTLY_LORA_ENABLED=1` before launching the daemon").
5. Heartbeat write itself is best-effort — daemon never fails a tick
   because heartbeat I/O failed.
6. Test: `tests/test_doctor_tick_freshness.c` writes a temp
   heartbeat file with known timestamps and asserts the per-tick
   PASS/FAIL verdicts.
7. Test: `tests/test_daemon_tick_heartbeat.c` (new) — simulate a
   tick fire, assert exactly one line is appended.

**Files expected to change:**

- `src/doctor/check_tick.c` (NEW, ~200 LoC)
- `src/daemon.c` — add heartbeat write to each tick (~50 LoC, 6
  insertion sites)
- `include/human/doctor.h`
- `tests/test_doctor_tick_freshness.c` (NEW, ~250 LoC)
- `tests/test_daemon_tick_heartbeat.c` (NEW, ~150 LoC)
- `CMakeLists.txt`

---

### Story US-C3.6 — Persona-block regression check

**As** an engineer who just landed a persona prompt-block change,
**I want** `human doctor` to verify every registered prompt block
            (B6 causal attribution, B7 anticipatory, B8 identity, the
            new B5 audio tone, etc.) emits non-empty output for at
            least one known contact,
**So that** a refactor that silently null-routes a prompt block
            surfaces as a doctor FAIL — not as a quietly-degraded
            persona for weeks until someone re-reads the prompt and
            notices.

**Acceptance criteria:**

1. New check `hu_doctor_check_persona_blocks` registered.
2. The check uses a deterministic test contact (loaded from
   `tests/fixtures/doctor_persona_contact.json` — a fully-populated
   personal-model fixture for "alice").
3. For EACH registered prompt block (queried via the existing prompt-
   block registry), the check invokes the block with the test
   contact and asserts non-empty output.
4. FAIL output names the SPECIFIC empty-emitting block AND the file
   that block lives in ("`B5 audio tone` block at
   `src/persona/render_audio_tone.c` returned empty for the test
   contact — wire likely broken").
5. The fixture contact has enough data that every currently-shipped
   block SHOULD emit output. When a new block is added, the fixture
   gets updated as part of THAT story (forcing future block authors
   to think about the regression signal).
6. Test: `tests/test_doctor_persona_regression.c` — runs the check
   against the fixture, asserts PASS in the normal case; with a
   monkey-patched no-op block, asserts FAIL with the block name in
   the output.

**Files expected to change:**

- `src/doctor/check_persona.c` (NEW, ~150 LoC)
- `tests/fixtures/doctor_persona_contact.json` (NEW)
- `include/human/doctor.h`
- `tests/test_doctor_persona_regression.c` (NEW, ~200 LoC)
- `tests/test_main.c`
- `CMakeLists.txt`

---

### Story US-C3.7 — `--json` output mode

**As** the dashboard ingest job (or any scripted health check),
**I want** `human doctor --json` to emit one JSON document per run
            with the per-check verdict, reason, and timestamp,
**So that** I can render a status grid without parsing human-readable
            output and without grepping log files.

**Acceptance criteria:**

1. New CLI flag `--json` on `human doctor`. Output goes to stdout
   only; stderr stays empty on PASS. (JSON output to stderr is a
   common pitfall — pin it with a test that asserts stderr is
   empty.)
2. Schema (informally documented in `docs/guides/doctor-json.md`):
   ```json
   {
     "version": 1,
     "ts": "2026-05-25T03:55:00Z",
     "checks": [
       { "name": "chatdb_readable", "verdict": "pass", "reason": "" },
       { "name": "provider_smoke",  "verdict": "fail", "reason": "credentials invalid (401)" }
     ],
     "aggregate": "fail"
   }
   ```
3. Schema version pinned at 1; future additions go in a `v2` field
   so consumers can stay on v1 without breaking.
4. Aggregate is `"pass"` iff every check is `"pass"`; otherwise
   `"fail"`. NO middle states ("warn", "info") — keep it binary so
   the exit-code contract (US-C3.9) is unambiguous.
5. Test: `tests/test_doctor_json.c` — runs doctor against a
   PASS-everything fixture and a FAIL-one-check fixture; parses the
   resulting JSON with the existing `hu_json_parse` and asserts the
   `aggregate` and per-check verdicts. Also asserts stderr is empty
   on the PASS run.

**Files expected to change:**

- `src/doctor.c` — add `--json` flag and JSON emitter (~100 LoC)
- `docs/guides/doctor-json.md` (NEW, ~50 lines)
- `tests/test_doctor_json.c` (NEW, ~250 LoC)
- `tests/fixtures/doctor_pass_all/` (NEW, fixture HOME tree)
- `tests/fixtures/doctor_fail_provider/` (NEW)
- `tests/test_main.c`
- `CMakeLists.txt`

---

### Story US-C3.8 — `--fix` mode with explicit confirmation

**As** a user whose doctor just reported a FAIL,
**I want** `human doctor --fix` to attempt the obvious safe
            remediations (restart MLX, re-init social_state.json,
            etc.) but ONLY after asking me to confirm each fix
            individually,
**So that** I can fix the easy stuff without `man`-ing every check
            output AND without ever waking up to "doctor --fix
            decided to delete my personal-model database overnight."

**Acceptance criteria:**

1. New CLI flag `--fix`. Default = interactive (TTY prompt per fix);
   `--fix --yes` skips the prompt; `--fix --dry-run` prints what it
   would do without doing it.
2. Each check's vtable gains an OPTIONAL `fix` member (function
   pointer). When NULL, doctor reports "no autofix available — see
   <doc link>".
3. Fixes that touch USER DATA (delete files in `~/.human/`, reset
   the personal model, etc.) are EXPLICITLY OFF-LIMITS — they are
   never registered as fixes. Per backlog anti-goals: "Don't autofix
   things that touch user data without explicit confirmation."
4. Fixes the registry CAN ship:
   - Restart MLX agent via `launchctl kickstart` (with confirmation)
   - Open System Settings → FDA (no confirmation needed; just
     `open(1)` the URL)
   - Recreate `social_state.json` from defaults (with confirmation)
   - Enable a missing config gate (e.g., set
     `lora_nightly.enabled=true` in config.json — with confirmation)
5. Every `--fix` action emits a one-line audit-log entry to
   `~/.human/doctor-fix.log` (append-only, timestamped, includes the
   user's yes/no choice). Pinned by a test.
6. Test: `tests/test_doctor_fix_mode.c` covers `--dry-run` (no side
   effects), `--fix --yes` happy path, `--fix` interactive with
   "no" response (no-op), and confirms the audit-log entry shape.

**Files expected to change:**

- `src/doctor.c` — `--fix` flag parsing + per-check dispatch (~150
  LoC)
- `src/doctor/check_*.c` — one OPTIONAL `fix` fn per check (~30
  LoC × 5 checks)
- `tests/test_doctor_fix_mode.c` (NEW, ~300 LoC)
- `tests/test_main.c`
- `CMakeLists.txt`

---

### Story US-C3.9 — Exit-code contract: 0 = everything ok, non-zero = attention needed

**As** a CI job, dashboard scraper, or alerting integration,
**I want** `human doctor` to return exit code 0 iff every check
            PASSes, and a stable non-zero exit code per failure
            category,
**So that** I can `if ! human doctor --json; then page; fi` without
            parsing JSON in the alert path.

**Acceptance criteria:**

1. Exit-code contract:
   - `0` — every check PASS
   - `1` — one or more user-action-required FAILs (FDA denied,
     credentials missing, MLX down, etc.)
   - `2` — one or more bug-grade FAILs (binary missing, config
     unparseable — things that shouldn't happen on a working install)
   - `64` — doctor itself crashed (uncaught error in the check
     dispatcher — surfaced via the existing crash-handler infra)
2. Exit-code contract documented in `docs/guides/doctor.md` (NEW —
   the canonical user-facing doc).
3. Test: `tests/test_doctor_exit_codes.c` — at least one test per
   exit code, asserts the exit code AND that the JSON aggregate
   matches when `--json` is on.
4. Pre-commit hook check: a small awk script enforces that the
   exit-code table in `docs/guides/doctor.md` and the constants in
   `src/doctor.c` stay in sync (mirrors the `cmake-build-stale-
   binary` style of structural enforcement).

**Files expected to change:**

- `src/doctor.c` — exit-code emission (~30 LoC)
- `docs/guides/doctor.md` (NEW, ~80 lines)
- `tests/test_doctor_exit_codes.c` (NEW, ~200 LoC)
- `scripts/check-doctor-exit-codes-in-sync.sh` (NEW, ~40 lines)
- `.githooks/pre-commit` (add the check, conditional on
  `src/doctor.c OR docs/guides/doctor.md` being staged)
- `tests/test_main.c`
- `CMakeLists.txt`

---

## Definition of Done (sprint-level)

Every story must satisfy ALL of:

1. `RESULT_verifier=PASS` from `/verify` on a deterministic
   reproduction of the acceptance criteria.
2. Critic review (or aspect-panel for security-touching stories —
   US-C3.2 has FDA messaging, US-C3.3 has credential paths, US-C3.4
   has subprocess invocation) reports no CRITICAL findings open.
3. Full test suite passes (≥ pre-sprint baseline + the story's new
   tests). NO `// allow-silent-pass` opt-outs (see
   `.claude/rules/tests-that-pin-bugs.md` — the recurring pattern is
   the reason this whole rule exists).
4. Sprint-auditor produces `RESULT_sprint-auditor=PASS` against the
   sprint goal.
5. Production binary signed (`Signing human binary with Human Local
   Dev certificate` line visible in the build output — per
   `~/.claude/rules/cmake-build-stale-binary.md`).
6. Tag `v0.7.0-doctor-v2` cut and pushed iff sprint-auditor PASS.

## Risk register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| FDA permission denial UX is platform-specific (macOS only) | High | Medium | US-C3.2 returns a NEUTRAL "platform check not applicable" verdict on non-macOS hosts, not FAIL — so the CI matrix on Linux stays green |
| Provider smoke uses real network if `HU_IS_TEST` guard is forgotten | High | High (test flakiness, billing surprise) | Add a pre-commit script that greps `src/doctor/check_provider.c` for `HU_IS_TEST` and refuses if absent. Same pattern as the existing test-reference check. |
| `--fix` mode races with a live daemon and breaks state | Medium | High | `--fix` checks for a running daemon (pidfile in `~/.human/`) and refuses with an actionable message: "stop the daemon first: `human daemon stop`". Tested. |
| Tick-heartbeat I/O degrades daemon throughput | Low | Medium | Heartbeat is best-effort (failure does NOT propagate to tick). Append-only `O_APPEND` write is single-syscall, sub-millisecond on local SSD. Benchmark in US-C3.5 verifier evidence. |

## Anti-goals (re-stated from backlog §C3)

- **No remote diagnostics.** Doctor reads only local state.
- **No phone-home mode.** Telemetry is C4 territory and OPT-IN ONLY.
- **No autofix of user data without explicit confirmation.** US-C3.8
  bakes this in structurally — fix-fns that touch user data simply
  don't get registered.

## Dependencies on other sprints

- None HARD. C3 can ship before or after C2.
- C4 (telemetry) consumes doctor output if it ships after; doctor
  emits no telemetry on its own.
- Sprint 49's app-bundle work (C1.1–C1.6) is the deployment vehicle:
  `human doctor` ships INSIDE the bundle, so US-C3.9's exit-code
  contract is what `verify-bundle.sh` (Sprint 49 deliverable) calls
  to confirm the bundle is functional.
