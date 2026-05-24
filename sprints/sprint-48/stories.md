# Sprint 48 Backlog: Close A-Loop Autoresponder End-to-End

## Goal
Deliver a dogfoodable A-loop autoresponder on iMessage for seth with measurable persona fidelity, per-contact memory integration, and proactive messaging — all enabled by default in `human onboard`.

## User Stories (in priority order)

### US-48-1 (P0): Validate A-loop autoresponder against seth's chat history
**As a** product owner validating A-loop readiness, **I want** a blind eval showing persona-fidelity comparison of autoresponder-generated replies vs baseline LLM replies against seth's last 50 iMessage conversations, **so that** we can quantify whether the A-loop actually adapts to seth's style.

**Acceptance criteria:**
- AC-1.1: Eval framework loads seth's iMessage chat.db (self-chat + allowlisted contacts)
- AC-1.2: For each conversation, extract 3 test messages; run through autoresponder with persona overlay active
- AC-1.3: Compare autoresponder replies vs baseline (same prompt without persona) via scored rubric: tone match, length match, formality match (0–10 each)
- AC-1.4: Blind eval reports aggregate score improvement; persona-aware ≥baseline at p50 or higher (≥60% win rate on message pairs)
- AC-1.5: Eval output includes per-contact breakdown so seth can spot false positives

**Test surface:** `tests/test_autoresponder_eval.c` (new)
**Estimate:** M
**Risk:** medium — rubric subjectivity; we may need to manually curate examples if eval shows near-parity
**Dependencies:** US-48-2 (per-contact M2 must be wired first to have persona context in autoresponder)
**Parallel-safe with:** US-48-3, US-48-4

---

### US-48-2 (P0): Wire per-contact M2 personal-model slice into iMessage agent turn
**As a** developer, **I want** the iMessage agent turn to load and inject the per-contact slice of `hu_personal_model_t` (typed facts + half-life decay) into the autoresponder prompt, **so that** tone/style are personalized per contact.

**Acceptance criteria:**
- AC-2.1: `hu_agent_turn_imessage()` calls `hu_personal_model_load_for_contact(contact_handle)` before autoresponder prompt assembly
- AC-2.2: Fact extraction fires on ingest: `hu_fact_extract()` parses seth's prior message for propositional/prescriptive triples (subject/predicate/object + confidence + trust tier)
- AC-2.3: Facts are stored in `~/.human/models/per_contact/<handle>.db` with 90-day exponential half-life; `hu_heuristic_fact_effective_confidence()` applies decay
- AC-2.4: Autoresponder `hu_autoresponder_generate_prompt()` includes "Contact insights: [top 3 facts by effective confidence]" section
- AC-2.5: Test fixture: `tests/test_personal_model_per_contact.c::test_half_life_decay_applies_to_contact_facts` — loads a 30-day-old fact, verifies confidence dropped to ~53%

**Test surface:** `tests/test_personal_model_per_contact.c` (new), `tests/test_fact_extract.c` (extend)
**Estimate:** M
**Risk:** medium — per-contact DB is new; initial proof-of-concept required that SQLite schema isolation works correctly
**Dependencies:** none
**Parallel-safe with:** US-48-1, US-48-3, US-48-4

---

### US-48-3 (P1): Wire follow-up watcher + daemon flush into iMessage proactive path
**As a** seth, **I want** the daemon to detect when I read a message from a contact without replying, compute an appropriate follow-up delay via `hu_followup_compute_send_time()`, and send a proactive "checking in" draft within the contact's active hours, **so that** I stay connected without manual effort.

**Acceptance criteria:**
- AC-3.1: `hu_daemon_tick_follow_up_watcher()` calls `hu_follow_up_watcher_detect_unresponded()` on iMessage chat.db every 5 min (configurable)
- AC-3.2: When a read-but-unreplied message is detected, `hu_followup_compute_send_time()` computes warmth tier + delay; result stored in `follow_up_scheduled` table
- AC-3.3: `hu_conversation_flush_scheduled_for(contact, time_window)` triggers send when the contact enters active hours; uses `hu_autoresponder_generate()` to draft the follow-up
- AC-3.4: Proactive throttle gate (`hu_proactive_throttle_should_send()`) enforces max 1 proactive msg per contact per day
- AC-3.5: Test fixture: `tests/test_follow_up_daemon_integration.c::test_scheduled_flush_honors_chronotype_active_hours` — schedules a follow-up at midnight, verifies it fires during the contact's awake window, not at 2 AM

**Test surface:** `tests/test_follow_up_daemon_integration.c` (new), extend `tests/test_proactive_throttle.c`
**Estimate:** L
**Risk:** high — daemon loop timing; chronotype evaluation is new code path; easy to ship a follow-up at the wrong time
**Dependencies:** US-48-2 (M2 context in autoresponder required for draft quality)
**Parallel-safe with:** US-48-1, US-48-4

---

### US-48-4 (P1): Audit + harden silent config-gated subsystems
**As a** operator, **I want** every config-gated subsystem (reaction_collection, follow_up_watcher, proactive_throttle) to emit exactly one log line per process when disabled, **so that** a missing config block is not invisible.

**Acceptance criteria:**
- AC-4.1: Grep audit finds all `if (!cfg->X.enabled)` patterns in daemon tick functions
- AC-4.2: Each match adds `hu_log_info_once(subsystem, "X subsystem disabled by config (cfg->X.enabled=false); set X.enabled=true in config.json to activate")`
- AC-4.3: For *enabled* path, add matching one-shot log: "X subsystem activated by config (cfg->X.enabled=true)"
- AC-4.4: Config parser elevates "unknown key" warnings to a single banner at daemon startup: "[config] WARNING: N unknown keys ignored: X, Y, Z — rebuild to use them"
- AC-4.5: Test: `tests/test_config_gated_subsystems.c::test_disabled_reaction_collection_logs_once`, etc. — each test mocks log sink, verifies exactly 1 line per first invocation

**Test surface:** `tests/test_config_gated_subsystems.c` (new), `tests/test_config_parse.c` (extend)
**Estimate:** M
**Risk:** low — mechanical adding of log lines; no behavior change
**Dependencies:** none
**Parallel-safe with:** US-48-1, US-48-2, US-48-3

---

### US-48-5 (P0): Harden onboarding config to enable A-loop + proactive subsystems by default
**As a** seth, **I want** `human onboard` to generate a config that has A-loop + follow-up watcher + proactive throttle enabled for iMessage with sensible defaults, **so that** I don't have to manually edit JSON.

**Acceptance criteria:**
- AC-5.1: `human onboard` interactive flow asks: "Enable autoresponder (reply-while-you're-busy)? [y/n]" — defaults to yes for iMessage
- AC-5.2: If yes, asks for allowlist of contact handles (at minimum seth's own number for self-chat)
- AC-5.3: Config written includes: `autoresponder.enabled=true, autoresponder.allowlist=[seth]`, `follow_up_watcher.enabled=true`, `proactive_throttle.enabled=true`
- AC-5.4: For iMessage specifically, sets reasonable DND window (default: 22:00–08:00 local time, configurable)
- AC-5.5: Test: `tests/test_onboard_aloop.c::test_onboard_generates_aloop_config_for_imessage` — runs wizard, verifies config.json contains all required fields

**Test surface:** `tests/test_onboard_aloop.c` (new), extend `tests/test_onboard.c`
**Estimate:** M
**Risk:** medium — onboarding is a UX surface; easy to miss a prompt or create confusing defaults
**Dependencies:** US-48-3, US-48-4 (subsystems must exist before config can enable them)
**Parallel-safe with:** US-48-1

---

### US-48-6 (P1): Smoke test: daemon sends first proactive message within 60s of human onboard
**As a** seth, **I want** to run `human onboard`, complete it, and see the daemon send a test proactive "checking in" message to my iMessage self-chat within 60 seconds, **so that** I immediately feel the feature working.

**Acceptance criteria:**
- AC-6.1: `human onboard` completion spawns the daemon if it's not running (or is respawned if it crashed)
- AC-6.2: Daemon loads config, enables follow_up_watcher + proactive_throttle, opens iMessage chat.db
- AC-6.3: Daemon finds seth's self-chat, checks for an unresponded message (or uses a synthetic test fixture)
- AC-6.4: Within 60s, daemon computes follow-up delay, snaps to active hours, and sends draft via iMessage
- AC-6.5: Log trace in ~/.human/logs/daemon-follow-up.log confirms: "scheduled follow-up for <handle> at <time>", then "flush_scheduled_for fired, sending draft"

**Test surface:** `tests/test_daemon_aloop_smoke.c` (new, integration test)
**Estimate:** L
**Risk:** high — integrates daemon loop, iMessage channel, follow-up scheduling, and message send; timing-dependent; easy to fail if any async operation stalls
**Dependencies:** US-48-3, US-48-5 (config and daemon wiring must be solid)
**Parallel-safe with:** none (integration test, runs last)

---

## Non-goals
- M3 Bridge B (on-device frontier model personalization) — scheduled sprint 49
- Other Tier-1 channels (Telegram, Discord, Slack) — focusing iMessage in this sprint
- DPO collector hardening for proactive feedback
- Persona Tier-2+ example bank enrichment (using Tier-1 banks from M1)

## Open questions for stakeholder
1. **Eval baseline**: Should the blind eval compare autoresponder replies to (a) raw frontier-model output, or (b) persona-overlay output from a neutral contact? Option (b) is fairer but requires an "unknown contact" baseline.
2. **Per-contact fact storage**: Is SQLite per-contact (one DB per handle) acceptable, or should we consolidate to a single `personal_model.db` with contact-scoped rows? Current plan assumes per-contact SQLite.
3. **Follow-up throttle scope**: Should `max_1_per_day` be per-contact or globally? Recommend per-contact (seth may want daily check-ins from close friends but weekly from acquaintances).
4. **Onboarding allowlist UX**: Should we auto-populate with seth's own iMessage handle, or prompt for it? Recommend auto-detect from config, with override.

## Revision notes

### Sprint sizing & decomposition strategy
This is a **7-story sprint** sized for a **2–3 person team** working in parallel:
- **Stream 1 (Eval + M2)**: US-48-1 (eval framework) + US-48-2 (per-contact facts) — 2 stories, 1 implementer
- **Stream 2 (Proactive)**: US-48-3 (follow-up watcher) — 1 story, 1 implementer
- **Stream 3 (Hardening)**: US-48-4 (config logging) + US-48-5 (onboarding) — 2 stories, can be 1 person
- **Integration (Smoke)**: US-48-6 (daemon smoke test) — 1 story, depends on all streams, brought in late

**Critical path**: US-48-2 → US-48-1 (eval needs M2). US-48-3 depends on US-48-2 indirectly (for draft quality, but not strictly).

**File conflict zones**:
- `src/agent/autoresponder.c` — touched by US-48-1 (eval hooks)
- `src/memory/personal_model.c` — touched by US-48-2 (per-contact slice)
- `src/daemon.c` — touched by US-48-3 (follow-up tick), US-48-4 (log once), US-48-6 (integration)
- `src/cli/onboard.c` — touched by US-48-5 (config prompts)
- `src/config.c` — touched by US-48-4 (unknown key banner), US-48-5 (schema defaults)

**Recommended wave order**:
1. **Wave 1**: US-48-2, US-48-4 (parallel, no conflict)
2. **Wave 2**: US-48-1, US-48-3, US-48-5 (once US-48-2 + US-48-4 land)
3. **Wave 3**: US-48-6 (once all others pass /verify)

RESULT_product-owner=READY
