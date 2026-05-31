---
title: Sprint C Backlog — Distribution
status: closed
created: 2026-05-25
last_audit: 2026-05-25
---

# Sprint C Backlog — Distribution

**Drafted:** 2026-05-25 (post Sprint B + v2026.5.x close)
**Theme:** Make h-uman something users can install.
**Story sizing:** 1500-3000 LoC, multi-session each.
**Release strategy:** No tags. Ship to main continuously.

---

## Why "Distribution" now

Sprint B closed the persona-deep memory + observe→adapt loop end-to-end.
The product (technically) works. But "works for me on a clean checkout
with `cmake --preset dev && cmake --build`" is not "works for someone
who downloads it." The strategic mission [M4 — Ship to Users](../../CLAUDE.md)
puts a 100 DAU goal on the table; we cannot reach 100 DAU without
solving the distribution surface first.

Sprint C is the slow-cooked work that compounds: every onboarding
friction we eliminate compounds across every future user. Five
larger stories instead of ten small ones because each surface (.pkg
installer, onboarding wizard, doctor v2) has a coherent shape that
breaks if you split it.

## Anti-goals for Sprint C

Things we are NOT building:
- **New features.** Persona depth, audio model, federation — all
  deferred. The product surface is locked at v2026.5.2's capability
  set.
- **Cloud/server.** Distribution = on-device; no SaaS hosting,
  no remote inference fallback as a default.
- **Mobile apps.** iOS/Android apps exist in `apps/`; Sprint C is
  about the macOS daemon + CLI surface.
- **Auto-update with code execution.** Self-updating binaries are
  a security surface we won't open in this sprint. Sprint C ships
  installable artifacts; users update by re-installing.

## Sequencing recommendation

C1 → C2 → C3 → C4 → C5, mostly in series because each story
builds on the previous one's deliverable:

```
C1 (installer)   produces a signed binary that
   ↓
C2 (onboarding)  runs on first launch and discovers
   ↓
C3 (doctor v2)   verifies a healthy install and
   ↓
C4 (telemetry)   reports first-run + activation events
   ↓
C5 (getting-started)  is the doc that ties it all together
```

A 2-week vacation in the middle of any one story is fine — they're
sized for multi-session work.

---

## C1 — Installable artifacts for macOS

**Why:** Today's user runs `cmake --preset dev && cmake --build`,
which requires Xcode tools, brew dependencies, and 5-10 minutes.
That filter eliminates 99% of plausible users. A .pkg installer
that drops a signed `/Applications/Human.app` is the minimum bar
for "can a curious person try this in 60 seconds."

**Acceptance:**
- `human --version` runs from a fresh download on a clean Mac (no
  Xcode required)
- macOS Gatekeeper does NOT scare the user (Developer ID signed +
  notarized)
- `brew install human` works as an alternative install path
- The .pkg installer drops daemon, CLI, MLX server bootstrapper,
  and writes a launchd plist so the daemon starts on login
- Uninstall is one-step (`/Applications/Human.app` → Trash AND a
  documented `human uninstall` CLI for the daemon + config)

**Files to create / modify:**
- `scripts/release/build-pkg.sh` (new, ~250 LoC)
- `scripts/release/sign-and-notarize.sh` (new, ~200 LoC)
- `Formula/human.rb` (brew formula, ~80 LoC)
- `apps/macOS/Human.app` skeleton (Info.plist + bundle layout)
- `.github/workflows/release-macos.yml` (CI for building + signing
  release artifacts)
- `docs/guides/installation.md` (new, ~200 lines)

**Dependencies:** Apple Developer ID certificate (user-provided
secret); `xcrun notarytool` on the CI runner.

**Anti-goals:** No auto-update mechanism. No telemetry from the
installer itself (telemetry is C4's surface, opt-in only).

**Risk:** Notarization is Apple's process; failures can be opaque
("rejected, see log"). Build a `scripts/release/diagnose-notary.sh`
that translates common rejection codes into actionable fixes.

**Estimated:** ~2500 LoC across scripts + workflows + the .app
skeleton; 2-3 multi-session work units.

---

## C2 — Onboarding wizard redesign

**Why:** `human onboard` exists today as a starter wizard (per
v2026.5.0 docs), but the first-run experience asks the user too
much too fast — provider config, persona JSON, channel selection
all in one flat session. A new user should reach "I sent a message
and the agent replied" inside 5 minutes.

**Acceptance:**
- On first launch (no `~/.human/config.json` exists), `human` AUTO-runs
  `human onboard`
- Onboarding is a five-step state machine, each step recoverable:
  1. **Welcome + privacy explanation** — what data lives where,
     what never leaves the device
  2. **Provider setup** — local MLX (download + start) OR cloud
     (`anthropic`/`gemini`/`openai` API key paste) OR both (cloud
     fallback). Test the connection live; show success / failure
     with actionable diagnostics
  3. **Persona seed** — pick from 3 starter templates (casual,
     formal, technical) OR import from a markdown file. Templates
     fill in starter example banks for Tier 1 channels
  4. **Channel configuration** — currently asks 31 questions;
     redesign to ask ONLY about Tier 1 channels first (iMessage,
     Slack, Discord, Telegram), with "set up more channels later"
     as the default path
  5. **Test send** — pick one allowlisted contact, send a real
     message, wait for the reply, confirm everything worked
- Each step has a `< back` option to revisit prior decisions
- The user can `Ctrl-C` at any step and resume from where they
  left off via `human onboard --resume`
- `~/.human/onboard-state.json` persists progress

**Files:**
- `src/onboard/onboard.c` (extensive rewrite, ~1500 LoC)
- `include/human/onboard.h` (new state-machine API)
- `tests/test_onboard.c` (state-machine transitions, resume from
  each step, ~400 LoC)
- `docs/guides/first-run.md` (new, ~150 lines)

**Dependencies:** C1 (the installable binary that triggers first-run).

**Anti-goals:** Don't try to be a GUI. CLI prompts only; the
"native app" vibe comes from doing the right things in the right
order, not from animation.

**Scope cuts:** No multi-user onboarding (single user assumed).
No teams/workspaces. No cloud account creation; user provides
keys.

**Estimated:** ~2000 LoC, 2 multi-session work units.

---

## C3 — `human doctor` v2

**Why:** `human doctor` exists today as a system diagnostic. Most
output is engineer-facing. For users, the right question is
narrower: "is this installation actually working?" Sprint C makes
`human doctor` the canonical "should this work right now?" check
that users run when something feels off.

**Acceptance:**
- `human doctor` returns 0 (everything ok) or non-zero (something
  needs attention) with a clear human-readable summary
- Checks cover the full inbound→outbound pipeline:
  - Provider reachable + responds to a smoke prompt
  - `~/Library/Messages/chat.db` readable (with macOS Full Disk
    Access permission request flow if denied — clear instructions
    + a direct link to System Settings)
  - MLX server up + adapter file present + adapter version recorded
  - Autoresponder config valid (parses, allowlist non-empty)
  - Identity graph loaded (or "no file — that's fine for first run")
  - Last successful daemon tick within the expected interval per
    tick type (social_tick, autodream, lora_nightly)
  - All persona prompt blocks emit non-empty output for at least
    one known contact (regression signal that the blocks are wired)
- Output supports `--json` for scripting + dashboard ingest
- `--fix` mode attempts the obvious remediations (restart MLX,
  re-init social_state.json, etc.) with explicit confirmation

**Files:**
- `src/doctor/doctor.c` (rewrite of the existing diagnostic, ~1200 LoC)
- `include/human/doctor.h` (check registry API; each check is a
  vtable with `name`, `run`, `fix`)
- `tests/test_doctor.c` (registry + per-check happy + sad paths,
  ~400 LoC)

**Dependencies:** None hard; can ship before or after C2.

**Anti-goals:** No remote diagnostics. No "phone home" mode (that's
C4 territory and opt-in only). Don't autofix things that touch
user data without explicit confirmation.

**Estimated:** ~1500 LoC, 1-2 multi-session work units.

---

## C4 — Opt-in first-run + activation telemetry

**Why:** We can't improve what we can't measure. Sprint C ships
to users; without aggregate signal we won't know if onboarding
crashes for 30% of installs, if the autoresponder never fires for
80% of users, if the lora-nightly job has a 1% failure rate at
scale. The trick is collecting useful signal WITHOUT betraying
the "private-by-architecture" thesis.

**Acceptance:**
- All telemetry is OPT-IN with an unambiguous prompt during C2
  onboarding ("share anonymized installation + crash events with
  the project? you can revoke any time")
- Events are aggregated locally in `~/.human/telemetry.jsonl` BEFORE
  any network send (user can read what would be sent, verbatim)
- Only these event types are ever collected:
  - `install_completed` (version, os.release, no user-id)
  - `onboarding_step_completed` (step_name, duration_seconds)
  - `daemon_crash` (signal, last 4KB of log, sanitized of PII via
    existing hu_pii_redact)
  - `tick_fired` (tick_name, success/failure — for measuring real-
    world job-success-rate)
- No message content, contact handles, fact text, or persona data
  is EVER collected. The opt-in screen says this in plain English.
- Daily batch upload to a static endpoint (TBD; provision matters
  less than the surface)
- `human telemetry status` shows what's been collected; `human
  telemetry revoke` permanently disables AND deletes the local
  buffer
- Includes a `tests/test_telemetry_no_pii.c` adversarial test that
  feeds known-PII strings through the redactor and asserts
  nothing leaks

**Files:**
- `src/telemetry/collector.c` (new, ~800 LoC)
- `src/telemetry/uploader.c` (new, ~400 LoC)
- `include/human/telemetry.h` (new)
- `tests/test_telemetry.c` + `tests/test_telemetry_no_pii.c`
  (~600 LoC combined)

**Dependencies:** C2 (the onboarding step that asks consent).

**Anti-goals:** **Never collect message content.** Never collect
contact handles. Never use telemetry for advertising or third-
party analytics. The opt-in screen must say all of these explicitly.

**Risk:** Privacy mistakes here destroy the product's core trust
story. Adversarial PII test must pass with zero leaks before this
ships. Worth a /aspect-panel review pre-merge.

**Estimated:** ~1800 LoC, 2 multi-session work units.

---

## C5 — Getting-started flow + docs

**Why:** Even a perfect installer + wizard won't reach users who
can't find them. A `getting-started.md` that's discoverable from
the GitHub README and a `human help getting-started` interactive
walk-through close the discovery gap.

**Acceptance:**
- `docs/guides/getting-started.md` (new) — single page, top of
  the project README links to it
- Covers: install (link to C1's installation.md), launch, what
  you'll see during onboarding, how to verify it's working
  (`human doctor`), what to do if something fails (`human help
  troubleshoot`)
- `human help getting-started` is an interactive in-CLI guide
  with prompts ("press enter to continue") — useful when the user
  is already in the terminal and wants pointer-by-pointer guidance
- Screenshots in docs/guides/img/ showing the wizard, the doctor
  output, a successful first message
- README.md updated: install link prominent, link to getting-
  started as the FIRST link under "Quick start"

**Files:**
- `docs/guides/getting-started.md` (new, ~400 lines)
- `docs/guides/img/` (new, ~5 PNG screenshots)
- `src/app/cli_commands.c` (`human help getting-started` handler,
  ~200 LoC)
- `README.md` (updated)

**Dependencies:** C1, C2, C3 (this is the doc that explains all of
them).

**Anti-goals:** Don't write a tutorial that recreates the
onboarding wizard. The wizard IS the tutorial; the doc points at
it and explains what to expect.

**Estimated:** ~800 LoC (mostly markdown), 1 multi-session work unit.

---

## Sequencing in calendar terms

If each multi-session work unit is ~1 week of focused effort:

| Story | Work units | Approx calendar |
|-------|------------|----------------|
| C1 — Installer  | 2-3 | ~3 weeks |
| C2 — Onboarding | 2   | ~2 weeks |
| C3 — Doctor v2  | 1-2 | ~1.5 weeks |
| C4 — Telemetry  | 2   | ~2 weeks |
| C5 — Docs       | 1   | ~1 week |
| **Total**       | **8-10** | **~9-10 weeks** |

That's a real quarter of distribution work. The output is
shippable to users at the end of C2 (with the rest as polish
that compounds afterward).

## What's NOT in Sprint C (deferred)

The honest list of things this sprint defers:
- **Auto-update.** Distribution v1 is "re-download to upgrade."
- **Sandboxed daemon.** Code-signing requires entitlements review;
  Sprint C ships unsandboxed (same as today).
- **Multi-user / household contexts.** Would multiply persona
  surface area; Sprint D+.
- **iOS/Android.** `apps/` directory exists; not in Sprint C scope.
- **Cloud sync / backup.** "Private-by-architecture" thesis
  forbids it. Users back up `~/.human/` themselves.
- **GUI / Electron app.** CLI + native macOS .app launcher only.

## Release strategy

Per the brainstorming decision: **no tags during Sprint C; ship
to main continuously.** The story-level "done" gate is `cmake
build clean + full test suite green + manual verification of the
new surface.` No version bumps; users install latest from the
release artifact CI produces on every main push.

The next tag (whenever it lands) is `v2026.6.0` and marks the
Sprint C arc complete.

## Related

- [`CLAUDE.md`](../../CLAUDE.md) — M4 strategic mission
- [`docs/plans/2026-05-19-sprint-backlog.md`](2026-05-19-sprint-backlog.md) — Sprint B backlog
- [`docs/eval/prompt-blocks-2026-05-24.md`](../eval/prompt-blocks-2026-05-24.md) — what Sprint B shipped that Sprint C builds on top of
- [`docs/guides/m3-bridge-runbook.md`](../guides/m3-bridge-runbook.md) — M3 path; Sprint C's C3 doctor verifies it's working
