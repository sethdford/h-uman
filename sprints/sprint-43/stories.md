# Sprint 43 Backlog — Distribution MVP

## Goal
Move h-uman from "git clone + cmake" to a shippable artifact path toward M4 (100 DAU)
by landing a Homebrew tap, polishing `human onboard` for iMessage-first activation,
shipping iMessage courtesy-reply, `human doctor --install`, a chat.db locked diagnostic,
and a website/formula drift detector — with zero identifier-bearing telemetry.

## User Stories (in priority order)

---

### US-43.1 (P1): Homebrew Formula Auto-Update

**As a** new user on macOS,
**I want** to install h-uman with `brew install sethdford/human/human`,
**so that** I can get a verified binary without cloning the repo or running cmake.

**Acceptance criteria:**
- AC-43.1.1: GIVEN a new semver tag `vX.Y.Z` is pushed and `release.yml` produces all three binaries (macos-aarch64, linux-x86_64, linux-aarch64), WHEN the `update-tap` job runs, THEN `Formula/human.rb` is committed with correct `version`, `url`, and `sha256` fields for all three variants and zero placeholder hashes remain.
- AC-43.1.2: GIVEN `scripts/update-formula-hashes.sh` is run when one artifact is missing, WHEN executed, THEN the script exits non-zero and refuses to commit, leaving the formula unchanged (idempotent on partial input).
- AC-43.1.3: GIVEN two tags are pushed in quick succession and both `update-tap` jobs reach the commit step concurrently, WHEN both run, THEN the `concurrency: tap-update` guard serializes them and neither job corrupts the formula.
- AC-43.1.4: GIVEN `scripts/check-formula-install.sh` is run on a host where the installed binary version does not match the formula `version` field, WHEN executed, THEN it exits non-zero and prints both versions and the mismatch path.
- AC-43.1.5: GIVEN the formula is installed on a pinned `macos-15` runner via `brew install`, WHEN `brew test sethdford/human/human` runs, THEN `human --version` exits 0 and the output contains the semver string from the formula.

**Test seam:** `scripts/update-formula-hashes.sh` (new, idempotent), `scripts/check-formula-install.sh` (new); CI job `brew-install-smoke` pinned to `macos-15` runner in `release.yml`. Formula already exists at `Formula/human.rb` (stub with placeholder hashes confirmed).
**Estimate:** M
**Risk:** MEDIUM — tap automation touches an external GitHub repo; `bottle do` block deliberately omitted pending sprint-42 signing; add `# TODO(US-42.X): bottle do` comment in formula.
**Dependencies:** `release.yml` `build` job must upload all three named artifacts before `update-tap` fires.
**Out of scope:** `bottle do` block, Linux apt/snap/pip, Windows Scoop formula.

---

### US-43.2 (P1): Onboard Post-Success Next-Step Message

**As a** first-time user who just completed `human onboard`,
**I want** to see a specific, actionable next step tailored to what I configured,
**so that** I know exactly which command to run next without reading the docs.

**Acceptance criteria:**
- AC-43.2.1: GIVEN `hu_onboard_nextstep_format` is called with `(imessage_paired=true, persona_set=true, ollama_ok=true, brew_installed=true)`, WHEN it writes to the output buffer, THEN the output contains `"human chat"` and does NOT contain the word `"setup"` or `"configure"`.
- AC-43.2.2: GIVEN the function is called with all four bools false (bare fallback state), WHEN executed, THEN the output is a distinct fallback message that does NOT match any of the other four output variants by `strcmp`.
- AC-43.2.3: GIVEN the output buffer is exactly 1 byte too small for the formatted message, WHEN `hu_onboard_nextstep_format` writes to it, THEN the function returns `HU_ERR_BUFFER_TOO_SMALL` and logs a diagnostic (no silent truncation).
- AC-43.2.4: GIVEN any of the five distinct input combinations is passed, WHEN the output is compared to the old generic success message text (e.g. `"You're all set"`), THEN none of the five outputs match that string.
- AC-43.2.5: GIVEN both call sites in `src/onboard.c` (post-`human init` and post-`human onboard`) complete successfully, WHEN each invokes the formatter, THEN the produced string is one of the five distinct outputs confirmed by test round-trip.

**Test seam:** `tests/test_onboard.c` — new suite `onboard_nextstep`; pure formatter, no I/O, no `HU_IS_TEST` guard required.
**Estimate:** S
**Risk:** LOW — pure formatter, no network, no filesystem writes, no security surface.
**Dependencies:** none.
**Out of scope:** Localisation, rich-text/ANSI formatting, dynamic persona name substitution.

---

### US-43.3 (P0): iMessage Non-Allowlisted Courtesy Reply

**As a** h-uman user who receives an iMessage from someone not on my allowlist,
**I want** the assistant to send a single polite courtesy reply (at most once per 24 h per handle, 50/day aggregate cap),
**so that** unknown senders know I use an AI assistant without being flooded by repeated automated messages.

**Acceptance criteria:**
- AC-43.3.1: GIVEN `hu_imessage_should_courtesy_reply` is called with `(handle_in_allowlist=false, hours_since_last_reply=25, aggregate_today=0, dedup_io_ok=true)`, WHEN evaluated, THEN the predicate returns `true`.
- AC-43.3.2: GIVEN the same handle received a courtesy reply 12 hours ago, WHEN the predicate is called with `hours_since_last_reply=12`, THEN it returns `false` (per-handle 24 h dedup enforced).
- AC-43.3.3: GIVEN `aggregate_today=50`, WHEN the predicate is called regardless of per-handle state, THEN it returns `false` (50/day aggregate cap enforced).
- AC-43.3.4: GIVEN the dedup log at `~/.human/imessage_courtesy.log` cannot be opened for write, WHEN the predicate is called with `dedup_io_ok=false`, THEN it returns `false` (fail-CLOSED — no reply sent on I/O uncertainty).
- AC-43.3.5: GIVEN a handle of the form `+1 (415) 555-0100`, WHEN the courtesy reply text is generated, THEN the handle does NOT appear verbatim in the reply body (handle-shaped name stripping applied).

**Test seam:** `tests/test_imessage_courtesy.c` — new; `hu_imessage_should_courtesy_reply` is a pure predicate over 4 bool inputs; dedup log path injectable via `HU_IS_TEST` env override; `pending_courtesy` buffer drain tested via mock agent-loop fixture.
**Estimate:** L
**Risk:** HIGH — outbound message to an unknown sender; a logic bug sends unsolicited messages at scale. Fail-CLOSED on every ambiguous state. Requires second-engineer review before merge. Do NOT touch `src/security/*`.
**Dependencies:** US-43.5 (chat.db locked diagnostic must be stable so the iMessage send path is not extended while poll errors are misclassified).
**Out of scope:** Group-chat courtesy reply, MMS, email, allowlist management UI, per-handle reply template customisation.

---

### US-43.4 (P1): `human doctor --install` Health Check

**As a** user who just installed h-uman via Homebrew,
**I want** to run `human doctor --install` and get a clear pass/fail verdict,
**so that** I can resolve problems before my first chat session without reading the docs.

**Acceptance criteria:**
- AC-43.4.1: GIVEN a clean Homebrew install (binary resolvable on `$PATH`, `~/.human/` config dir present, at least 1 channel paired, persona file present and parses cleanly), WHEN `hu_doctor_check_install` runs, THEN it returns `HU_OK` and emits exactly 4 `HU_DIAG_OK` items — one per sub-check.
- AC-43.4.2: GIVEN the persona file exists but contains malformed JSON, WHEN `hu_doctor_check_install` runs the persona sub-check, THEN that item is `HU_DIAG_ERR` with a message containing both `"persona"` and `"parse"`, and the overall return is still `HU_OK` (diagnostic, not abort).
- AC-43.4.3: GIVEN `human doctor` is invoked with the `--install` flag, WHEN CLI dispatch runs, THEN it routes to `hu_doctor_check_install` ahead of `--privacy` (RESERVED sprint-42), `--fix`, and the default full-scan (precedence: subcommand `--install` > `--privacy` > `--fix` > default).
- AC-43.4.4: GIVEN no config dir and no paired channel, WHEN `human doctor --install` runs, THEN the output contains `"human onboard"` as the remediation hint for the config-dir failure and `"human channel pair imessage"` for the channel failure.
- AC-43.4.5: GIVEN `human doctor --install` runs under ASan, WHEN it completes, THEN no heap leaks, use-after-free, or buffer overflows are reported.

**Test seam:** `tests/test_doctor_install.c` — new suite; `hu_doctor_check_install` receives injected state structs, no real filesystem reads in unit tests. Extends the existing `doctor.h` / `doctor.c` check pattern confirmed in `src/doctor.c`.
**Estimate:** M
**Risk:** LOW — read-only diagnostic; no write path; no network calls.
**Dependencies:** none (extends existing `doctor.h` check pattern; `doctor.c` confirmed at 1022 lines with established `doctor_push_line` idiom).
**Out of scope:** `--privacy` flag (sprint-42 owns), `--fix` auto-remediation (existing `doctor_fix.h`), Windows path handling.

---

### US-43.5 (P1): chat.db Locked Diagnostic

**As a** user whose iMessage channel stops polling silently,
**I want** `human doctor` to surface an actionable message distinguishing Full Disk Access denied from Messages.app sync lock from a missing chat.db,
**so that** I fix the correct problem without filing a support ticket.

**Acceptance criteria:**
- AC-43.5.1: GIVEN `hu_imessage_diag_from_poll_status` is called with `HU_IMESSAGE_ERR_AUTH`, WHEN it formats the diagnostic, THEN the output contains `"Full Disk Access"` and does NOT contain `"Messages.app may be syncing"`.
- AC-43.5.2: GIVEN the same function is called with `HU_IMESSAGE_ERR_BUSY`, WHEN it formats the diagnostic, THEN the output contains `"Messages.app may be syncing"` and does NOT contain `"Full Disk Access"` (mandatory cross-contamination adversary check).
- AC-43.5.3: GIVEN `HU_IMESSAGE_ERR_CANTOPEN`, WHEN the function runs, THEN the output contains `"chat.db"` and a path hint for the expected file location.
- AC-43.5.4: GIVEN `HU_IMESSAGE_ERR_NONE`, WHEN the function runs, THEN it returns a null or empty diagnostic (no spurious output on a healthy poll).
- AC-43.5.5: GIVEN `HU_IMESSAGE_ERR_OTHER`, WHEN the function runs, THEN it emits a generic message using the raw class name via existing `hu_imessage_error_class_name`; the implementation MUST NOT define a second or parallel `hu_imessage_error_class_t` enum (reuse the one in `include/human/channels/imessage.h`).

**Test seam:** `tests/test_imessage_diag.c` — new; pure function over the existing `hu_imessage_error_class_t` enum (confirmed: AUTH, CANTOPEN, BUSY, NONE, OTHER in `include/human/channels/imessage.h`); no SQLite, no filesystem; all 5 variants covered, cross-contamination assertions for AUTH and BUSY are mandatory.
**Estimate:** S
**Risk:** MEDIUM — wrong diagnostic message causes the user to grant unnecessary macOS permissions (e.g. FDA when only a sync lock is needed). The cross-contamination ACs in AC-43.5.1 and AC-43.5.2 are non-negotiable.
**Dependencies:** none — `hu_imessage_error_class_t` already defined in header.
**Out of scope:** Automatic remediation (granting FDA), Linux or Windows iMessage proxies.

---

### US-43.6 (P2): Website Install Drift Detector

**As a** maintainer releasing a new version,
**I want** CI to fail if the install instructions on the website show a different version than `Formula/human.rb`,
**so that** users never copy a stale `brew install` command from the marketing site.

**Acceptance criteria:**
- AC-43.6.1: GIVEN `website/src/data/install.json` version matches `Formula/human.rb` version, WHEN `website/scripts/check-install-matches-formula.mjs` runs, THEN it exits 0.
- AC-43.6.2: GIVEN `install.json` version is `"0.5.0"` and the formula version is `"0.6.0"`, WHEN the script runs, THEN it exits non-zero and prints both version strings and the file paths of the diverging sources.
- AC-43.6.3: GIVEN every Astro component under `website/src/` is grepped for a bare semver literal matching `\d+\.\d+\.\d+`, WHEN the grep runs, THEN it returns zero matches (all version references read from `install.json`, none hardcoded in component source).
- AC-43.6.4: GIVEN the drift-detector script is wired into the `docs` CI gate, WHEN a PR updates the formula version without updating `install.json`, THEN CI fails on that PR before merge.
- AC-43.6.5: GIVEN `install.json` is malformed JSON, WHEN the script runs, THEN it exits non-zero with a clear parse-error message (not a silent pass).

**Test seam:** `website/scripts/check-install-matches-formula.mjs` (new, no runtime deps beyond Node.js stdlib); tested by fixture pairs in `website/scripts/fixtures/` (matching pair, mismatched pair, malformed JSON). Note: `website/src/data/` directory does not yet exist — script creates it as part of this story.
**Estimate:** XS
**Risk:** LOW — read-only CI gate; no production code path touched.
**Dependencies:** US-43.1 (formula `version` field must be stabilised before the gate is meaningful; can be authored in parallel, wired into CI after US-43.1 lands).
**Out of scope:** Linux package version checking, Docker image tag drift, auto-correcting drift.

---

## Non-goals
- We will NOT ship native iOS/macOS/Android apps this sprint (sprint-45 owns).
- We will NOT add identifier-bearing telemetry or any opt-out-required analytics.
- We will NOT touch `src/security/*` (sprint-42), `src/hula/*` (sprint-44), `src/ml/*`, or `src/persona/persona.c`.
- We will NOT add pip, apt, snap, or Scoop packaging.
- We will NOT implement multi-user accounts or allowlist management UI.

## Open questions for stakeholder
- US-43.3 (courtesy reply): what is the exact courtesy reply text? The story defers template content to the implementation author. If a brand-approved template exists, surface it before sprint-43 kickoff.
- US-43.1 (Homebrew tap): does the tap live in a separate `sethdford/homebrew-human` repo or is it served directly from this repo's `Formula/` directory? The formula currently points to GitHub releases — confirm the tap repo target before the `update-tap` job is wired.

RESULT_product-owner=READY
