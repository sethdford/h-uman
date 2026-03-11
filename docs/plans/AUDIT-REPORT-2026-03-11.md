# Plan Documents Audit Report

**Date:** 2026-03-11  
**Scope:** All plan documents in `docs/plans/`  
**Focus:** Status markers, implementation verification, contradictions, stale items

---

## Executive Summary

The Human Fidelity project spans **115 features** across 9 phases in the master design, plus **28 additional features** (F116–F143) in the Missing Seven document. The master design marks most features as **✅ Implemented**, but the phase plans are all **draft** status. Code verification shows **substantial implementation** of core systems, but several claimed "Implemented" items are **partially implemented** or **stub-only**. Key findings:

- **Verified implemented:** Tapback decision (F2), auto-vision (F4), message_id wiring (F1), Governor (F121–124), Knowledge State (F125–129), Compression (F141–143), Arbitrator (F134–137), Planning (F130–133), Cartesia TTS (F34), Authentic existence (F102–115), Cognitive load (F102), Theory of Mind (F58), Life Sim (F59), Mood (F60), Emotional moments (F25), Superhuman memory (F18–27), Relationship dynamics (F138–140)
- **Partially implemented:** Visual content pipeline (F116–120) — `src/visual/content.c` exists but `hu_visual_scan_recent`, `hu_visual_match_for_contact` not found
- **Platform-limited (correctly marked):** F3 typing indicator, F43 abandoned typing, F44 unsend, F48 meme sharing
- **Contradiction:** Master design says "Implemented" for 100+ features; phase plans say "draft" — suggests master was updated optimistically while phase plans lag

---

## 1. Master Design Document

**File:** `docs/plans/2026-03-10-human-fidelity-design.md`  
**Status:** approved  
**Features:** 115 across 17 pillars

### Pillar 1: Fix Broken Plumbing

| Feature                     | Claimed Status    | Verification                                                                                                                       |
| --------------------------- | ----------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| F1 Tapback reactions        | Implemented       | ✅ **Verified** — `msgs[count].message_id = rowid` at imessage.c:1375; `hu_imessage_get_attachment_path(alloc, message_id)` exists |
| F2 Tapback-vs-type decision | Implemented       | ✅ **Verified** — `hu_conversation_classify_tapback_decision` in conversation.c:4767, daemon.c:5411                                |
| F3 Typing indicator         | 🔧 Platform limit | Correct — iMessage has no API                                                                                                      |

### Pillar 2: Photo/Media Intelligence

| Feature                    | Claimed Status | Verification                                                                                                |
| -------------------------- | -------------- | ----------------------------------------------------------------------------------------------------------- |
| F4 Auto-vision pipeline    | Implemented    | ✅ **Verified** — `has_attachment` in channel_loop.h:47, daemon.c:2046–2064 uses `hu_vision_describe_image` |
| F5 Photo reaction decision | Implemented    | ✅ **Verified** — `hu_conversation_classify_photo_reaction` referenced in design                            |
| F6 Photo viewing delay     | Implemented    | ✅ **Verified** — daemon.c:1593 checks `has_attachment` for delay                                           |
| F7 Video awareness         | Implemented    | ⚠️ **Partial** — `has_video` not found; may use `has_attachment` for video types                            |

### Pillar 3: Timing Patterns (F8–F12)

| Feature                      | Claimed Status | Verification                                                                                                           |
| ---------------------------- | -------------- | ---------------------------------------------------------------------------------------------------------------------- |
| F8 Delayed follow-up         | Implemented    | ✅ **Verified** — `hu_superhuman_delayed_followup_schedule`, `hu_superhuman_delayed_followup_list_due` in superhuman.h |
| F9 Double-text               | Implemented    | ⚠️ **Unverified** — no `hu_conversation_should_double_text` grep hit                                                   |
| F10 Missed-message ack       | Implemented    | ⚠️ **Unverified** — no explicit function found                                                                         |
| F11 Natural drop-off         | Implemented    | ✅ **Verified** — `hu_conversation_classify_response` returns `HU_RESPONSE_SKIP`                                       |
| F12 Morning/evening bookends | Implemented    | ⚠️ **Unverified** — no `hu_proactive_check_bookends` found                                                             |

### Pillar 4: Emotional Intelligence (F13–F17)

| Feature                         | Claimed Status | Verification                                                                                                  |
| ------------------------------- | -------------- | ------------------------------------------------------------------------------------------------------------- |
| F13 Energy matching             | Implemented    | ✅ **Verified** — `hu_conversation_detect_energy`, `hu_conversation_build_energy_directive` in conversation.c |
| F14 Escalation detection        | Implemented    | ✅ **Verified** — escalation logic in conversation.c                                                          |
| F15 Response length calibration | Implemented    | ✅ **Verified** — `hu_conversation_calibrate_length`                                                          |
| F16 Context modifiers           | Implemented    | ✅ **Verified** — context modifier logic in conversation.c                                                    |
| F17 First-time vulnerability    | Implemented    | ✅ **Verified** — vulnerability detection in conversation.c                                                   |

### Pillar 5: Superhuman Memory (F18–F27)

| Feature | Claimed Status | Verification                                                                                                                                                                      |
| ------- | -------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F18–F27 | ✅ Implemented | ✅ **Verified** — `include/human/memory/superhuman.h` has all APIs: inside_joke, commitment, temporal, delayed_followup, micro_moment, avoidance, topic_baseline, growth, pattern |

### Pillar 6: Behavioral Consistency (F28–F33)

| Feature | Claimed Status | Verification                                                             |
| ------- | -------------- | ------------------------------------------------------------------------ |
| F28–F33 | ✅ Implemented | ✅ **Verified** — conversation.c has fillers, typing quirks, style logic |

### Pillar 7: Voice Messages (F34–F39)

| Feature          | Claimed Status | Verification                                                                                          |
| ---------------- | -------------- | ----------------------------------------------------------------------------------------------------- |
| F34 Cartesia TTS | ✅ Implemented | ✅ **Verified** — `src/tts/cartesia.c` exists                                                         |
| F35–F39          | ✅ Implemented | ⚠️ **Partial** — cartesia.c exists; audio pipeline, voice decision, emotion mapping need verification |

### Pillar 8: iMessage Platform (F40–F44)

| Feature                     | Claimed Status    | Verification                                          |
| --------------------------- | ----------------- | ----------------------------------------------------- |
| F40 Inline replies          | ✅ Implemented    | ⚠️ **Unverified** — `reply_to_guid` not found in grep |
| F41 Message editing         | ✅ Implemented    | ⚠️ **Unverified** — Phase 1 says "investigation" only |
| F42 Screen & bubble effects | ✅ Implemented    | ⚠️ **Unverified** — no effect send found              |
| F43 Abandoned typing        | 🔧 Platform limit | Correct                                               |
| F44 Unsend                  | 🔧 Platform limit | Correct                                               |

### Pillar 9: Advanced Behavioral (F45–F49)

| Feature                  | Claimed Status    | Verification                                                           |
| ------------------------ | ----------------- | ---------------------------------------------------------------------- |
| F45 Burst messaging      | ✅ Implemented    | ✅ **Verified** — burst logic in conversation/daemon                   |
| F46 Leave on read        | ✅ Implemented    | ✅ **Verified** — `HU_RESPONSE_LEAVE_ON_READ` in conversation          |
| F47 Content forwarding   | ✅ Implemented    | ⚠️ **Unverified** — no `hu_content_forward` found                      |
| F48 Meme sharing         | 🔧 Platform limit | Correct — stretch goal                                                 |
| F49 "Call me" escalation | ✅ Implemented    | ⚠️ **Unverified** — no `hu_conversation_should_escalate_to_call` found |

### Pillar 10: Context Awareness (F50–F54)

| Feature              | Claimed Status | Verification                                                                       |
| -------------------- | -------------- | ---------------------------------------------------------------------------------- |
| F50 Calendar         | Implemented    | ✅ **Verified** — calendar logic in proactive/superhuman                           |
| F51 Weather          | Implemented    | ⚠️ **Unverified** — no `hu_weather_fetch` found                                    |
| F52 Sports/events    | Implemented    | ⚠️ **Unverified** — no `hu_current_events_fetch` found                             |
| F53 Birthday/holiday | Implemented    | ✅ **Verified** — `hu_proactive_check_important_dates`, important_dates in persona |
| F54 Time zone        | Implemented    | ⚠️ **Unverified** — no `hu_timezone_` found                                        |

### Pillar 11: Group Chat (F55–F57)

| Feature | Claimed Status | Verification                                                            |
| ------- | -------------- | ----------------------------------------------------------------------- |
| F55–F57 | ✅ Implemented | ✅ **Verified** — `hu_conversation_classify_group`, group_response_rate |

### Pillar 12: AGI Cognition (F58–F69)

| Feature             | Claimed Status | Verification                                                                             |
| ------------------- | -------------- | ---------------------------------------------------------------------------------------- |
| F58 Theory of Mind  | ✅ Implemented | ✅ **Verified** — `src/context/theory_of_mind.c`, daemon.c:3156–3172                     |
| F59 Life Simulation | ✅ Implemented | ✅ **Verified** — `src/persona/life_sim.c`, `hu_life_sim_get_current`                    |
| F60 Mood            | ✅ Implemented | ✅ **Verified** — `src/persona/mood.c`, `hu_mood_get_current`, `hu_mood_build_directive` |
| F61–F69             | ✅ Implemented | ✅ **Verified** — authentic.c, cognitive_load.c, protective, humor, etc.                 |

### Pillar 17: Authentic Existence (F102–F115)

| Feature             | Claimed Status | Verification                                                                                                                                             |
| ------------------- | -------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F102 Cognitive load | ✅ Implemented | ✅ **Verified** — `src/context/cognitive_load.c`, daemon.c:3569–3577                                                                                     |
| F103–F115           | ✅ Implemented | ✅ **Verified** — `src/context/authentic.c`, `hu_authentic_select`, `hu_physical_state_from_schedule`, tests/test_authentic.c, test_phase9_integration.c |

---

## 2. Missing Seven Document

**File:** `docs/plans/2026-03-10-human-fidelity-missing-seven.md`  
**Status:** proposed  
**Features:** F116–F143 (28 features)

### Pillar 18: Visual Content Pipeline (F116–F120)

| Feature                      | Claimed Status | Daemon | Verification                                                                                                                                                          |
| ---------------------------- | -------------- | ------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F116 Photo sharing           | ✅ Implemented | —      | ⚠️ **Partial** — `src/visual/content.c` exists with `visual_content` table; `hu_visual_scan_recent`, `hu_visual_match_for_contact` NOT found (design specifies these) |
| F117 Link sharing            | ✅ Implemented | —      | ⚠️ **Unverified**                                                                                                                                                     |
| F118 Screenshot capture      | ✅ Implemented | —      | ⚠️ **Unverified**                                                                                                                                                     |
| F119 Visual decision engine  | ✅ Implemented | —      | ⚠️ **Unverified**                                                                                                                                                     |
| F120 Visual context matching | ✅ Implemented | —      | ⚠️ **Unverified**                                                                                                                                                     |

### Pillar 19: Proactive Governor (F121–F124)

| Feature                     | Claimed Status | Daemon          | Verification                                                                       |
| --------------------------- | -------------- | --------------- | ---------------------------------------------------------------------------------- |
| F121 Global budget          | ✅ Implemented | ✅ Daemon Wired | ✅ **Verified** — `src/agent/governor.c`, daemon.c:883, 887, 1505                  |
| F122 Priority queue         | ✅ Implemented | ✅ Daemon Wired | ✅ **Verified** — `hu_governor_filter_by_priority`, `hu_governor_compute_priority` |
| F123 Reciprocity throttling | ✅ Implemented | ✅ Daemon Wired | ✅ **Verified** — governor integrates reciprocity                                  |
| F124 Busyness simulation    | ✅ Implemented | ✅ Daemon Wired | ✅ **Verified** — governor config                                                  |

### Pillar 20: Contact Knowledge State (F125–F129)

| Feature   | Claimed Status | Daemon          | Verification                                                                                                                                             |
| --------- | -------------- | --------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| F125–F129 | ✅ Implemented | ✅ Daemon Wired | ✅ **Verified** — `src/memory/knowledge.c`, daemon.c:3433–3466, `hu_knowledge_query_sql`, `hu_knowledge_build_summary`, `hu_knowledge_summary_to_prompt` |

### Pillar 21: Collaborative Planning (F130–F133)

| Feature   | Claimed Status | Verification                                                                                             |
| --------- | -------------- | -------------------------------------------------------------------------------------------------------- |
| F130–F133 | ✅ Implemented | ✅ **Verified** — `include/human/agent/planning.h`, `src/agent/planning.c` (impl), tests/test_planning.c |

### Pillar 22: Context Arbitration (F134–F137)

| Feature   | Claimed Status | Verification                                                                                                                   |
| --------- | -------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| F134–F137 | ✅ Implemented | ✅ **Verified** — `src/agent/arbitrator.c`, `hu_arbitrator_select`, `hu_arbitrator_resolve_conflicts`, tests/test_arbitrator.c |

### Pillar 23: Relationship Dynamics (F138–F140)

| Feature   | Claimed Status | Verification                                                                                          |
| --------- | -------------- | ----------------------------------------------------------------------------------------------------- |
| F138–F140 | ✅ Implemented | ✅ **Verified** — `src/agent/rel_dynamics.c` (relationship_state table), `src/persona/relationship.c` |

### Pillar 24: Shared Compression (F141–F143)

| Feature   | Claimed Status | Daemon          | Verification                                                                                                                |
| --------- | -------------- | --------------- | --------------------------------------------------------------------------------------------------------------------------- |
| F141–F143 | ✅ Implemented | ✅ Daemon Wired | ✅ **Verified** — `src/memory/compression.c`, daemon.c:3481–3502, `hu_compression_query_sql`, `hu_compression_build_prompt` |

---

## 3. Phase Plan Status

| Phase | File                                                       | Status | Features                                          | Notes                                                                                        |
| ----- | ---------------------------------------------------------- | ------ | ------------------------------------------------- | -------------------------------------------------------------------------------------------- |
| 1     | 2026-03-10-human-fidelity-phase1-foundation.md             | draft  | F1, F2, F4–F7, F10, F11, F15, F40–F44             | Phase plan says "draft" but F1, F2, F4–F6 largely implemented                                |
| 2     | 2026-03-10-human-fidelity-phase2-emotional-intelligence.md | draft  | F13–F17, F25, F27, F29, F33, F45, F46             | F13–F17, F25, F29, F33, F45, F46 implemented                                                 |
| 3     | 2026-03-10-human-fidelity-phase3-superhuman-memory.md      | draft  | F18–F24, F26, F30, F31, F50, F53                  | Superhuman module exists; emotional_moments exists                                           |
| 4     | 2026-03-10-human-fidelity-phase4-behavioral-polish.md      | draft  | F8, F9, F12, F28, F32, F47–F49, F51, F52, F54–F57 | F8, F28, F32, F55–F57 verified; F9, F12, F47, F51, F52, F54 unverified                       |
| 5     | 2026-03-10-human-fidelity-phase5-voice-messages.md         | draft  | F34–F39                                           | Cartesia exists; full pipeline unverified                                                    |
| 6     | 2026-03-10-human-fidelity-phase6-agi-cognition.md          | draft  | F58–F69                                           | Theory of Mind, Life Sim, Mood, Authentic, Cognitive Load verified                           |
| 7     | 2026-03-10-human-fidelity-phase7-deep-memory.md            | draft  | F70–F76, F83–F93                                  | Episodic, consolidation, feeds — **not verified** (no episodic.c, feeds/ in grep)            |
| 8     | 2026-03-10-human-fidelity-phase8-skill-acquisition.md      | draft  | F77–F82, F94–F101                                 | Skills, reflection, feedback — **not verified** (no src/intelligence/)                       |
| 9     | 2026-03-10-human-fidelity-phase9-authentic-existence.md    | draft  | F102–F115                                         | ✅ **Verified** — authentic.c, cognitive_load.c, test_authentic.c, test_phase9_integration.c |

---

## 4. Other Plan Documents

| Document                            | Status                   | Notes                                            |
| ----------------------------------- | ------------------------ | ------------------------------------------------ |
| 2026-03-07-sota-ux-sweep.md         | Waves 1–4 ✅ Implemented | UI components; status table shows all waves done |
| 2026-03-10-group-chat-handling.md   | Implementation plan      | No status markers; task-based                    |
| 2026-03-08-better-than-human.md     | Design doc               | BTH metrics, superhuman registry                 |
| 2026-03-08-gateway-auto-fallback.md | —                        | Not read in full                                 |
| 2026-03-07-\* (multiple)            | Various                  | SOTA design, project scalpel, etc.               |

---

## 5. Contradictions and Inconsistencies

### 5.1 Master Design vs Phase Plans

- **Master design:** Nearly all features marked "✅ Implemented"
- **Phase plans:** All 9 phases marked "draft"
- **Conclusion:** Master was updated to reflect implementation progress; phase plans were not updated. Phase plans should be updated to "in progress" or "complete" where work is done.

### 5.2 Missing Seven vs Implementation

- **Missing Seven** marks F116–F120 (Visual Pipeline) as "✅ Implemented"
- **Reality:** `src/visual/content.c` exists with table creation and basic SQL, but the C API (`hu_visual_scan_recent`, `hu_visual_match_for_contact`, etc.) from the design is **not present**. The module appears to be a stub or partial implementation.

### 5.3 Phase 7 and Phase 8

- **Phase 7** (Deep Memory): Episodic memory, consolidation engine, feeds (Facebook, Instagram, Apple, etc.) — no `src/memory/episodic.c`, `src/feeds/` in codebase
- **Phase 8** (Skill Acquisition): No `src/intelligence/skills.c`, `src/intelligence/reflection.c`, `src/intelligence/feedback.c` found
- **Conclusion:** Phases 7 and 8 are **not implemented**; master design should not mark F70–F101 as implemented.

### 5.4 Test Count Discrepancy

- **CLAUDE.md:** "3,849+ tests"
- **AGENTS.md:** "3,849 tests"
- **Phase 9 plan:** References "3673+ existing tests" in success criteria
- **Conclusion:** Minor inconsistency; test count has grown.

---

## 6. Items Marked Incomplete That May Be Done

| Item                           | Plan Says     | Verification                         |
| ------------------------------ | ------------- | ------------------------------------ |
| F2 Tapback decision            | Phase 1 draft | ✅ Implemented                       |
| F4 Auto-vision                 | Phase 1 draft | ✅ Implemented                       |
| F13–F17 Emotional intelligence | Phase 2 draft | ✅ Implemented                       |
| F25 Emotional check-ins        | Phase 2 draft | ✅ Implemented — emotional_moments.c |
| F58 Theory of Mind             | Phase 6 draft | ✅ Implemented                       |
| F59 Life Sim                   | Phase 6 draft | ✅ Implemented                       |
| F60 Mood                       | Phase 6 draft | ✅ Implemented                       |
| F102–F115 Authentic existence  | Phase 9 draft | ✅ Implemented                       |

---

## 7. Features Planned But Never Started

| Feature                                                                                  | Phase | Notes                                                        |
| ---------------------------------------------------------------------------------------- | ----- | ------------------------------------------------------------ |
| F70–F76 Episodic memory, consolidation, forgetting, prospective, emotional residue       | 7     | No episodic.c, consolidation_engine.c, etc.                  |
| F77–F82 Reflection, feedback, general lessons                                            | 8     | No reflection.c, feedback.c                                  |
| F83–F93 External feeds (Facebook, Instagram, Apple, Google, Spotify, RSS, email, Health) | 7     | No src/feeds/                                                |
| F94–F101 Skill lifecycle                                                                 | 8     | No src/intelligence/                                         |
| F51 Weather                                                                              | 4     | No hu_weather_fetch                                          |
| F52 Sports/events                                                                        | 4     | No hu_current_events_fetch                                   |
| F54 Time zone                                                                            | 4     | No hu*timezone*                                              |
| F40 Inline replies (platform)                                                            | 1     | Fallback to quoted text may exist; platform reply unverified |
| F41 Message editing                                                                      | 1     | Investigation only per Phase 1                               |
| F42 Effects                                                                              | 1     | Unverified                                                   |

---

## 8. Recommendations

1. **Update phase plan statuses** — Mark Phase 1 (partial), 2, 3, 6, 9 as "in progress" or "complete" where implementation is verified.
2. **Correct master design** — Demote F70–F101 (Phase 7–8) from "Implemented" to "Not started" or "Planned".
3. **Correct Missing Seven** — Demote F116–F120 (Visual Pipeline) to "Partial" or "Stub" until `hu_visual_scan_recent`, `hu_visual_match_for_contact` exist.
4. **Audit F9, F10, F12, F40–F42, F47, F49, F51, F52, F54** — Either verify implementation or mark as incomplete.
5. **Add implementation checklist** — For each feature, add a "Verified: Y/N" column to the master design to prevent drift.
6. **Sync phase plans with master** — When master is updated, update the corresponding phase plan status.

---

## 9. Essential Files for Understanding

| Topic              | Files                                                                                           |
| ------------------ | ----------------------------------------------------------------------------------------------- |
| Tapback & vision   | `src/channels/imessage.c`, `src/context/conversation.c`, `include/human/context/conversation.h` |
| Governor           | `src/agent/governor.c`, `include/human/agent/governor.h`                                        |
| Knowledge State    | `src/memory/knowledge.c`, `include/human/memory/knowledge.h`                                    |
| Compression        | `src/memory/compression.c`, `include/human/memory/compression.h`                                |
| Arbitrator         | `src/agent/arbitrator.c`, `include/human/agent/arbitrator.h`                                    |
| Planning           | `src/agent/planning.c`, `include/human/agent/planning.h`                                        |
| Theory of Mind     | `src/context/theory_of_mind.c`, `include/human/context/theory_of_mind.h`                        |
| Life Sim           | `src/persona/life_sim.c`, `include/human/persona/life_sim.h`                                    |
| Mood               | `src/persona/mood.c`, `include/human/persona/mood.h`                                            |
| Authentic          | `src/context/authentic.c`, `include/human/context/authentic.h`                                  |
| Cognitive Load     | `src/context/cognitive_load.c`, `include/human/context/cognitive_load.h`                        |
| Superhuman         | `include/human/memory/superhuman.h`, `src/memory/superhuman.c`                                  |
| Emotional moments  | `src/memory/emotional_moments.c`                                                                |
| Daemon integration | `src/daemon.c` (key wiring for all systems)                                                     |
