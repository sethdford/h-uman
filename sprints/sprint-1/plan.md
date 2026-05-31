# Sprint 1 Plan — Activate and MEASURE h-uman's humanness moat

**Sprint:** Sprint 1  
**Status:** PLAN_READY  
**Date created:** 2026-05-30  
**Scrum Master:** Claude Code (Haiku 4.5)

---

## Branch and Working Directory

- **Sprint branch:** `sprint-1-sota-activation`
- **Working directory:** `/Users/sethford/Projects/human-sprint-1-sota` (dedicated worktree isolation if concurrent activity detected)
- **Base commit:** `28966648`
- **Isolation level:** Worktree (due to shared-file collision risk on `src/agent/agent_turn.c`)

---

## Sprint Overview

**Goal:** Activate and measure h-uman's already-built humanness subsystems (GraphRAG memory grounding, contextual bandit, salience/arbitration) toward SOTA and better-than-human performance. Gated on blind A/B human measurement.

**Five user stories:**
- **US-1 (P0):** GraphRAG memory-grounding activation and measurement (C, SHADOW mode, agent_turn.c edit)
- **US-2 (P0):** Contextual bandit wiring audit and activation (C, possible agent_turn.c edit if wiring needed)
- **US-3 (P1):** Promote salience/arbitration from SHADOW toward LIVE (C, agent_turn.c edit ~lines 2655–2853)
- **US-4 (P0):** Wire and run blind A/B rating harness (Python, no C changes, HUMAN-IN-THE-LOOP blocker on final results)
- **US-5 (P2):** Adopt TwinVoice six-axis eval (Python, depends on US-4)

---

## Critical Constraint: Shared-File Collision

**MANDATORY STRICT SEQUENCE:** US-1 → US-3 → US-2

All three C stories touch `src/agent/agent_turn.c`:
- **US-1** adds a gate comment at line 1471 (GraphRAG activation gated on Story D)
- **US-3** modifies lines 2655–2853 (salience trichotomy logic)
- **US-2** may add wiring calls around line 1500 (contextual bandit integration) IF the audit confirms the module is dead

**Merge strategy:** Execute in strict sequence. Each story merges to the sprint branch before the next story is dispatched. This prevents parallel edits to the same 20-line region and ensures git merge can resolve unambiguously.

---

## Wave Sequencing & Parallelization

### Wave 1: Python harness wiring and measurement (FAST GATE)
**Stories:** US-4, US-5 (parallel, no C dependencies)  
**Isolation:** Main working tree (Python-only, no C collision)  
**Rationale:** 
- US-4 and US-5 are pure Python. No agent_turn.c collision.
- US-4 is the CRITICAL PATH gate: Stories US-1 and US-3 (prod activation) are blocked on US-4's blind A/B result.
- US-4 completes in ~30 min (synthetic harness wiring); the human-in-the-loop part (rater collection) is user-blocked and does NOT block the C story execution.
- US-5 depends on US-4's harness structure but can wire the six-axis extension in parallel with US-4's testing.

**Acceptance criteria (Wave 1 DONE):**
- US-4: Harness wired end-to-end (gen → sheet → score pipeline verified with synthetic data). `RATING-BLOCKED.md` documents user responsibilities. No C tests run (pure Python).
- US-5: Six-axis framework documented. `score.py` extended to emit per-axis scores. `make_rating_sheet.py` updated with six Likert columns. Backward-compatible JSON output.

**Exit criteria (before Wave 2 dispatch):**
- `python3 -m py_compile scripts/blind_ab/*.py` exits 0 (syntax check)
- Synthetic test run (5 triples → rating sheet → score.py) produces valid JSON output
- Both stories: `/verify` PASS (no C code, so verifier validates Python correctness + schema adherence)

**Estimated duration:** 45 min (30 min US-4, 20 min US-5, 5 min overlap)

---

### Wave 2: C story sequence — US-1 (GraphRAG) (FIRST OF THREE)
**Story:** US-1  
**Isolation:** Dedicated worktree OR main working tree (both clean; C edits only)  
**Rationale:**
- US-1 is the fastest C story (measurement + gate comment, no complex wiring).
- It adds a comment at line 1471 only, which is far from US-3's code (lines 2655–2853).
- Establishes the base state for US-3 to merge on top of.

**Acceptance criteria (US-1 DONE):**
- AC-1.1: Autodream writes community_summaries on a daemon tick. Test `test_autodream_tick_populates_community_summaries_for_contact` PASS.
- AC-1.2: SHADOW-mode harness runs ≥10 turns, captures `graph_context_bytes` and summary count. Metrics JSON at `sprints/sprint-1/evidence/US-1/shadow-run-metrics.json` valid (all three fields > 0).
- AC-1.3: Gate comment at agent_turn.c:1471 stating "GraphRAG activation gated on Story D blind A/B; do not flip to ON without measurement."
- AC-1.4: Full test suite passes. `./build/human_tests --suite=graph` → 0 failures, 0 ASan errors.

**Exit criteria (before Wave 3 dispatch):**
- `git log sprint-1-sota-activation ^28966648 --oneline` shows US-1 commit
- `grep -n "GraphRAG activation gated" src/agent/agent_turn.c | head -1` returns line 1471 (or nearby)
- `/verify` PASS (agent verifies test run, ASan clean, metrics JSON valid)
- Critic review CLEAN (architecture, naming, test coverage)

**Estimated duration:** 2–3 hours (includes cmake build + 13,231 test suite runs; may span multiple sessions if build is slow)

---

### Wave 3: C story sequence — US-3 (Salience/Arbitration) (SECOND OF THREE)
**Story:** US-3  
**Isolation:** Same worktree as Wave 2  
**Rationale:**
- US-3 is the next C story after US-1 merges.
- Edits lines 2655–2853 (salience trichotomy), isolated from US-1's line 1471 comment.
- Depends on US-1 being merged so US-3's edits can land on top of a clean base.

**Acceptance criteria (US-3 DONE):**
- AC-3.1: Current state verified: salience.c wired, HU_SALIENCE_SHADOW env gate in agent_turn.c, test coverage in test_salience.c.
- AC-3.2: New HU_SALIENCE_LIVE flag added. Three states (OFF/SHADOW/LIVE) mutually exclusive, default OFF. Tests verify trichotomy behavior.
- AC-3.3: Calibration harness `scripts/salience-calibration.sh` runs daemon for ≥10 turns in LIVE mode, outputs `sprints/sprint-1/evidence/US-3/calibration-metrics.json` with keys `contact_id`, `suppressed_count`, `kept_count`. Valid JSON, all fields populated.
- AC-3.4: Behavior unchanged when LIVE=OFF. Full suite run three times (OFF, default, SHADOW) produces identical test counts.
- AC-3.5: Gate comment at agent_turn.c ~line 2655 stating "Salience LIVE activation gated on Story D blind A/B; do not flip to default-ON without measurement."
- AC-3.6: Full test suite passes. `./build/human_tests 2>&1 | tail -3` → 0 failures, 0 ASan errors.

**Exit criteria (before Wave 4 dispatch):**
- `git log sprint-1-sota-activation ^28966648 --oneline` shows both US-1 and US-3 commits
- `git diff sprint-1-sota-activation..28966648 -- src/agent/agent_turn.c` shows no conflicts (clean merge)
- Calibration metrics JSON valid and non-empty
- `/verify` PASS (test run passes, ASan clean, metric validation)
- Critic review CLEAN (buffer reconstruction logic, never-suppress floor, observability)

**Estimated duration:** 2–4 hours (complex buffer manipulation, requires careful testing of trichotomy + filter logic)

---

### Wave 4: C story sequence — US-2 (Contextual Bandit Audit) (THIRD OF THREE)
**Story:** US-2  
**Isolation:** Same worktree as Waves 2 & 3  
**Rationale:**
- US-2 is the final C story, executing after US-1 and US-3 have merged.
- Audit first: grep/LSP verify whether contextual_bandit.c is dead or wired (AC-2.1).
- If dead: wire to humanization-param selection at persona-builder seam (likely ~line 1500, after US-3's salience code, no collision).
- If wired: document the wiring in AC-2.2 (read-only, no implementation risk).

**Acceptance criteria (US-2 DONE):**
- AC-2.1: LSP `findReferences` on contextual_bandit functions confirms 0 incoming calls from agent_turn.c send path (dead code confirmed) OR findings documented.
- AC-2.2 (if wired): Wiring report created at `sprints/sprint-1/evidence/US-2/wiring-report.md` with call site line numbers, flow to send decision, channel/contact path coverage.
- AC-2.3 (if dead): Contextual bandit wired to per-contact humanization-param selection. Thompson-sample-based decision (aggressive/moderate/conservative based on theta). Three contract tests (high theta → aggressive, low theta → conservative, new contact → neutral). Tests PASS, ASan-clean.
- AC-2.4: No new factory includes, no channel-name memcmp checks added. `scripts/check-agent-core-boundary.sh` exits 0.
- AC-2.5: Full test suite passes. `./build/human_tests 2>&1 | tail -3` → 0 failures, 0 ASan errors.

**Exit criteria (US-2 DONE):**
- `git log sprint-1-sota-activation ^28966648 --oneline` shows all three US-1, US-3, US-2 commits
- If AC-2.3 wired: agent_turn.c at ~line 1500 calls `hu_humanization_decide_contact_params()` with bandit handle + contact_handle (safety gate: `if (HU_IS_BANDIT_ENABLED && bandit)`).
- Agent-core-boundary check passes
- `/verify` PASS (test run, ASan clean, boundary validation)
- Critic review CLEAN (humanization override precedence, bandit initialization, modularity)

**Estimated duration:** 1.5–3 hours (audit up to 30 min if dead confirmed quickly; if wiring needed, 90–180 min for implementation + tests)

---

## Pre-Flight Checks Per Wave

### Wave 1 (US-4, US-5 — Python)
- [ ] User story AC inline in agent prompts
- [ ] Test commands specified (Python syntax check + synthetic end-to-end)
- [ ] Verifier scope: Python correctness, JSON schema validation, backward compatibility
- [ ] Both stories must pass `/verify` before closure

### Wave 2 (US-1 — C)
- [ ] User story AC inline in agent prompt
- [ ] Test commands specified: `touch src/agent/agent_turn.c && cmake --build build --target human_tests -j8 && ./build/human_tests --suite=graph 2>&1 | tail -3`
- [ ] Verifier scope: Full graph test suite PASS, ASan clean, metrics JSON valid
- [ ] Merge test: `grep -n "GraphRAG activation gated" src/agent/agent_turn.c` returns expected line
- [ ] Critic review: architecture (fail-open/fail-safe SHADOW design), naming (graph_context_bytes clarity), test coverage (fixture contact + synthetic message)
- [ ] Must pass `/verify` AND critic before closure

### Wave 3 (US-3 — C)
- [ ] User story AC inline in agent prompt
- [ ] Test commands specified: 
  - Build: `touch src/agent/agent_turn.c && cmake --build build --target human_tests -j8`
  - Test three states: `HU_SALIENCE_LIVE=off ./build/human_tests > /tmp/off.txt 2>&1 && unset HU_SALIENCE_LIVE && ./build/human_tests > /tmp/default.txt 2>&1 && diff /tmp/off.txt /tmp/default.txt`
  - Verify trichotomy: `./build/human_tests --suite=salience 2>&1 | grep "PASS"`
- [ ] Verifier scope: Salience test suite PASS, three-state behavior isolation confirmed (no change when OFF), calibration harness runnable, ASan clean
- [ ] Merge test: `grep -n "HU_SALIENCE_LIVE" src/agent/agent_turn.c` shows trichotomy flag handling; `grep -n "Salience.*LIVE activation gated" src/agent/agent_turn.c` returns gate comment
- [ ] Critic review: buffer reconstruction correctness (invariant checks to prevent malformed prompt), never-suppress floor enforced, observability (startup warning log)
- [ ] Must pass `/verify` AND critic before closure

### Wave 4 (US-2 — C)
- [ ] User story AC inline in agent prompt
- [ ] Test commands specified:
  - Audit: `grep -rn "hu_contextual_bandit_decide_send" src/ include/ | wc -l` (should be near 1 if unwired)
  - Build: `touch src/agent/agent_turn.c && cmake --build build --target human_tests -j8`
  - Test: `./build/human_tests --suite=humanization_bandit 2>&1 | grep "PASS"` (if wiring added)
  - Boundary check: `bash scripts/check-agent-core-boundary.sh && echo "PASS" || echo "FAIL"`
- [ ] Verifier scope: Audit conclusions + wiring (if applicable) verified, humanization bandit test suite PASS, agent-core-boundary ratchet held, ASan clean
- [ ] Merge test: if AC-2.3 wired, `grep -n "hu_humanization_decide_contact_params" src/agent/agent_turn.c` shows exactly one call site with correct safety gate
- [ ] Critic review: bandit initialization (agent struct wiring or injection), Thompson sampling determinism under test, modeled-person-layering compliance (behavior decision, pure predicate, no backward cognition include)
- [ ] Must pass `/verify` AND critic before closure

---

## Wave Dependency Graph

```
Wave 1: US-4, US-5 (parallel, independent)
   ↓ (python success + synthetic harness verified)
Wave 2: US-1 (C, agent_turn.c:1471)
   ↓ (US-1 merged to sprint branch)
Wave 3: US-3 (C, agent_turn.c:2655–2853)
   ↓ (US-3 merged to sprint branch)
Wave 4: US-2 (C, audit then conditionally wire)
   ↓ (all C stories merged)
Sprint close
```

**Rationale for strict sequence on C stories:**
- US-1 → US-3 → US-2 ensures each story merges to sprint-1-sota-activation before the next starts.
- git merge is deterministic if stories don't edit overlapping regions AND edits are sequential.
- By the time US-2 lands (story 3), US-1's line 1471 comment and US-3's lines 2655–2853 are already committed, so US-2's audit results and any humanization_bandit wiring have a clean base to build on.

---

## Per-Wave Exit Criteria (Definition of Done)

### Wave 1 DONE when:
- US-4 and US-5 both closed with `/verify` PASS
- Critic review CLEAN (or LOW/INFO only) for both
- Python syntax check passes: `python3 -m py_compile scripts/blind_ab/*.py`
- Synthetic test run produces valid JSON: `jq . sprints/sprint-1/evidence/US-4/blind-ab-results.json` exits 0
- RATING-BLOCKED.md documents user responsibilities explicitly
- Six-axis documentation complete and clear (AC-5.1)

### Wave 2 DONE when:
- US-1 commit exists on sprint-1-sota-activation branch: `git log sprint-1-sota-activation ^28966648 --oneline | grep -i "US-1\|graphrag"`
- `/verify` agent reports PASS (test suite runs, ASan clean, metrics JSON present)
- Critic review CLEAN (or LOW/INFO only)
- Gate comment verified in-place: `grep -n "GraphRAG activation gated" src/agent/agent_turn.c`
- US-1 branch merged to sprint-1-sota-activation (ready for US-3)

### Wave 3 DONE when:
- US-3 commit exists on sprint-1-sota-activation: `git log sprint-1-sota-activation ^28966648 --oneline | grep -i "US-3\|salience"`
- `/verify` agent reports PASS (trichotomy tests PASS, calibration harness produces JSON, ASan clean, OFF behavior unchanged)
- Critic review CLEAN (or LOW/INFO only)
- Trichotomy logic verified: `grep -n "HU_SALIENCE_LIVE\|sal_mode ==" src/agent/agent_turn.c | wc -l` shows ≥5 lines (enum, branches, log)
- Gate comment verified: `grep -n "Salience.*LIVE activation gated" src/agent/agent_turn.c`
- US-3 branch merged to sprint-1-sota-activation (ready for US-2)

### Wave 4 DONE when:
- US-2 commit exists on sprint-1-sota-activation: `git log sprint-1-sota-activation ^28966648 --oneline | grep -i "US-2\|bandit"`
- `/verify` agent reports PASS:
  - If AC-2.1 audit confirms dead: humanization_bandit test suite PASS, agent_turn.c wiring verified, ASan clean
  - If AC-2.1 confirms wired: wiring-report.md exists, audit findings documented, no C changes needed (tests still run, ASan clean)
- Critic review CLEAN (or LOW/INFO only)
- Agent-core-boundary check passes: `bash scripts/check-agent-core-boundary.sh && echo "PASS"`
- US-2 branch merged to sprint-1-sota-activation

### Sprint CLOSE when:
- All four waves DONE (US-4, US-5, US-1, US-3, US-2 all merged)
- Full test suite passes: `./build/human_tests 2>&1 | tail -3` shows `Results: N/N passed, 0 failed`
- No outstanding HIGH/CRITICAL critic findings
- Artifact evidence directory complete:
  - `sprints/sprint-1/evidence/US-1/shadow-run-metrics.json` ✓
  - `sprints/sprint-1/evidence/US-2/wiring-report.md` OR (if wired) humanization_bandit tests ✓
  - `sprints/sprint-1/evidence/US-3/calibration-metrics.json` ✓
  - `sprints/sprint-1/evidence/US-4/RATING-BLOCKED.md` ✓
  - `sprints/sprint-1/evidence/US-5/six-axes.md` ✓
- Sprint review report written (see Phase 5 below)
- Sprint audit completed (see Phase 6 below)
- Retro findings captured

---

## Implementation Contract for Each Wave

### Wave 1 Dispatch (US-4, US-5)

**Agent type:** general-purpose (Python)  
**Isolation:** main working tree (Python-only, no C collision)  
**Prompt includes:**
- Full US-4 story AC verbatim
- Full US-5 story AC verbatim
- Design documents (US-4.md, US-5.md)
- Explicit instruction: "Commit your work to `sprint-1-sota-activation` via `git add <paths> && git commit -m \"feat(blind-ab): ...\"` BEFORE reporting DONE. Working-tree-only DONE reports will be rejected."
- Test commands: Python syntax check + synthetic harness run
- Verifier requirement: `/verify` must return RESULT_verifier=PASS before story can close

**Implementer output:**
- US-4: gen_huuman_replies.py wiring verified, make_rating_sheet.py produces CSV, score.py outputs JSON, synthetic test run PASS
- US-5: six-axes.md documented, score.py extended with axis aggregation, backward-compatible JSON output
- Both: commits on sprint-1-sota-activation, evidence artifacts present, no C code changes

---

### Wave 2 Dispatch (US-1)

**Agent type:** general-purpose (C, ASan required)  
**Isolation:** Same worktree as Wave 1 (or new worktree if Wave 1 already completed and checked in)  
**Prompt includes:**
- Full US-1 story AC verbatim
- Design document (US-1.md)
- Explicit instruction: "Execute in this branch: sprint-1-sota-activation. Merge order: this is the FIRST of three C stories. Do not parallelize with US-2 or US-3."
- Build/test commands: `touch src/agent/agent_turn.c && cmake --build build && ./build/human_tests --suite=graph 2>&1 | tail -3`
- Verifier requirement: `/verify` must return RESULT_verifier=PASS (test suite PASS, ASan clean)
- Critic requirement: `/aspect-panel` CLEAN (or LOW/INFO) before closure

**Implementer output:**
- Autodream test populated, SHADOW harness script created, metrics JSON valid
- Gate comment at agent_turn.c:1471
- Tests PASS, ASan clean
- Commit on sprint-1-sota-activation
- Evidence directory populated

---

### Wave 3 Dispatch (US-3)

**Agent type:** general-purpose (C, ASan required)  
**Isolation:** Same worktree as Waves 1 & 2  
**Prompt includes:**
- Full US-3 story AC verbatim
- Design document (US-3.md)
- Explicit instruction: "This is the SECOND of three C stories. US-1 has already merged to sprint-1-sota-activation; you are building on top of that commit. Do not parallelize with US-2."
- Build/test commands: `cmake --build build --target human_tests && ./build/human_tests --suite=salience 2>&1 | tail -3`
- State-isolation test: run full suite three times (OFF, default, SHADOW) and diff results
- Verifier requirement: `/verify` PASS (trichotomy tests, state isolation, calibration harness, ASan clean)
- Critic requirement: `/aspect-panel` CLEAN (or LOW/INFO) before closure

**Implementer output:**
- Salience trichotomy (OFF/SHADOW/LIVE) wired to agent_turn.c
- Calibration harness script created, produces metrics JSON
- Three contract tests (off-skips, shadow-logs, live-filters)
- Gate comment at agent_turn.c ~line 2655
- Tests PASS, ASan clean
- Commit on sprint-1-sota-activation
- Evidence directory populated

---

### Wave 4 Dispatch (US-2)

**Agent type:** general-purpose (C, ASan required, audit-first)  
**Isolation:** Same worktree as Waves 1, 2, 3  
**Prompt includes:**
- Full US-2 story AC verbatim
- Design document (US-2.md)
- Explicit instruction: "This is the THIRD and FINAL C story. US-1 and US-3 have merged; you build on that. AC-2.1 is an AUDIT: grep + LSP verification that contextual_bandit is dead or wired. If dead (likely), AC-2.3 wires to humanization-param selection. If wired, AC-2.2 documents the existing wiring (read-only)."
- Audit tools: `grep -rn "hu_contextual_bandit_decide_send" src/ include/`, LSP `findReferences`, agent-core-boundary check
- Build/test commands: `cmake --build build --target human_tests && ./build/human_tests --suite=humanization_bandit 2>&1 | tail -3` (if wiring added)
- Verifier requirement: `/verify` PASS (audit conclusions validated, wiring tests if AC-2.3, ASan clean)
- Critic requirement: `/aspect-panel` CLEAN (or LOW/INFO) before closure

**Implementer output:**
- Audit results documented (AC-2.1 or AC-2.2)
- If dead: humanization_bandit wiring added to agent_turn.c (~line 1500, after US-3's code); three contract tests; gate comment
- If wired: wiring-report.md created with call sites + flow (read-only)
- Agent-core-boundary check passes
- Tests PASS, ASan clean
- Commit on sprint-1-sota-activation
- Evidence directory populated

---

## Standup Cadence

**Frequency:** Per wave completion (not daily, as each wave may span multiple sessions)

**Format:** 3-section report
1. **Done since last standup** — completed stories with verifier PASS + critic status
2. **In flight** — current wave assignee, started N hours ago, blockers if any
3. **Blockers** — sequence holds, missing context, user input needed (e.g., RATING-BLOCKED on US-4)

**Example (after Wave 1 complete, Wave 2 in flight):**
```
## Standup — Sprint 1, Wave 1 complete

### Done
- US-4: blind-ab harness wired end-to-end (verifier PASS, panel CLEAN)
- US-5: six-axis framework + score.py extended (verifier PASS, panel CLEAN)

### In flight
- US-1: general-purpose agent, started 30 min ago, building test suite (no blockers)

### Blockers
- US-2/US-3 queued, waiting for US-1 merge
- US-4 real ratings blocked-on-user (Seth export + rater recruitment): timeline 2–4 weeks
```

---

## Risk Mitigation Checklist

### Shared-file collision (HIGH/MEDIUM)
- ✅ Strict sequence enforcement: US-1 → US-3 → US-2 (no parallel agent_turn.c edits)
- ✅ Merge test per wave: `git diff` confirms no conflicts, comment/flag intact
- ✅ Artifact pinning: tests assert gate comment/flag exists at expected line

### ASan false positives on macOS arm64 (MEDIUM/MEDIUM)
- ✅ Relevant rule: `asan-pthread-stack-aliasing-darwin.md` (cross-thread context struct handling)
- ✅ Mitigation: heap-allocate agent_turn_ctx_t if stack-local ASan false positives appear (pre-decision per agent, not blocking)
- ✅ Fallback: if ASan blocks closure due to false positive, disable `detect_stack_use_after_scope` for that specific function only (documented in WAR)

### Autodream scheduler not running (MEDIUM/MEDIUM)
- ✅ US-1 test calls `hu_autodream_runner()` directly, bypassing scheduler
- ✅ If AC-1.2 daemon run sees 0 summaries, SHADOW log will show 0 bytes, signaling diagnosis

### Bandit state not injected (MEDIUM/MEDIUM)
- ✅ US-2 AC-2.1 audit discovers injection requirement before wiring
- ✅ If bandit field missing from agent struct, design is revised (likely via config/thread-local cache)
- ✅ Test in Phase 3 passes bandit explicitly; does not depend on agent struct wiring

### RATING-BLOCKED on US-4 (MEDIUM/HIGH)
- ✅ AC-4.5 explicitly documents user responsibilities (Seth export + rater recruitment)
- ✅ RATING-BLOCKED.md gates Stories US-1 and US-3 activation (do NOT flip to default-ON without blind A/B PASS)
- ✅ Scrum master enforces: US-1 AC-1.3 and US-3 AC-3.5 gate comments remain OFF until US-4 human-in-the-loop returns PASS

---

## Artifacts & Evidence Directory Structure

```
sprints/sprint-1/
├── stories.md                          # Product owner's story definitions
├── plan.md                             # THIS FILE — wave sequencing & DoD
├── designs/
│   ├── US-1.md, US-2.md, US-3.md, US-4.md, US-5.md
├── evidence/
│   ├── US-1/
│   │   ├── shadow-run-metrics.json     # AC-1.2: SHADOW-mode measurement (graph_context_bytes, summary_count)
│   │   └── gate-comment-pinned-test.txt # AC-1.3: proof comment exists
│   ├── US-2/
│   │   ├── wiring-report.md            # AC-2.2 (if wired): existing call sites + flow
│   │   └── humanization_bandit_tests.txt # AC-2.3 (if dead, wired): test PASS output
│   ├── US-3/
│   │   ├── calibration-metrics.json    # AC-3.3: suppressed_count, kept_count per contact
│   │   └── regression-results.txt      # AC-3.4: diff of three test runs (OFF, default, SHADOW)
│   ├── US-4/
│   │   ├── synthetic_triples.json      # AC-4.2: 5 test items
│   │   ├── rating_sheet_example.csv    # AC-4.3: unlabeled, 5 rows
│   │   ├── blind-ab-results.json       # AC-4.4: score.py output (detection rate, Wilson CI)
│   │   └── RATING-BLOCKED.md           # AC-4.5: user responsibilities (CRITICAL)
│   └── US-5/
│       └── six-axes.md                 # AC-5.1: axis definitions tied to Seth's voice
```

---

## One-Line Wave Summary

**Wave 1 (Python):** US-4 + US-5 parallel, ~45 min, pure Python harness wiring (synthetic test run PASS, no C changes, RATING-BLOCKED user gate).

**Wave 2–4 (C, strict sequence):** US-1 (GraphRAG SHADOW gate comment, ~2–3h) → US-3 (salience trichotomy + calibration, ~2–4h) → US-2 (bandit audit + conditional wiring, ~1.5–3h). Total C work: ~6–10 hours (spans multiple sessions). All prod-activation flags gated on US-4's blind A/B result.

---

`RESULT_scrum-master=PLAN_READY`

**Next step:** Dispatch Wave 1 (US-4, US-5 general-purpose agents in parallel, Python-only, no C collision). Wave 1 completion unlocks Wave 2 (US-1 C agent, agent_turn.c:1471 gate comment).
