# Sprint 9 Backlog — Distribution MVP

## Header

| Field | Value |
|---|---|
| Sprint | 9 |
| Goal | Land a one-command install path (Homebrew) and a polished first-run experience so that a macOS user can go from zero to chatting via iMessage in under 10 minutes |
| Dates | 2026-05-17 — 2026-05-30 |
| Scrum-master | TBD |
| Branch | `sprint-9-distribution-mvp` |
| Working directory | `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-9-distribution` |
| Base SHA | ea02b08e |
| Related sprints | Sprint 7 (learning loop — file-domain-disjoint, no conflict); Sprint 8 (privacy — do NOT touch persona encryption or signing) |

---

## User Stories (in priority order)

---

### US-9.1 (P0): As a macOS user, I want `brew install humanlabs/human/human` to give me a working binary, so that I can start using human without reading a build guide

**Context from codebase:**
`Formula/human.rb` exists but has placeholder SHA256s (`0000000000000000000000000000000000000000000000000000000000000000`). The formula is structurally complete (pre-built binary path + head build path, arm64 darwin + linux x86_64, bash/zsh/fish completions, man pages, caveats). The gap is: no real release artifacts exist at the referenced GitHub release URL, so both the binary install path and `brew install --build-from-source` are untested end-to-end.

**Acceptance criteria:**

- AC-9.1.1: GIVEN the `humanlabs/homebrew-human` tap exists on GitHub, WHEN a user runs `brew tap humanlabs/human && brew install humanlabs/human/human`, THEN the binary is installed to `$(brew --prefix)/bin/human` and `human --version` exits 0 with a semver string matching the release tag (e.g. `0.5.0`).
- AC-9.1.2: GIVEN a fresh macOS arm64 machine with Homebrew but no Xcode CLT, WHEN the pre-built bottle is available for the release tag, THEN `brew install` completes without invoking cmake or a compiler.
- AC-9.1.3: GIVEN `brew install --build-from-source humanlabs/human/human`, WHEN run on arm64 macOS with cmake and Xcode CLT present, THEN the build succeeds, tests pass (`./build/human_tests` exits 0), and the installed binary passes `brew test humanlabs/human/human`.
- AC-9.1.4: GIVEN the formula's `test do` block, WHEN `brew test` runs, THEN `human --version` exits 0 AND the output contains the string `"human"`.
- AC-9.1.5: GIVEN a GitHub Actions release workflow (`release.yml`) produces artifacts for `human-macos-aarch64.bin`, WHEN the formula's SHA256 fields are updated to match those artifacts, THEN `brew fetch humanlabs/human/human` downloads the artifact and verifies the hash without error.

**Estimate:** M
**Dependencies:** none
**Risk tier:** Low (formula + CI plumbing; no security-sensitive code paths)
**Test seam:** `brew test` is the test seam; formula unit test is `assert_match "human", shell_output("#{bin}/human --version")`. For CI: add a `brew-install-smoke` job to `release.yml` that taps and installs from the just-built artifact.
**Out of scope:** Windows installer, pip/apt/snap, notarization (sprint-8 owns code signing).
**DoD:** `brew install` smoke passes on a clean arm64 macOS runner in CI; `human --version` exits 0; formula SHA256s are real values (not zeros); tap README documents one-line install command.

---

### US-9.2 (P0): As a first-time installer, I want `human onboard` to end with a concrete next step pointing me at iMessage pairing, so that I am not left at a prompt wondering what to do

**Context from codebase:**
`src/onboard.c` line 493-496: the wizard ends with either `"Run 'human onboard --apple' to start chatting with Apple Intelligence.\n"` (Apple path) or `"Run 'human agent' to start chatting.\n"` (all other paths). This is the dead end. The user has no config, no channel paired, and no idea what `human agent` does on its own. The wizard also has no post-write validation — it does not confirm the config parses cleanly before exiting.

**Acceptance criteria:**

- AC-9.2.1: GIVEN `human onboard` completes successfully on macOS, WHEN the wizard exits, THEN it prints a "What's next" block containing ALL of: (a) the config file path written, (b) the exact command to run to pair iMessage (`human doctor imessage` or `/imessage:access` as appropriate), and (c) the command to start the agent (`human agent`), in that order.
- AC-9.2.2: GIVEN `human onboard` completes successfully on a non-macOS platform, WHEN the wizard exits, THEN the "What's next" block omits the iMessage pairing step and instead shows the next concrete channel action for that platform.
- AC-9.2.3: GIVEN `human onboard` writes `config.json`, WHEN the wizard is about to exit, THEN it calls `hu_config_load` on the written file and prints `"Config verified OK"` if it parses, or prints `"Warning: config written but failed to parse — run 'human doctor --fix' to repair"` and returns `HU_ERR_IO` if it does not.
- AC-9.2.4: GIVEN `human onboard` is run when a config already exists, WHEN the wizard exits early with `"Config already exists..."`, THEN the message also prints the config path and says `"Run 'human doctor' to check status, or 'human doctor imessage' to pair iMessage."` (currently neither path nor pairing hint is shown).
- AC-9.2.5: GIVEN `tests/test_onboard.c` (or equivalent), WHEN the test invokes `hu_onboard_run_with_args` under `HU_IS_TEST`, THEN it asserts the function returns `HU_OK` and does not assert the printed output (print is side-effect-only in test mode per existing guard at line 182). The production output change is verified by manual smoke-test documented in the PR description.

**Estimate:** S
**Dependencies:** none
**Risk tier:** Low
**Test seam:** `hu_onboard_run_with_args` is already callable under `HU_IS_TEST`. The "What's next" printing is inside the `#ifndef HU_IS_TEST` block; verify production output via smoke-test or a `HU_IS_TEST`-gated string capture parameter added to the function signature (implementer's choice, but the latter is preferred).
**Out of scope:** Multi-channel selection in the wizard (pick one channel post-onboard, not all); wizard GUI.
**DoD:** Full suite green (10,000+ tests, 0 ASan); `/verify` PASS; PR description includes a terminal recording or transcript showing the new "What's next" block on macOS and on a non-macOS path.

---

### US-9.3 (P1): As an iMessage user who DMed the assistant from a non-allowlisted handle, I want to receive a message explaining I need to be added to the allowlist, so that I am not left wondering why the assistant went silent

**Context from codebase:**
`src/channels/imessage.c` lines 3889-3908: the allowlist check silently `continue`s past non-allowlisted senders — `c->last_rowid = rowid; continue;`. There is no log, no reply, no user-facing feedback. The only trace is the rowid advance (message is consumed and forgotten). A new user who self-DMs from a second Apple ID, or a contact the user forgot to allowlist, gets silence.

Additionally, the circuit-breaker logs use `hu_log_error`/`hu_log_warn` (daemon log), not any user-facing output. When `chat.db` is `SQLITE_BUSY` (locked by Messages.app during a large sync), the retry loop in `imessage.c` lines 553-569 does 3 retries with no user-visible feedback; after exhaustion the circuit-breaker accumulates failures silently.

**Acceptance criteria:**

- AC-9.3.1: GIVEN the iMessage channel is running with a non-empty allowlist, WHEN a DM arrives from a handle not in the allowlist, THEN the assistant sends ONE reply to that handle saying (paraphrase acceptable): `"Hi — I'm [persona name]. To chat with me, ask [owner handle] to add you. (human assistant — running locally on their Mac)"`. The reply is sent at most once per handle per 24-hour window (dedup key: `handle + floor(epoch / 86400)`).
- AC-9.3.2: GIVEN a non-allowlisted sender triggers the once-per-day reply cap, WHEN the same handle sends a second message within 24 hours, THEN no second reply is sent and the rowid is advanced (message consumed silently, as today).
- AC-9.3.3: GIVEN `chat.db` returns `SQLITE_BUSY` or `SQLITE_LOCKED` on all 3 retry attempts, WHEN the circuit-breaker records the failure, THEN `hu_imessage_save_poll_status` is called immediately AND a `hu_log_warn` line is emitted with the text `"chat.db busy after 3 retries — Messages.app may be syncing"` (exact wording subject to implementer; "busy" and "3 retries" must appear).
- AC-9.3.4: GIVEN `tests/test_imessage_allowlist.c` (or the existing iMessage test file), WHEN a mock message from a non-allowlisted sender is injected, THEN `hu_imessage_ctx_t.last_message` contains the one-time reply text AND a second injection within the same 24-hour bucket does NOT produce a second `last_message` write (use mock epoch injection via `HU_IS_TEST` seam).
- AC-9.3.5: GIVEN `tests/test_imessage_allowlist.c`, WHEN the test asserts AC-9.3.4, THEN it references `hu_imessage_*` production symbols (not a local reimplementation), per `.claude/rules/test-references-production-symbol.md`.

**Estimate:** M
**Dependencies:** none
**Risk tier:** Medium (touches channel message dispatch; allowlist is security-adjacent but not the security predicate itself)
**Test seam:** Existing `HU_IS_TEST` mock message injection in `hu_imessage_ctx_t.mock_msgs` (lines 222-238). The dedup key requires a mockable "current epoch" parameter or a `HU_IS_TEST` hook — implementer must expose this as a pure predicate per `.claude/rules/security-predicate-extraction.md`.
**Out of scope:** Allowlist self-service via DM ("add me" auto-approve) — that is the exact attack vector warned against in the iMessage MCP instructions. Any DM-driven allowlist mutation is NOT part of this story.
**DoD:** Full suite green; `/verify` PASS; new tests reference production symbols; no allowlist mutation from inbound messages.

---

### US-9.4 (P1): As a new user who ran `brew install` and `human onboard`, I want `human doctor` to confirm in green/red whether my install is ready to use, so that I know whether I can start chatting or need to fix something first

**Context from codebase:**
`src/doctor.c` has `hu_doctor_check_imessage` (line 450) and fix routines in `src/doctor_fix.c`. The existing `human doctor` subcommand (wired in `src/main.c` lines 564-684) already has imessage-specific and verifier-specific sub-diagnostics. What is missing: a top-level "is this install ready to use?" summary check that covers: (a) config parses, (b) at least one channel is paired/active, (c) persona file exists and parses, (d) default provider is configured (API key present or no-key provider like mlx_local or apple). The existing doctor does not emit a nonzero exit code when checks fail.

**Acceptance criteria:**

- AC-9.4.1: GIVEN `human doctor` is run after a successful `human onboard`, WHEN all four conditions are met (config parses, at least one channel configured, persona parses, provider configured), THEN the output contains `"install: READY"` and the process exits 0.
- AC-9.4.2: GIVEN `human doctor` is run when no channel is configured (e.g., fresh onboard with no `/imessage:access` run yet), WHEN it prints results, THEN the channel check line reads `"channel: NONE — run 'human doctor imessage' to pair iMessage"` and the process exits 1.
- AC-9.4.3: GIVEN `human doctor` is run when the persona file is missing or fails JSON parse, WHEN it prints results, THEN the persona check line reads `"persona: MISSING — run 'human doctor --fix' to restore defaults"` and the process exits 1.
- AC-9.4.4: GIVEN `human doctor --json` is run, WHEN any check is red, THEN the JSON output contains `{"status": "NOT_READY", "checks": [...]}` where each check has `"name"`, `"ok"` (bool), and `"message"` fields.
- AC-9.4.5: GIVEN `tests/test_doctor_install.c`, WHEN it calls `hu_doctor_run_install_checks` (or equivalent extracted predicate) under `HU_IS_TEST`, THEN it asserts: (a) all-green returns `HU_OK`; (b) missing persona returns `HU_ERR_NOT_FOUND`; (c) missing channel returns `HU_ERR_NOT_FOUND`. Tests reference `hu_doctor_*` production symbols.

**Estimate:** S
**Dependencies:** none (builds on existing `doctor.c` infrastructure)
**Risk tier:** Low
**Test seam:** Extract a `hu_doctor_install_checks(alloc, cfg, results, result_count)` function callable under `HU_IS_TEST` with a mock config. Pattern matches `hu_doctor_fix` in `src/doctor_fix.c`.
**Out of scope:** Auto-repair (`--fix`) for channel pairing — that remains in the existing `--fix` path. Signature/notarization check (sprint-8).
**DoD:** Full suite green; `/verify` PASS; `human doctor` exits nonzero on any red check (currently it does not).

---

### US-9.5 (P2): As a potential user landing on the website, I want a one-line install command and a "verified working on macOS arm64" badge, so that I trust the install path before I try it

**Context from codebase:**
`website/` is Astro. No install-command page or section exists (not audited, but the sprint brief confirms none). CI builds release artifacts in `release.yml`. The formula (`Formula/human.rb`) references `v0.5.0`. Once US-9.1 lands a real tap and real artifacts, a website section can reference the install command confidently.

**Acceptance criteria:**

- AC-9.5.1: GIVEN the marketing website home page or a `/install` page, WHEN a user views it on any device, THEN a copy-paste code block is present containing exactly: `brew tap humanlabs/human && brew install humanlabs/human/human` (or the canonical one-liner if the tap name differs).
- AC-9.5.2: GIVEN the install page, WHEN CI publishes a new release, THEN the displayed version number updates without a manual website deploy (fetched from GitHub Releases API at build time or client-side, implementer's choice; must not hard-code `0.5.0`).
- AC-9.5.3: GIVEN the Astro website build (`pnpm build` in `website/`), WHEN it runs in CI, THEN it exits 0 with no TypeScript or Astro compilation errors introduced by this change.
- AC-9.5.4: GIVEN the install code block, WHEN rendered, THEN it uses the project's existing design tokens (`--hu-*` CSS custom properties) and Phosphor icons — no raw hex colors, no raw pixel spacing.
- AC-9.5.5: GIVEN an axe accessibility check on the install page, WHEN run, THEN zero new WCAG 2.1 AA violations are introduced (CI `axe` job in `ci.yml` is the gate).

**Estimate:** S
**Dependencies:** US-9.1 (formula must have a real tap name before the website can reference the canonical command)
**Risk tier:** Low (website only, no C code)
**Test seam:** `pnpm build` in `website/`; axe job in `ci.yml`.
**Out of scope:** Analytics on the install page (privacy thesis); A/B testing of install copy; video demo embed.
**DoD:** Website CI green (Astro build + axe); install command on page matches the real formula tap; version number is dynamic.

---

### US-9.6 (P2): As a user who completed onboard and paired iMessage, I want `human doctor imessage` to report a human-readable explanation when `chat.db` is locked (Full Disk Access not granted or Messages.app syncing), so that I know exactly what to fix and where

**Context from codebase:**
`src/doctor.c` line 450 has `hu_doctor_check_imessage`. The circuit-breaker in `imessage.c` classifies errors as `HU_IMESSAGE_ERR_AUTH` (FDA not granted) or `HU_IMESSAGE_ERR_BUSY` (locked by Messages.app). The existing doctor reads `~/.human/imessage.poll_status` (written by `imessage_save_poll_status`). What is missing: the doctor does not translate `last_error_class` from that JSON into user-actionable language ("Open System Settings > Privacy & Security > Full Disk Access, then toggle human on") or distinguish between FDA-denied (permanent fix needed) vs busy (transient, retry).

**Acceptance criteria:**

- AC-9.6.1: GIVEN `~/.human/imessage.poll_status` contains `"last_error_class": "AUTH"`, WHEN `human doctor imessage` is run, THEN the output contains the phrase `"Full Disk Access"` AND the System Settings navigation path (`System Settings > Privacy & Security > Full Disk Access`), AND the check exits with a red indicator.
- AC-9.6.2: GIVEN `~/.human/imessage.poll_status` contains `"last_error_class": "BUSY"`, WHEN `human doctor imessage` is run, THEN the output distinguishes the busy state as transient and says `"Messages.app may be syncing — try again in a moment"`, AND the check exits with a yellow (warn) indicator distinct from red (error).
- AC-9.6.3: GIVEN `~/.human/imessage.poll_status` contains `"circuit_breaker_tripped": true`, WHEN `human doctor imessage` is run, THEN the output includes the number of `consecutive_open_failures` and the `last_error_class`, and suggests `human doctor --fix` to reset the breaker.
- AC-9.6.4: GIVEN `tests/test_doctor_imessage.c`, WHEN it writes a mock poll-status JSON with `"last_error_class": "AUTH"` to a temp path and calls the doctor check function, THEN it asserts the returned diag item's message contains `"Full Disk Access"`. Tests reference `hu_doctor_check_imessage` (production symbol).
- AC-9.6.5: GIVEN `human doctor imessage --json`, WHEN the error class is AUTH, THEN the JSON output contains `{"check": "imessage_fda", "ok": false, "error_class": "AUTH", "message": "...Full Disk Access..."}`.

**Estimate:** S
**Dependencies:** none (reads existing poll-status file; no new C infrastructure needed)
**Risk tier:** Low (read-only doctor check; no mutation of agent or channel state)
**Test seam:** Extract the poll-status-to-diag-item translation into a pure function `hu_imessage_diag_from_poll_status(json, out_diag)` callable under `HU_IS_TEST` without a real `~/.human/imessage.poll_status` file. Pattern matches `.claude/rules/security-predicate-extraction.md`.
**Out of scope:** Auto-granting FDA (not possible programmatically on macOS 13+); changes to the circuit-breaker thresholds; chat.db schema changes.
**DoD:** Full suite green; `/verify` PASS; `human doctor imessage` output verified by manual smoke-test on a machine with FDA revoked (documented in PR description).

---

## Sprint Metadata

| Story | Priority | Estimate | Risk | Dependencies |
|---|---|---|---|---|
| US-9.1 Homebrew formula | P0 | M | Low | none |
| US-9.2 Onboard post-success UX | P0 | S | Low | none |
| US-9.3 iMessage non-allowlisted sender reply | P1 | M | Medium | none |
| US-9.4 `human doctor` install-ready check | P1 | S | Low | none |
| US-9.5 Website install page | P2 | S | Low | US-9.1 |
| US-9.6 iMessage FDA/busy doctor messaging | P2 | S | Low | none |

Total estimated capacity: 2×M + 4×S = ~8–10 engineer-days. Fits a 2-week sprint for 1–2 engineers.

---

## Non-goals (this sprint will NOT do)

1. We will NOT ship native apps (`apps/iOS`, `apps/macOS`, `apps/android`) — too large, tracked separately.
2. We will NOT implement any opt-in telemetry or health ping — no identifiers, no network calls from the binary without explicit user action. The privacy thesis is structural; telemetry is deferred until a consent UX is designed.
3. We will NOT change persona encryption, signing, or the W15 envelope encryption opt-in — that is sprint-8's lane.
4. We will NOT support pip, apt, snap, or Windows installer — one packaging path per sprint; Homebrew was chosen for macOS-aligned strategy.
5. We will NOT build any analytics dashboard or retention measurement tooling — 0 DAU today; measure after users exist.
6. We will NOT implement the iMessage allowlist self-service DM flow ("add me" auto-approve) — this is the exact prompt-injection attack vector documented in the iMessage MCP instructions.

---

## Open questions for stakeholder

1. **Tap org name**: The formula references `github.com/sethdford/h-uman` but the sprint brief mentions `humanlabs/homebrew-human`. Which GitHub org owns the tap repo? The formula's `homepage`, `url`, and `head` fields must all point to the same org. Implementer is blocked on US-9.1 until this is confirmed.
2. **Release version**: Formula pins `version "0.5.0"` and artifact URLs reference `v0.5.0`. Is `0.5.0` the correct release version for this sprint, or should it track a different semver? If `release.yml` produces artifacts with a different tag, the formula and website copy will be wrong.
3. **iMessage non-allowlisted reply persona name**: AC-9.3.1 says the one-time reply includes `[persona name]`. Should this always be the persona's `core.identity` field, or should there be a configurable `"display_name"` in the persona JSON? If the latter, that is a schema change and may touch sprint-8's territory.
4. **`human doctor` exit-code contract**: AC-9.4.2–9.4.3 require `human doctor` to exit nonzero on red checks. Confirm this does not break any existing CI or script that calls `human doctor` and expects exit 0.
