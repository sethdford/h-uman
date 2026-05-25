---
title: "Sprint 51 — Sprint C / C2: Onboarding wizard redesign"
created: 2026-05-24
sprint: 51
branch: sprint-51-onboarding-v2
spec: docs/plans/2026-05-25-sprint-c-backlog.md (§C2, lines 104-153)
program: docs/plans/2026-05-25-sprint-c-backlog.md
status: planned (PO + design only — implementation deferred to a follow-up multi-session work unit)
---

# Sprint 51 — Onboarding wizard redesign

## Goal

`human onboard` exists today, but the first-run experience asks too
much, too fast. A new user must reach **"I sent a message and the
agent replied"** inside 5 minutes. Sprint 51 turns the wizard from
"every config knob, flat" into a **five-step recoverable state
machine** with live connection testing and the smallest possible
initial decision surface.

The strategic frame from backlog §C2: this is the user's FIRST 5
minutes with the product. Every friction we remove compounds across
every future user.

## Scope discipline

- **In scope:** state-machine refactor of `src/onboard.c`, resume-from-step
  semantics, live provider-connection testing, Tier-1-only channel ask,
  `--resume` flag, `~/.human/onboard-state.json` persistence, real "test
  send" loop in Step 5.
- **Out of scope:** GUI, web onboarding, multi-user / household, cloud
  account creation (user provides keys), persona authoring beyond the 3
  starter templates, channels beyond Tier 1 (Telegram, Discord, iMessage,
  Slack) — the wizard explicitly defers those with "set up more channels
  later" as the default path.

## Wave plan (deferred to implementation sprint)

```
US-C2.1 (state-machine skeleton + onboard-state.json) ──┐
                                                        ├── US-C2.2 (Step 1: welcome + privacy)
                                                        ├── US-C2.3 (Step 2: provider setup + live smoke)
                                                        ├── US-C2.4 (Step 3: persona seed)
                                                        ├── US-C2.5 (Step 4: Tier-1 channels)
                                                        └── US-C2.6 (Step 5: test send loop)
                                                                  │
                                                                  ↓
                                                        US-C2.7 (--resume + back navigation)
                                                                  │
                                                                  ↓
                                                        US-C2.8 (first-run auto-trigger)
```

US-C2.1 is foundation; once landed, US-C2.2–C2.6 are parallelizable per
implementer (each step is independent given the state-machine contract).
US-C2.7 and C2.8 are serial — both need every step to exist.

Estimate matches backlog: **~2000 LoC, 2 multi-session work units.**

## Stories

### Story US-C2.1 — State-machine skeleton + persistent state

**As** the onboarding wizard architecture,
**I want** every step to be an entry in a step-table with
            `enter(state)`, `validate(state)`, `next(state)`, `back(state)`
            semantics, persisted to `~/.human/onboard-state.json` after
            every transition,
**So that** the user can `Ctrl-C` at any moment and resume cleanly via
            `human onboard --resume`, and so that new steps can land
            without rewriting flow control.

**Acceptance criteria:**

1. `include/human/onboard/state.h` (NEW) defines `hu_onboard_state_t`
   with: current step enum, per-step data union (provider config,
   persona template choice, channel selections, test-send result), step
   history for `< back`.
2. `include/human/onboard/step.h` (NEW) defines `hu_onboard_step_t`
   vtable: `name`, `enter`, `prompt`, `validate`, `apply`, `next_default`.
3. `src/onboard/state.c` (NEW) — load/save `onboard-state.json` atomically
   (tmp + fsync + rename, per `tests/test_personal_model_atomic_save.c`
   pattern).
4. State JSON schema versioned (`{"version":1,...}`); a v2 schema would
   require a migration path that's out of scope here.
5. Tests: `tests/test_onboard_state.c` — round-trip save/load, atomic-
   save under tmp-blocked adversary, version-mismatch rejection.
6. Existing `src/onboard.c` keeps its current entry point but delegates
   step execution to the new dispatcher.

**Files expected to change:**

- `include/human/onboard/state.h` (NEW)
- `include/human/onboard/step.h` (NEW)
- `src/onboard/state.c` (NEW, ~200 LoC)
- `src/onboard/dispatcher.c` (NEW, ~150 LoC — runs the state-machine)
- `src/onboard.c` (~80 LoC delta — delegate to dispatcher)
- `tests/test_onboard_state.c` (NEW, ~200 LoC)
- `tests/test_main.c`
- `CMakeLists.txt`

---

### Story US-C2.2 — Step 1: Welcome + privacy explanation

**As** a new user launching `human` for the first time,
**I want** the first onboarding screen to explain in plain English
            (5-8 lines max) what data lives where and what never leaves
            the device,
**So that** the "private-by-architecture" thesis is something I
            understand BEFORE I paste an API key, not a footnote I
            discover after I've already shared data.

**Acceptance criteria:**

1. Step renders the welcome text from `docs/copy/onboarding-step1.md`
   (a NEW canonical copy file — not inline-strings — so a wording fix
   is a one-line PR without recompiling).
2. Text covers: where chat.db ingest stays, where the LoRA training
   runs, where the personal-model database lives, what crosses the
   network ONLY when the cloud provider is configured.
3. User presses Enter (or `n`) to continue; `q` to quit gracefully
   (writes state, exits 0, prints resume instruction).
4. Test: `tests/test_onboard_step1.c` — captures rendered output,
   asserts the four privacy bullets are present verbatim. A
   regression on the copy file fails the test (forcing the author of
   the copy change to also update the test, which forces them to
   read what they're changing).

**Files expected to change:**

- `docs/copy/onboarding-step1.md` (NEW)
- `src/onboard/step_welcome.c` (NEW, ~80 LoC)
- `tests/test_onboard_step1.c` (NEW, ~120 LoC)

---

### Story US-C2.3 — Step 2: Provider setup with live connection smoke

**As** a new user picking a model provider,
**I want** the wizard to test the connection LIVE with a 1-token
            prompt and show me a green-or-red result before moving on,
**So that** a stale key, a paste with a trailing newline, or an
            unreachable provider surfaces NOW — not three messages
            later when the autoresponder silently drops a real reply.

**Acceptance criteria:**

1. Step presents three options: local MLX (download + start), cloud
   (anthropic / gemini / openai — paste API key), both (cloud
   fallback when local fails).
2. For cloud: after paste, fires `hu_provider_create` + a 1-token
   `complete("ok")` call. Time-bounded at 10s.
3. PASS: green "✓ connected" line, persists `provider.<name>` block
   to in-progress `~/.human/config.json`.
4. FAIL: surfaces one of (`credentials missing`, `credentials invalid
   401/403`, `rate limited 429`, `unreachable`) with actionable next
   step ("Check the key starts with `sk-` and has no trailing
   spaces"). User chooses retry / pick different provider / skip
   provider for now.
5. For local MLX: kicks off the model download in the background;
   the step blocks ONLY on confirming the download started, not on
   completion (downloads are 4-30 GB — completion happens during
   Step 3+).
6. Behind `HU_IS_TEST`: NO real network calls. Mock provider with
   injectable failure modes. Same discipline as
   `sprints/sprint-50/stories.md US-C3.3`.
7. Tests cover EACH failure branch + the PASS path.

**Files expected to change:**

- `src/onboard/step_provider.c` (NEW, ~300 LoC)
- `tests/test_onboard_step_provider.c` (NEW, ~400 LoC)

---

### Story US-C2.4 — Step 3: Persona seed (3 templates + markdown import)

**As** a new user with no time to write a persona JSON by hand,
**I want** the wizard to offer three starter templates (casual,
            formal, technical) and seed example banks for the Tier-1
            channels,
**So that** the agent has SOMETHING to respond like before I've
            written a single line of persona config.

**Acceptance criteria:**

1. Step displays three templates with a one-paragraph preview each
   (sample reply showing tone differences).
2. User picks 1/2/3, OR provides a path to a markdown file to import.
3. Selected template renders into `~/.human/personas/<user-handle>.json`
   using the existing `hu_starter_persona_json` infrastructure (commit
   `71de40e6` already ships Tier-1 example banks for telegram /
   discord / imessage / slack — re-use, don't reinvent).
4. Import path: parses markdown front-matter for traits / values /
   communication-rules; validates via `hu_persona_validate`. On
   validation failure, surfaces the rejected field + suggested fix.
5. Tests: per-template generation produces a parseable persona file
   with non-empty Tier-1 example banks; import-from-markdown happy
   path; import with malformed front-matter fails gracefully.

**Files expected to change:**

- `src/onboard/step_persona.c` (NEW, ~250 LoC)
- `src/onboard/persona_templates.c` (NEW, ~200 LoC — three
  hard-coded template bodies)
- `tests/test_onboard_step_persona.c` (NEW, ~350 LoC)
- `tests/fixtures/onboard_persona_import.md` (NEW)

---

### Story US-C2.5 — Step 4: Tier-1 channel setup only

**As** a new user who just wants to try iMessage first,
**I want** the channel-setup step to ask ONLY about the four Tier-1
            channels (iMessage, Slack, Discord, Telegram) with "set up
            more later" as the default path,
**So that** I'm not making 31 yes/no decisions before I've sent a
            single test message.

**Acceptance criteria:**

1. Step lists the four Tier-1 channels with their current setup state
   (configured / available / requires-permission).
2. For each: PROMINENT default action (enter to enable with sane
   defaults; `s` to skip).
3. iMessage triggers the chat.db FDA check (see Sprint 50 US-C3.2) —
   if FDA not granted, surfaces the System Settings deep link AND
   marks the channel as "pending" so the test-send step (US-C2.6)
   skips it.
4. The remaining 27 channels are listed under a one-line dimmed
   footer: "27 more channels available — run `human channels list` to
   see them, `human channels enable <name>` to add later." No
   per-channel prompts in the wizard.
5. Test: per-channel skip / enable transitions; FDA-pending state for
   iMessage; the dimmed footer is always present.

**Files expected to change:**

- `src/onboard/step_channels.c` (NEW, ~200 LoC)
- `tests/test_onboard_step_channels.c` (NEW, ~250 LoC)

---

### Story US-C2.6 — Step 5: Test send loop

**As** a new user finishing onboarding,
**I want** the wizard to send a real test message to one of my
            allowlisted contacts and wait for the reply, confirming
            everything actually works end-to-end,
**So that** I leave onboarding with empirical proof — not faith — that
            the agent is working.

**Acceptance criteria:**

1. Step presents the configured allowlisted contacts (from
   `autoresponder.json`); user picks one.
2. Wizard sends `"hi — this is a test message from human, ignore"` via
   the picked contact's channel.
3. Wizard waits up to 5 minutes for a reply (with a "still waiting…"
   tick every 30s).
4. On reply: green "✓ test passed — your install is working" + transitions
   to onboarding-complete state.
5. On timeout: amber "still waiting — your install is probably fine,
   but we didn't see a reply yet. Try `human doctor` to verify."
   transitions to onboarding-complete state (does NOT block).
6. On send failure: red "test send failed — <reason>" + offers to
   loop back to Step 2 (provider) or Step 4 (channels).
7. Test: send-then-reply happy path (uses the mock channel's reply
   injector); timeout path; send-failure path. NO real network.

**Files expected to change:**

- `src/onboard/step_testsend.c` (NEW, ~280 LoC)
- `tests/test_onboard_step_testsend.c` (NEW, ~350 LoC)

---

### Story US-C2.7 — `--resume` flag + `< back` navigation

**As** a user who Ctrl-C'd during Step 3,
**I want** `human onboard --resume` to drop me back into Step 3 with
            my prior answers prefilled, and a `< back` option in every
            step to revisit a prior decision,
**So that** wizard interruptions are recoverable (network blip, the
            cat walks on the keyboard, etc.) without restarting from
            Step 1.

**Acceptance criteria:**

1. `human onboard --resume` reads `~/.human/onboard-state.json`,
   validates the schema, and enters the saved current step.
2. If state file is missing or invalid: prints "no in-progress
   onboarding to resume — starting fresh" and proceeds with Step 1.
3. Each step's `prompt` includes a "`b` to go back" option except
   Step 1.
4. `< back` rewinds the state-machine to the prior step's saved
   state (history stack, depth bounded at 10 — same as a shell's
   default).
5. Test: resume from each of the 5 steps lands in that step with
   prior answers visible; `b` from Step 3 lands in Step 2 with the
   provider config preselected; `b` from Step 1 is a no-op (no-op
   confirmed by a test, not just absence of code).

**Files expected to change:**

- `src/onboard/dispatcher.c` (extension — back navigation)
- `src/onboard.c` (`--resume` CLI flag, ~30 LoC)
- `tests/test_onboard_resume.c` (NEW, ~250 LoC)

---

### Story US-C2.8 — First-run auto-trigger

**As** a brand-new user typing `human` for the first time,
**I want** the binary to detect no `~/.human/config.json` exists and
            AUTO-run `human onboard` without me knowing to invoke it,
**So that** the wizard is the discovery path — not something I have
            to find in the docs first.

**Acceptance criteria:**

1. `src/main.c` or `src/cli_commands.c` checks for the existence of
   `~/.human/config.json` before dispatching the user's command.
2. If missing: prints one-line "First run detected. Starting setup…"
   and runs `human onboard`.
3. If the user's command was something other than the default
   (e.g. `human --version`, `human doctor`, `human help`), the
   auto-onboard is SKIPPED — those are diagnostic commands that
   should work without onboarding.
4. After onboarding completes, the user's original command is dispatched
   (so `human send alice "hi"` does first-run-onboard, then sends).
5. Auto-trigger gated behind a TTY check — non-TTY invocation (CI,
   scripts, piped input) skips the wizard and prints an error
   pointing to the docs.
6. Test: `tests/test_first_run_trigger.c` — temp HOME with no config,
   simulated TTY → onboarding runs; same temp HOME, simulated
   non-TTY → onboarding skipped, exit 1 with doc-link error.

**Files expected to change:**

- `src/main.c` OR `src/cli_commands.c` — auto-trigger (~80 LoC)
- `tests/test_first_run_trigger.c` (NEW, ~200 LoC)

---

## Definition of Done (sprint-level)

Identical to Sprint 50 §DoD. Plus:

- The five-step state machine MUST round-trip via resume from EVERY
  step in an integration test (`tests/test_onboard_e2e_resume.c`).
- A real test send via the mock channel MUST complete in <2 seconds in
  the test path (the user-facing timeout is 5 min, but the test
  injector should reply immediately).

## Risk register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| Live provider smoke leaks billing in dev runs | High | Medium | Same `HU_IS_TEST` discipline as Sprint 50 §C3.3 — pre-commit script enforces guard on `src/onboard/step_provider.c` |
| State-machine resume races with a partial config write | Medium | High | Atomic save (tmp+fsync+rename) per personal-model atomic-save pattern. Validated by tmp-blocked adversary test. |
| Auto-trigger fires inside a CI run that has no config but ALSO no TTY | High | Medium | TTY check is the structural guard; pin with a test using `setsid` to detach from any TTY. |
| Wizard's "test send" reaches a real person during dev | High | High (privacy) | Test-send step is dispatcher-gated; under HU_IS_TEST the channel send is intercepted by the mock. The pre-commit hook from §G1 (tests-that-pin-bugs) catches if the mock isn't actually called. |

## Anti-goals (re-stated from backlog §C2)

- **No GUI.** CLI prompts only.
- **No multi-user.** Single-user assumed.
- **No teams/workspaces.**
- **No cloud account creation.** User pastes their own API keys.
