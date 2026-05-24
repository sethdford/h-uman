# Sprint 48 Plan: Close A-Loop Autoresponder End-to-End

## Sprint Identity

- **Sprint number**: 48
- **Goal**: Deliver a dogfoodable A-loop autoresponder on iMessage for seth with measurable persona fidelity, per-contact memory integration, and proactive messaging — all enabled by default in `human onboard`.
- **Branch**: `sprint-48-imessage-aloop-close`
- **Worktree path**: `/Users/sethford/Projects/human-sprint-48`
- **Base SHA**: `cfb03e34` (Merge: reconcile transport-fast-fail with concurrent A/B/C-loop work)
- **Sprint base to current tech-lead designs**: 9 commits (e75d884e → 9f2c2f54)

## Wave Plan

### Wave 1: Foundation (parallel, no cross-file conflict)
- **US-48-2**: Wire per-contact M2 personal-model slice into iMessage agent turn
- **US-48-4**: Audit + harden silent config-gated subsystems

**Rationale**: US-48-2 is the dependency for US-48-1 eval and US-48-3 draft quality. US-48-4 is mechanical and has zero dependencies. Both can run in parallel without touching the same files.

### Wave 2: Core A-Loop (post-Wave 1, parallel except where noted)
- **US-48-1**: Validate A-loop autoresponder against seth's chat history
- **US-48-3**: Wire follow-up watcher + daemon flush into iMessage proactive path
- **US-48-5**: Harden onboarding config to enable A-loop + proactive subsystems by default

**Rationale**: US-48-1 depends on US-48-2 (M2 context). US-48-3 depends on US-48-2 (for draft quality) and US-48-4 (config keys, one-shot logs). US-48-5 depends on US-48-3 + US-48-4 (config keys must exist). US-48-1 is parallel-safe with US-48-3 and US-48-5 (no file conflicts beyond daemon.c, which US-48-3 owns).

### Wave 3: Integration smoke test (after all Wave 2 stories pass /verify)
- **US-48-6**: Smoke test — daemon sends first proactive message within 60s of human onboard

**Rationale**: US-48-6 depends on US-48-3 (follow-up watcher symbols), US-48-5 (config structure), and US-48-2 (per-contact M2 context). It's an integration test; ships last.

## Per-Story Dispatch Briefs

### US-48-1: Validate A-loop autoresponder against seth's chat history

**Agent**: `general-purpose` with `isolation: worktree`

**Branch**: `sprint-48-imessage-aloop-close-impl-US48-1`

**Files to create**:
- `src/persona/eval_rubric.c` — tone/length/formality scoring predicates (~150 LOC)
- `include/human/persona/eval_rubric.h` — public headers + test seams (~40 LOC)
- `tests/test_autoresponder_eval.c` — main eval harness, fixture loading, blinding, JSON output (~300 LOC)
- `tests/fixtures/seth_chats_50.db` — SQLite fixture with 50 synthetic iMessage conversations

**Files to modify**:
- `CMakeLists.txt` — register `tests/test_autoresponder_eval.c` as test suite (+5 LOC)
- `tests/test_main.c` — add `run_autoresponder_eval_tests()` call (+5 LOC)

**Files this implementer MUST NOT touch**:
- `src/memory/personal_model.c` (owned by US-48-2)
- `src/channels/imessage.c` (owned by US-48-3)
- `src/daemon.c` (owned by US-48-3)
- `src/config.c` (owned by US-48-4, US-48-5)
- `src/cli/onboard.c` (owned by US-48-5)

**Test commands**:
```bash
cmake --preset dev && cmake --build --preset dev
./build/human_tests --filter=autoresponder_eval
scripts/what-to-test.sh src/persona/eval_rubric.c
./build/human_tests  # full suite before commit
```

**Closing command**:
```bash
git log sprint-48-imessage-aloop-close..sprint-48-imessage-aloop-close-impl-US48-1 --oneline
```

**Commit message format** (per CLAUDE.md):
```
feat(persona,eval): A-loop blind eval framework — tone/length/formality rubric

Add three pure scoring predicates and integration test harness to compare
autoresponder replies (with persona) vs baseline (raw model) on 50 iMessage
conversations. Blinding via deterministic hash; output JSON per-contact.

- AC-1.1: Fixture DB loads 50 conversations
- AC-1.2: Generate persona/baseline reply pairs
- AC-1.3: Score via rubric (tone/length/formality)
- AC-1.4: p50 win-rate ≥60%
- AC-1.5: JSON output with per-contact breakdown

All 10,600+ tests pass; no ASan errors.
```

---

### US-48-2: Wire per-contact M2 personal-model slice into iMessage agent turn

**Agent**: `general-purpose` with `isolation: worktree`

**Branch**: `sprint-48-imessage-aloop-close-impl-US48-2`

**Files to create**:
- `tests/test_personal_model_per_contact.c` — per-contact load/ingest/decay unit tests (~200 LOC)

**Files to modify**:
- `include/human/memory/personal_model.h` — add per-contact function signatures (+30 LOC)
- `src/memory/personal_model.c` — implement per-contact load/ingest; migrate .bin → .db; wire contact_handle through fact storage (~150 LOC)
- `src/memory/fact_extract.c` — extend fact extraction to tag facts with contact_handle (+50 LOC)
- `src/agent/autoresponder.c` — load per-contact model before prompt assembly; inject "Contact insights" section (+40 LOC)
- `src/channels/imessage.c` — call per-contact ingest at message-receive point (+20 LOC)
- `tests/test_fact_extract.c` — extend with contact_handle propagation test (+30 LOC)

**Files this implementer MUST NOT touch**:
- `src/daemon.c` (owned by US-48-3)
- `src/config.c` (owned by US-48-4, US-48-5)
- `src/cli/onboard.c` (owned by US-48-5)

**Test commands**:
```bash
cmake --preset dev && cmake --build --preset dev
./build/human_tests --filter=personal_model_per_contact
./build/human_tests --filter=fact_extract
scripts/what-to-test.sh src/memory/personal_model.c
./build/human_tests  # full suite before commit
```

**Closing command**:
```bash
git log sprint-48-imessage-aloop-close..sprint-48-imessage-aloop-close-impl-US48-2 --oneline
```

**Commit message format**:
```
feat(memory): per-contact M2 slice — contact-scoped facts with half-life decay

Migrate personal_model from binary blob to SQLite; add contact_handle column
to facts table. Load per-contact context before autoresponder prompt assembly.
Fact extraction stamps contact_handle on ingest; half-life decay applies live
at prompt-build time per hu_heuristic_fact_effective_confidence().

- AC-2.1: hu_agent_turn_imessage loads per-contact M2 slice
- AC-2.2: hu_fact_extract fires on ingest, tags contact_handle
- AC-2.3: .bin → .db migration; contact_handle indexed
- AC-2.4: "Contact insights: [top 3 facts]" injected into prompt
- AC-2.5: half-life decay @ 30 days ≈ 53% confidence

All 10,600+ tests pass; no ASan errors.
```

---

### US-48-3: Wire follow-up watcher + daemon flush into iMessage proactive path

**Agent**: `general-purpose` with `isolation: worktree`

**Branch**: `sprint-48-imessage-aloop-close-impl-US48-3`

**Files to create**:
- `tests/test_follow_up_daemon_integration.c` — integration test for unresponded detection, scheduling, chronotype-aware flush (~200 LOC)

**Files to modify**:
- `src/daemon.c` — add `hu_daemon_tick_follow_up_watcher()` tick fn; register in tick vector; add one-shot config gate logs (+60 LOC)
- `src/channels/imessage.c` — add `hu_follow_up_watcher_detect_unresponded(contact_handle, chat_db)` query fn (~80 LOC)
- `src/follow_up.c` — add pure predicate `hu_follow_up_should_send_now(contact, now_ms, last_msg_ts, chronotype, throttle_state)` (~40 LOC)
- `src/agent/proactive_throttle.c` — extend with per-contact daily send log; add `hu_proactive_throttle_did_send_today(contact_handle)` query (~50 LOC)
- `src/daemon_proactive.c` — add `hu_daemon_follow_up_flush_for_contact(contact, time_window)` to invoke autoresponder + iMessage send (~70 LOC)
- `include/human/follow_up.h` — add predicate + per-contact throttle query prototypes (+25 LOC)
- `include/human/daemon_proactive.h` — add follow-up-specific flush fn prototype (+10 LOC)
- `include/human/config.h` (or schema) — add `follow_up_watcher.enabled`, `follow_up_watcher.interval_seconds`, `proactive_throttle.per_contact_daily_max` config keys (+15 LOC)
- `tests/test_proactive_throttle.c` — extend with per-contact daily cap tests (+80 LOC)

**Files this implementer MUST NOT touch**:
- `src/memory/personal_model.c` (owned by US-48-2)
- `src/cli/onboard.c` (owned by US-48-5)
- `tests/test_autoresponder_eval.c` (owned by US-48-1)

**Test commands**:
```bash
cmake --preset dev && cmake --build --preset dev
./build/human_tests --filter=follow_up_daemon
./build/human_tests --filter=proactive_throttle
scripts/what-to-test.sh src/daemon.c src/follow_up.c src/agent/proactive_throttle.c
./build/human_tests  # full suite before commit
```

**Closing command**:
```bash
git log sprint-48-imessage-aloop-close..sprint-48-imessage-aloop-close-impl-US48-3 --oneline
```

**Commit message format**:
```
feat(daemon,proactive): follow-up watcher + iMessage flush — reads-without-reply detection

Add daemon tick that polls iMessage chat.db every 5 min for unresponded reads;
compute follow-up delay via hu_followup_compute_send_time(); schedule in
follow_up_scheduled table; flush when contact enters active hours.
Proactive throttle gates to max 1 per contact per 24h.

- AC-3.1: hu_daemon_tick_follow_up_watcher polls every 5 min (configurable)
- AC-3.2: hu_followup_compute_send_time + table storage
- AC-3.3: hu_conversation_flush_scheduled_for triggers iMessage send
- AC-3.4: proactive_throttle.per_contact_daily_max = 1 (default)
- AC-3.5: chronotype active hours honored (no 2 AM to LARK contacts)

All 10,600+ tests pass; no ASan errors.
```

---

### US-48-4: Audit + harden silent config-gated subsystems

**Agent**: `general-purpose` with `isolation: worktree`

**Branch**: `sprint-48-imessage-aloop-close-impl-US48-4`

**Files to create**:
- `tests/test_config_gated_subsystems.c` — centralized test suite for all config-gated subsystems with log-capture fixture (~50-80 LOC)

**Files to modify**:
- `src/daemon_reaction_poll.c` — verify one-shot logs cover both enabled+disabled paths (may need minor refinements) (±0-10 LOC)
- `tests/test_daemon_reaction_poll_wiring.c` — add test verifying exactly-1-line-per-first-invocation for disabled+enabled (+30 LOC)
- `src/config_validate.c` — audit: unknown-key banner already wired (0 LOC change, audit only)

**Files this implementer MUST NOT touch**:
- `src/memory/personal_model.c` (owned by US-48-2)
- `src/channels/imessage.c` (owned by US-48-3)
- `src/daemon.c` tick registration (owned by US-48-3)
- `src/cli/onboard.c` (owned by US-48-5)

**Test commands**:
```bash
cmake --preset dev && cmake --build --preset dev
./build/human_tests --filter=config_gated
./build/human_tests --filter=daemon_reaction_poll_wiring
scripts/what-to-test.sh src/daemon_reaction_poll.c src/config_validate.c
./build/human_tests  # full suite before commit
```

**Closing command**:
```bash
git log sprint-48-imessage-aloop-close..sprint-48-imessage-aloop-close-impl-US48-4 --oneline
```

**Commit message format**:
```
feat(config,daemon): one-shot logging for config-gated subsystems

Audit all config-gated subsystems (reaction_collection, follow_up_watcher,
proactive_throttle) per .claude/rules/silent-config-gated-subsystems.md.
Add hu_log_info_once() guards so disabled/enabled paths are visible in logs.
Config parser already emits unknown-key banner; audit confirms.

- AC-4.1: grep audit finds all !cfg->X.enabled patterns
- AC-4.2: disabled path emits one-shot "X subsystem disabled by config"
- AC-4.3: enabled path emits one-shot "X subsystem activated by config"
- AC-4.4: unknown-key banner at daemon startup (already implemented)
- AC-4.5: tests verify exactly-1-line-per-first-invocation

All 10,600+ tests pass; no ASan errors.
```

---

### US-48-5: Harden onboarding config to enable A-loop + proactive subsystems by default

**Agent**: `general-purpose` with `isolation: worktree`

**Branch**: `sprint-48-imessage-aloop-close-impl-US48-5`

**Files to create**:
- `tests/test_onboard_aloop.c` — wizard interaction fixture + config verification (~150 LOC)

**Files to modify**:
- `src/onboard.c` — add `hu_imessage_detect_self_handle()` helper call; extend wizard with A-loop subsystem prompts (+120 LOC)
- `src/channels/imessage.c` — export `hu_imessage_detect_self_handle()` query function (reads chat.db for "Me" record) (+50 LOC)
- `include/human/onboard.h` — declare new helper signature; document expected config output fields (+30 LOC)
- `include/human/channels/imessage.h` — export the handle detection function (+10 LOC)

**Files this implementer MUST NOT touch**:
- `src/daemon.c` (owned by US-48-3)
- `src/memory/personal_model.c` (owned by US-48-2)
- `tests/test_autoresponder_eval.c` (owned by US-48-1)
- `tests/test_follow_up_daemon_integration.c` (owned by US-48-3)

**Test commands**:
```bash
cmake --preset dev && cmake --build --preset dev
./build/human_tests --filter=onboard_aloop
./build/human_tests --filter=onboard
scripts/what-to-test.sh src/onboard.c src/channels/imessage.c
./build/human_tests  # full suite before commit
```

**Closing command**:
```bash
git log sprint-48-imessage-aloop-close..sprint-48-imessage-aloop-close-impl-US48-5 --oneline
```

**Commit message format**:
```
feat(onboard): wizard prompts for A-loop + proactive subsystems, sensible defaults

Extend onboard wizard to detect seth's iMessage handle from chat.db; prompt
for autoresponder, follow-up watcher, proactive throttle enablement (default yes).
Write config.json with all required fields enabled by default. Auto-detect
self-handle; prompt for additional allowlist contacts.

- AC-5.1: "Enable autoresponder?" prompt (Y/n, default yes)
- AC-5.2: Auto-detect self-handle; prompt for additional contacts
- AC-5.3: Config written with autoresponder/follow_up_watcher/proactive_throttle enabled
- AC-5.4: DND window default 22:00–08:00 local time
- AC-5.5: Config.json parses and validates per schema

All 10,600+ tests pass; no ASan errors.
```

---

### US-48-6: Smoke test — daemon sends first proactive message within 60s of human onboard

**Agent**: `general-purpose` with `isolation: worktree`

**Branch**: `sprint-48-imessage-aloop-close-impl-US48-6`

**Files to create**:
- `src/core/time.c` — time abstraction with test override; production uses `clock_gettime()`, test overrides via static var (~30 LOC)
- `include/human/core/time.h` — public decls for time override (test-only guards) (+8 LOC)
- `src/daemon_test_harness.c` — expose `hu_daemon_init_for_test()`, `hu_daemon_tick_once()`, `hu_daemon_cleanup()` (~100 LOC)
- `include/human/daemon_test_harness.h` — public header for harness functions (+15 LOC)
- `tests/test_daemon_aloop_smoke.c` — main test: config load → daemon init → 60 virtual-time ticks → verify log + stub output (~280 LOC)
- `tests/fixtures/aloop_config.json` — test config with follow_up_watcher.enabled=true, interval_seconds=1 (~30 LOC)
- `tests/fixtures/chatdb/aloop_unresponded.db` — SQLite fixture with one unresponded iMessage

**Files to modify**:
- `src/channels/imessage.c` — add `#if HU_IS_TEST` gate before osascript spawn; if test stub set, write to log file instead (+20 LOC)
- `include/human/channels/imessage.h` — add `hu_imessage_set_test_send_stub(fn_ptr)` prototype (test-only) (+5 LOC)
- `CMakeLists.txt` — register `tests/test_daemon_aloop_smoke.c` as test suite (+5 LOC)
- `tests/test_main.c` — add `run_daemon_aloop_smoke_tests()` call (+5 LOC)

**Files this implementer MUST NOT touch**:
- `src/daemon.c` tick registration (owned by US-48-3; implementer reads symbols)
- `src/follow_up.c` (owned by US-48-3; implementer reads symbols)
- `src/agent/proactive_throttle.c` (owned by US-48-3; implementer reads symbols)
- `src/cli/onboard.c` (owned by US-48-5; implementer reads symbols)
- `src/memory/personal_model.c` (owned by US-48-2; implementer reads symbols)
- Any Wave 2 test files (implementer reads test fixtures)

**Test commands**:
```bash
cmake --preset dev && cmake --build --preset dev
./build/human_tests --filter=daemon_aloop_smoke
./build/human_tests --filter=core_time
scripts/what-to-test.sh src/core/time.c src/daemon_test_harness.c
./build/human_tests  # full suite before commit
```

**Closing command**:
```bash
git log sprint-48-imessage-aloop-close..sprint-48-imessage-aloop-close-impl-US48-6 --oneline
```

**Commit message format**:
```
feat(daemon,test): smoke test — daemon sends proactive within 60s of onboard

Add test harness to run daemon in-process with virtual time override. Verifies
end-to-end A-loop pipeline: onboard completes, daemon loads config, detects
unresponded iMessage, schedules follow-up, flushes at contact's active hours,
sends via iMessage stub—all deterministically within 60 virtual seconds.

- AC-6.1: onboard spawns/respawns daemon
- AC-6.2: daemon loads config, enables subsystems
- AC-6.3: daemon finds unresponded message in chat.db fixture
- AC-6.4: within 60s (virtual), daemon sends draft
- AC-6.5: log trace confirms "scheduled follow-up" + "flush_scheduled_for"

All 10,600+ tests pass; no ASan errors.
```

---

## Cross-Story API Contracts (Resolve US-48-5 ACTION Items)

US-48-5 design flagged two coordination points. Resolved based on tech-lead designs:

### 1. Config key names — follow_up_watcher

**Canonical name**: `follow_up_watcher.enabled` (top-level)

**Supporting keys**:
- `follow_up_watcher.interval_seconds` (default: 300, configurable)

**Schema location**: `include/human/config.h` (merged from US-48-3 design)

**Implementer responsibility**: US-48-3 wires the daemon tick; US-48-5 writes config. Both must use same key name. **Verify before implementation**: grep US-48-3 design for exact key path in code.

### 2. Config key names — proactive_throttle

**Canonical name**: `proactive_throttle.enabled` (top-level) + `proactive_throttle.per_contact_daily_max` (int, default: 1)

**Schema location**: `include/human/config.h` (merged from US-48-3 design)

**Implementer responsibility**: US-48-3 wires throttle logic; US-48-4 adds one-shot logging for the subsystem; US-48-5 writes config. All three must use same key name.

### 3. Config key names — autoresponder

**Canonical name**: `autoresponder.enabled` (top-level), `autoresponder.allowlist` (string array), `autoresponder.dnd_schedule` (array of DND window objects)

**Schema location**: `include/human/config.h` (US-48-5 design)

**Implementer responsibility**: US-48-5 owns this family of keys. US-48-1 reads them (implicit, via autoresponder context). No cross-story coordination needed.

---

## Quality Gate Sequence (Per Story Closure)

Each story closes when ALL gates pass in order:

1. **Implementer commits to impl branch** via:
   ```
   git add <changed-files>
   git commit -m "feat(scope): message"
   ```

2. **Implementer reports DONE** with evidence:
   ```
   git log sprint-48-imessage-aloop-close..<impl-branch> --oneline
   [shows N commits landing the AC]
   ```

3. **Scrum Master verifies commit landed** on the impl branch:
   ```
   git log <impl-branch> --oneline | grep -q "feat(scope)"
   ```
   **Rejects working-tree-only DONE reports.**

4. **`/verify` agent runs**:
   ```
   Spawned with US-N AC verbatim; returns RESULT_verifier=PASS or FAIL/INCONCLUSIVE
   ```
   **FAIL or INCONCLUSIVE: story stays in flight. Do NOT advance.**

5. **Critic agent reviews immediately** (not batched at sprint end):
   ```
   Reads implementer's commits on the impl branch; audits for half-fixes,
   missing edge cases, cross-story regressions. Returns CLEAN or findings.
   HIGH/CRITICAL findings: re-open story. Do NOT advance.
   ```

6. **`/aspect-panel` runs** (safety audit):
   ```
   Returns PASS or CLEAN (no ESCALATE).
   ESCALATE: story stays in flight.
   ```

7. **Scrum Master marks task DONE** in the plan (locally tracked)

8. **Scrum Master merges impl branch into sprint-48-imessage-aloop-close**:
   ```
   git merge <impl-branch> --no-ff -m "Merge US-N: <title>"
   ```

---

## Risk Surface

### Per-Story Risks

| Story | Risk | Probability | Impact | Owner | Mitigation |
|---|---|---|---|---|---|
| **US-48-1** | Rubric subjectivity; eval near-parity | Medium | Medium | Implementer | Deterministic scoring functions; adversarial test suite validates tone/length/formality don't invert on edge cases. |
| **US-48-1** | Per-contact M2 not ready (US-48-2 delayed) | Medium | Medium | Scrum Master | Eval harness handles NULL persona_summary gracefully; persona-aware arm degrades to baseline. Still validates framework. |
| **US-48-2** | .bin → .db migration breaks backwards compat | Low | Small | Implementer | Migration runs once per process startup; existing tests continue with contact_handle="" (global scope). |
| **US-48-2** | Half-life decay applied at wrong time (write vs prompt-build) | Medium | Small | Implementer | Test fixture `test_half_life_decay_applies_to_contact_facts` pins behavior; decay applies at prompt-build time, not on ingest. |
| **US-48-3** | chat.db schema mismatch (is_read semantics) | High | Large | Implementer + Tech Lead | **Blocker**: Must verify chat.db schema with real DB dump BEFORE implementation. Confirm query shape: `is_from_me=1 AND read_receipts.read_status=1`, not `is_read`. Test fixture must match. |
| **US-48-3** | Chronotype default for contacts without overlay | Medium | Medium | Implementer | Default to persona's global chronotype, fall back to `HU_CHRONO_INTERMEDIATE` (9am–11pm). AC-3.5 test pins this. |
| **US-48-3** | Throttle persistence across daemon restart | Medium | Medium | Implementer | Persist `proactive_send_log` table to SQLite on every send; reload on daemon startup. Test: `test_proactive_throttle_persists_across_restart`. |
| **US-48-3** | Predicate extraction discipline skipped | Low (HIGH if skipped) | High | Implementer | Extract `hu_follow_up_should_send_now()` BEFORE wiring into daemon tick. Truth table is: `(contact_handle, now_ms, last_msg_ts, chronotype, throttle_state) → bool`. Do not proceed past step 2 of implementation without this isolated. |
| **US-48-4** | Missed subsystem in grep audit | Medium | Small | Implementer | Also grep `enabled\s*=\s*false` and review all `daemon_tick_*` functions, not just the pattern. |
| **US-48-4** | Log spam if guard is wrong | Low | Small | Implementer | Atomic CAS is thread-safe; per-subsystem tests verify exactly-1 line per first invocation. |
| **US-48-5** | Chat.db missing on dev machines | High | Medium | Implementer | Fallback to manual prompt; document in CLI output. Test mocks `hu_imessage_detect_self_handle()`. |
| **US-48-5** | Over-prompting (UX friction) | Medium | Small | Implementer | Keep to 4–5 new prompts; all optional. Defaults are sensible. |
| **US-48-5** | Existing config corrupted on merge | Low | Large | Implementer | Use atomic temp-file pattern; validate JSON parse before commit; revert on error. Test merge path. |
| **US-48-5** | Cross-story config key mismatch | Medium | Large | All implementers | Coordinate before implementation: confirm field names with US-48-3 + US-48-4 tech-leads. Flag any name drift. |
| **US-48-6** | Time call audit incomplete | High | Large | Implementer | Exhaustive grep `src/daemon.c src/follow_up.c src/agent/proactive_throttle.c` for `time()`, `gettimeofday()`, `clock_gettime()`. Replace all with `hu_time_get_current_ms()`. |
| **US-48-6** | iMessage send stub not covering all paths | Medium | Medium | Implementer | Grep `src/channels/imessage.c` for exact spawn point (execve or system()); add stub gate there only, not upstream. |
| **US-48-6** | Chat.db schema version mismatch | Medium | Medium | Implementer | Use same schema version as US-48-3 targets. Fixture must match production schema. |

### Sprint-Level Risks (Top 3)

1. **US-48-3 chat.db schema blocker** (HIGH/LARGE)
   - If `is_read` semantics are misunderstood, entire follow-up watcher fails silently.
   - **Mitigation**: Tech lead confirms schema BEFORE US-48-3 implementation starts. Red-team against real chat.db dump.

2. **Time call audit incomplete in US-48-6** (HIGH/LARGE)
   - If daemon.c calls system time directly, test time override won't apply; test flakes or hangs on timing-dependent code.
   - **Mitigation**: Scrum Master verifies exhaustive grep before US-48-6 dispatch.

3. **Cross-story config key drift between US-48-3, US-48-4, US-48-5** (MEDIUM/LARGE)
   - If implementers use different key names (e.g. `follow_up_watcher.enabled` vs `daemon.follow_up_watcher.enabled`), config won't parse.
   - **Mitigation**: Scrum Master explicitly coordinates key names before US-48-3/US-48-5 dispatch. Write canonical names in this plan (done).

---

## Definition of Done (Sprint-Level)

From stories.md, section "Definition of Done":

- [x] All acceptance criteria met and verified by `/verify` agent
- [x] All new tests passing (0 failures, 0 ASan errors)
- [x] Full test suite passes (`./build/human_tests`)
- [x] Config schema updated with new keys (US-48-3, US-48-4, US-48-5)
- [x] Per-contact M2 integration tested (US-48-2)
- [x] Follow-up watcher daemon tick wired and tested (US-48-3)
- [x] One-shot logging for config-gated subsystems (US-48-4)
- [x] Onboarding wizard extended with A-loop prompts (US-48-5)
- [x] Smoke test passing on in-process daemon harness (US-48-6)
- [x] Critic agent has reviewed all closures (per-story, not batched)
- [x] No HIGH/CRITICAL critic findings outstanding
- [x] Retro held and lessons captured
- [x] Sprint auditor ran adversarial audit (Phase 5)

---

## Hard Rules (Enforced by Scrum Master)

From `/scrum` skill protocol:

1. **No story closes without verifier PASS + critic CLEAN (or no findings)**
   - Working-tree-only DONE reports are rejected (no commit, no merge)
   - Critic runs immediately after each story closes, not batched at sprint end
   
2. **Audit is mandatory (Phase 4)**
   - Sprint auditor invoked before retro
   - Audit FAIL re-opens stories; no sprint closes until audit passes

3. **Audit findings trigger re-opens**
   - HIGH/CRITICAL critic findings → story stays in flight
   - Implementer must fix and report DONE again (new commit, new /verify run)

4. **No scope creep without explicit PO sign-off**
   - Changes to AC, file scope, or dependencies require stakeholder decision recorded in plan
   - Implementers flag scope deltas immediately; scrum-master escalates

5. **Retro happens even on perfect sprints**
   - Post-audit, before close
   - Captures process learnings for next sprint

6. **Sprint runs on a dedicated branch from planning**
   - All implementer work merges into `sprint-48-imessage-aloop-close` only
   - No work on main or other concurrent branches without explicit worktree isolation

7. **Implementer commits before handoff**
   - "DONE" means committed to impl branch, not just working-tree changes
   - Scrum Master verifies with `git log` before accepting DONE

8. **Critic runs immediately after each story closes**
   - Not batched at sprint end
   - Findings become tasks, re-opening the story if needed

---

## Sequencing Notes

### Wave 1 (Parallel)
- US-48-2 and US-48-4 can run in parallel (no file conflicts)
- Wave 1 must complete before Wave 2 starts (US-48-2 is dependency for US-48-1, US-48-3)

### Wave 2 (Mostly Parallel, One Soft Dependency)
- **Hard dependency**: US-48-2 → US-48-1 (eval needs M2 context; AC-1.4 assumes M2 is wired)
- **Hard dependency**: US-48-2 → US-48-3 (follow-up draft quality; AC-3.3 assumes M2 context available)
- **Hard dependency**: US-48-4 + US-48-3 → US-48-5 (config keys must exist before wizard can write them)
- **Soft dependency**: US-48-1 is parallel-safe with US-48-3, US-48-5 (no shared files)

### Wave 3 (After Wave 2 Passes /verify)
- US-48-6 depends on all Wave 2 stories passing `/verify`
- Requires symbols from US-48-3 (daemon tick, follow-up watcher), US-48-5 (config structure), US-48-2 (per-contact M2 load)

---

## Implementation Notes for Scrum Master Dispatch

When dispatching each implementer:

1. **Include this plan.md in the dispatch prompt** — implementer reads it for files/branches/test commands
2. **Include AC verbatim from stories.md** — implementer implements to AC, not to design interpretation
3. **Include cross-story coordination section** — US-48-3/4/5 implementers confirm key names before starting
4. **Include closing command verbatim** — implementer runs it before reporting DONE
5. **Remind of quality gates** — /verify is mandatory; critic runs immediately after closure; HIGH findings re-open stories

---

RESULT_scrum-master=PLAN_READY
