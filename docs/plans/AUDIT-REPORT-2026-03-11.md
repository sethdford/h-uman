---
title: "Plan documents audit report"
created: 2026-03-11
status: closed
last_audit: 2026-05-25
---

# Plan Documents Audit Report

**Date:** 2026-03-11 (revised)  
**Scope:** All plan documents in `docs/plans/`  
**Focus:** Status markers, implementation verification, contradictions, stale items  
**Test baseline:** 6032/6032 passed, 0 ASan errors

---

## Executive Summary

The Human Fidelity project spans **115 features** across 9 phases in the master design, plus **28 additional features** (F116–F143) in the Missing Seven document. **All 9 phases are code-complete.** All features are either verified implemented or correctly marked as platform-limited.

- **Verified implemented:** F1–F2, F4–F15, F17–F39, F45–F47, F49–F57, F58–F115, F116–F143
- **Platform-limited (correctly marked):** F3 (typing indicator), F41 (message editing), F42 (screen effects), F43 (abandoned typing), F44 (unsend), F48 (meme sharing)

---

## 1. Master Design Document

**File:** `docs/plans/2026-03-10-human-fidelity-design.md`  
**Status:** approved  
**Features:** 115 across 17 pillars

### Pillar 1: Fix Broken Plumbing

| Feature                     | Status            | Verification                                                                      |
| --------------------------- | ----------------- | --------------------------------------------------------------------------------- |
| F1 Tapback reactions        | ✅ Implemented    | `msgs[count].message_id = rowid` at imessage.c:1375; JXA handler exists           |
| F2 Tapback-vs-type decision | ✅ Implemented    | `hu_conversation_classify_tapback_decision` in conversation.c:4830, daemon.c:5922 |
| F3 Typing indicator         | 🔧 Platform limit | Correct — iMessage has no API                                                     |

### Pillar 2: Photo/Media Intelligence

| Feature                    | Status         | Verification                                                                                   |
| -------------------------- | -------------- | ---------------------------------------------------------------------------------------------- |
| F4 Auto-vision pipeline    | ✅ Implemented | `has_attachment` in channel_loop.h, daemon.c:2212–2260, `hu_vision_describe_image`             |
| F5 Photo reaction decision | ✅ Implemented | `hu_conversation_classify_photo_reaction` in conversation.c:5071, daemon.c:5945                |
| F6 Photo viewing delay     | ✅ Implemented | `hu_daemon_photo_viewing_delay_ms` in daemon.c:1683–1718                                       |
| F7 Video awareness         | ✅ Implemented | `has_video` in imessage.c:44, 1270–1299, 1334, 1378; video viewing delay in daemon.c:1697–1718 |

### Pillar 3: Timing Patterns (F8–F12)

| Feature                      | Status         | Verification                                                                                         |
| ---------------------------- | -------------- | ---------------------------------------------------------------------------------------------------- |
| F8 Delayed follow-up         | ✅ Implemented | `hu_superhuman_delayed_followup_schedule`, `hu_superhuman_delayed_followup_list_due` in superhuman.h |
| F9 Double-text               | ✅ Implemented | `hu_conversation_should_double_text` in conversation.c, wired in daemon post-send                    |
| F10 Missed-message ack       | ✅ Implemented | `hu_missed_message_acknowledgment` in daemon.c:1735, daemon.c:6962                                   |
| F11 Natural drop-off         | ✅ Implemented | `hu_conversation_classify_response` returns `HU_RESPONSE_SKIP`; drop-off logic in conversation.c     |
| F12 Morning/evening bookends | ✅ Implemented | `hu_bookend_check`, `hu_bookend_build_prompt` in behavioral.c; wired in daemon.c:1317                |

### Pillar 4: Emotional Intelligence (F13–F17)

| Feature                         | Status         | Verification                                                                                |
| ------------------------------- | -------------- | ------------------------------------------------------------------------------------------- |
| F13 Energy matching             | ✅ Implemented | `hu_conversation_detect_energy`, `hu_conversation_build_energy_directive` in conversation.c |
| F14 Escalation detection        | ✅ Implemented | Escalation logic in conversation.c                                                          |
| F15 Response length calibration | ✅ Implemented | `hu_conversation_calibrate_length` in conversation.c:3026, daemon.c:5211                    |
| F16 Context modifiers           | ✅ Implemented | Context modifier logic in conversation.c                                                    |
| F17 First-time vulnerability    | ✅ Implemented | Vulnerability detection in conversation.c                                                   |

### Pillar 5: Superhuman Memory (F18–F27)

| Feature | Status         | Verification                                                                                                                                                             |
| ------- | -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| F18–F27 | ✅ Implemented | `include/human/memory/superhuman.h` has all APIs: inside_joke, commitment, temporal, delayed_followup, micro_moment, avoidance, topic_baseline, growth, pattern, comfort |

### Pillar 6: Behavioral Consistency (F28–F33)

| Feature | Status         | Verification                                                            |
| ------- | -------------- | ----------------------------------------------------------------------- |
| F28–F33 | ✅ Implemented | conversation.c has fillers, typing quirks, style logic, mirroring, etc. |

### Pillar 7: Voice Messages (F34–F39)

| Feature                     | Status         | Verification                                                                              |
| --------------------------- | -------------- | ----------------------------------------------------------------------------------------- |
| F34 Cartesia TTS            | ✅ Implemented | `src/tts/cartesia.c`, `include/human/tts/cartesia.h`                                      |
| F35 Seth voice clone        | ✅ Implemented | `voice_id` in persona.h:296, persona.c:1535, daemon.c:6884                                |
| F36 Audio format pipeline   | ✅ Implemented | `src/tts/audio_pipeline.c:72–75` (`afconvert`), `hu_audio_mp3_to_caf`, daemon.c:6929      |
| F37 Voice decision engine   | ✅ Implemented | `src/context/voice_decision.c:100` (`hu_voice_decision_classify`), daemon.c:6886          |
| F38 Emotion-modulated voice | ✅ Implemented | `src/tts/emotion_map.c` (`hu_cartesia_emotion_from_context`), daemon.c:6904               |
| F39 Nonverbal sounds        | ✅ Implemented | `hu_conversation_inject_nonverbals` in conversation.c:5334, `[laughter]` tag, daemon:6899 |

### Pillar 8: iMessage Platform (F40–F44)

| Feature                     | Status            | Verification                                                                                    |
| --------------------------- | ----------------- | ----------------------------------------------------------------------------------------------- |
| F40 Inline replies          | ✅ Implemented    | `hu_conversation_should_inline_reply` in conversation.c:4758, daemon.c:6824, guid in imessage.c |
| F41 Message editing         | 🔧 Platform limit | No AppleScript API; `*correction` text pattern used as mitigation                               |
| F42 Screen & bubble effects | 🔧 Platform limit | No effect send API; keyword-triggered effects (e.g. "Happy birthday!") are the mitigation       |
| F43 Abandoned typing        | 🔧 Platform limit | No public typing indicator API                                                                  |
| F44 Unsend                  | 🔧 Platform limit | No programmatic unsend API                                                                      |

### Pillar 9: Advanced Behavioral (F45–F49)

| Feature                  | Status            | Verification                                                                 |
| ------------------------ | ----------------- | ---------------------------------------------------------------------------- |
| F45 Burst messaging      | ✅ Implemented    | Burst logic in conversation/daemon                                           |
| F46 Leave on read        | ✅ Implemented    | `HU_RESPONSE_LEAVE_ON_READ` in conversation                                  |
| F47 Content forwarding   | ✅ Implemented    | `save_for_later` tool in factory, stores via memory vtable                   |
| F48 Meme sharing         | 🔧 Platform limit | Stretch goal — requires external image search API                            |
| F49 "Call me" escalation | ✅ Implemented    | `hu_conversation_should_escalate_to_call` in conversation.c, wired in daemon |

### Pillar 10: Context Awareness (F50–F54)

| Feature              | Status         | Verification                                                             |
| -------------------- | -------------- | ------------------------------------------------------------------------ |
| F50 Calendar         | ✅ Implemented | Calendar logic in proactive/superhuman                                   |
| F51 Weather          | ✅ Implemented | `hu_weather_fetch` in weather_fetch.c with 30-min cache + OpenWeatherMap |
| F52 Sports/events    | ✅ Implemented | `hu_current_events_fetch` in context_ext.c with RSS parsing              |
| F53 Birthday/holiday | ✅ Implemented | `hu_proactive_check_important_dates`, important_dates in persona         |
| F54 Time zone        | ✅ Implemented | `hu_timezone_compute`, `hu_timezone_build_directive` in behavioral.c     |

### Pillar 11: Group Chat (F55–F57)

| Feature | Status         | Verification                                                            |
| ------- | -------------- | ----------------------------------------------------------------------- |
| F55–F57 | ✅ Implemented | `hu_conversation_classify_group`, group_response_rate, group lurk logic |

### Pillar 12: AGI Cognition (F58–F69)

| Feature             | Status         | Verification                                                           |
| ------------------- | -------------- | ---------------------------------------------------------------------- |
| F58 Theory of Mind  | ✅ Implemented | `src/context/theory_of_mind.c`, daemon.c:3156–3172                     |
| F59 Life Simulation | ✅ Implemented | `src/persona/life_sim.c`, `hu_life_sim_get_current`                    |
| F60 Mood            | ✅ Implemented | `src/persona/mood.c`, `hu_mood_get_current`, `hu_mood_build_directive` |
| F61–F69             | ✅ Implemented | authentic.c, cognitive_load.c, protective.c, humor.c, self_awareness.c |

### Pillar 13–16: Deep Memory & Skill Acquisition (F70–F101)

| Feature                     | Status         | Verification                                                                                            |
| --------------------------- | -------------- | ------------------------------------------------------------------------------------------------------- |
| F70–F76 Episodic memory     | ✅ Implemented | `src/memory/episodic.c`, `src/memory/consolidation_engine.c`, forgetting curves, associative recall     |
| F77–F82 Reflection/feedback | ✅ Implemented | `src/intelligence/reflection.c`, `src/intelligence/feedback.c`, daily/weekly reflection wired in daemon |
| F83–F93 External feeds      | ✅ Implemented | `src/feeds/` (apple, google, email, music, processor, oauth, social, news RSS)                          |
| F94–F101 Skill lifecycle    | ✅ Implemented | `src/intelligence/skills.c`, `src/intelligence/skill_system.c`, `src/intelligence/meta_learning.c`      |

### Pillar 17: Authentic Existence (F102–F115)

| Feature             | Status         | Verification                                                                                                |
| ------------------- | -------------- | ----------------------------------------------------------------------------------------------------------- |
| F102 Cognitive load | ✅ Implemented | `src/context/cognitive_load.c`, daemon.c:3569–3577                                                          |
| F103–F115           | ✅ Implemented | `src/context/authentic.c`, `hu_authentic_select`, `hu_physical_state_from_schedule`, tests/test_authentic.c |

---

## 2. Missing Seven Document

**File:** `docs/plans/2026-03-10-human-fidelity-missing-seven.md`  
**Features:** F116–F143 (28 features)

| Pillar                  | Features  | Status         | Verification                                                                                   |
| ----------------------- | --------- | -------------- | ---------------------------------------------------------------------------------------------- |
| Visual Content Pipeline | F116–F120 | ✅ Implemented | `src/visual/content.c` — `hu_visual_scan_recent`, `hu_visual_match_for_contact` (SQLite-gated) |
| Proactive Governor      | F121–F124 | ✅ Implemented | `src/agent/governor.c`, daemon wired at lines 883, 887, 1505                                   |
| Contact Knowledge State | F125–F129 | ✅ Implemented | `src/memory/knowledge.c`, daemon.c:3433–3466                                                   |
| Collaborative Planning  | F130–F133 | ✅ Implemented | `src/agent/planning.c`, tests/test_planning.c                                                  |
| Context Arbitration     | F134–F137 | ✅ Implemented | `src/agent/arbitrator.c`, tests/test_arbitrator.c                                              |
| Relationship Dynamics   | F138–F140 | ✅ Implemented | `src/agent/rel_dynamics.c`, `src/persona/relationship.c`                                       |
| Shared Compression      | F141–F143 | ✅ Implemented | `src/memory/compression.c`, daemon.c:3481–3502                                                 |

---

## 3. Phase Plan Status

| Phase | File                                                       | Status   | Features                                          |
| ----- | ---------------------------------------------------------- | -------- | ------------------------------------------------- |
| 1     | 2026-03-10-human-fidelity-phase1-foundation.md             | complete | F1, F2, F4–F7, F10, F11, F15, F40–F44             |
| 2     | 2026-03-10-human-fidelity-phase2-emotional-intelligence.md | complete | F13–F17, F25, F27, F29, F33, F45, F46             |
| 3     | 2026-03-10-human-fidelity-phase3-superhuman-memory.md      | complete | F18–F24, F26, F30, F31, F50, F53                  |
| 4     | 2026-03-10-human-fidelity-phase4-behavioral-polish.md      | complete | F8, F9, F12, F28, F32, F47–F49, F51, F52, F54–F57 |
| 5     | 2026-03-10-human-fidelity-phase5-voice-messages.md         | complete | F34–F39                                           |
| 6     | 2026-03-10-human-fidelity-phase6-agi-cognition.md          | complete | F58–F69                                           |
| 7     | 2026-03-10-human-fidelity-phase7-deep-memory.md            | complete | F70–F76, F83–F93                                  |
| 8     | 2026-03-10-human-fidelity-phase8-skill-acquisition.md      | complete | F77–F82, F94–F101                                 |
| 9     | 2026-03-10-human-fidelity-phase9-authentic-existence.md    | complete | F102–F115                                         |

---

## 4. Other Plan Documents

| Document                                   | Status         | Notes                              |
| ------------------------------------------ | -------------- | ---------------------------------- |
| 2026-03-10-human-fidelity-missing-seven.md | ✅ Implemented | F116–F143 all verified             |
| 2026-03-07-sota-ux-sweep.md                | ✅ Implemented | UI components; all waves done      |
| 2026-03-10-group-chat-handling.md          | ✅ Implemented | Group chat logic in conversation.c |
| 2026-03-08-better-than-human.md            | ✅ Implemented | BTH metrics, superhuman registry   |

---

## 5. Platform Limitations (Accepted)

These features cannot be implemented due to macOS/iMessage API constraints:

| Feature                     | Limitation                                               | Mitigation                                       |
| --------------------------- | -------------------------------------------------------- | ------------------------------------------------ |
| F3 Typing indicator         | iMessage has no typing indicator API via AppleScript/JXA | None needed — typing bubble is native UI only    |
| F41 Message editing         | AppleScript has no API for editing sent messages         | `*correction` text pattern (standard convention) |
| F42 Screen & bubble effects | No effect send API via AppleScript                       | Keyword-triggered effects on recipient's end     |
| F43 Abandoned typing        | No controllable typing indicator                         | Aspirational — not implementable                 |
| F44 Unsend                  | No programmatic unsend API via AppleScript               | Extremely rare use case; skipped                 |
| F48 Meme/image sharing      | Requires external image search API                       | Stretch goal for future work                     |

---

## 6. Test Count History

| Milestone          | Tests  |
| ------------------ | ------ |
| CLAUDE.md baseline | 3,849+ |
| Phase 9 plan       | 3,673+ |
| Current            | 4,662  |

---

## 7. Resolved Issues (Previously Open)

All items from the original audit have been resolved:

- ~~Phase 7/8 not implemented~~ — Fully implemented and daemon-wired
- ~~F9, F12 partial~~ — Fully implemented
- ~~F40 unverified~~ — Implemented with guid tracking in imessage.c
- ~~F42 unverified~~ — Correctly classified as platform limitation
- ~~F51 partial~~ — `hu_weather_fetch` with OpenWeatherMap + 30-min cache
- ~~F49 unverified~~ — `hu_conversation_should_escalate_to_call` exists
- ~~F54 unverified~~ — `hu_timezone_compute`, `hu_timezone_build_directive` exist
- ~~F10 unverified~~ — `hu_missed_message_acknowledgment` wired in daemon
- ~~F35–F39 partial~~ — All voice pipeline components verified
- ~~Phase plan statuses stale~~ — All updated to `complete`
- ~~Master design ⚠️ markers stale~~ — All updated to reflect implementation

---

## 8. Essential Files for Understanding

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
| Deep Memory        | `src/memory/episodic.c`, `src/memory/consolidation_engine.c`                                    |
| Feeds              | `src/feeds/` (apple, google, email, music, processor, oauth, social, news)                      |
| Skills             | `src/intelligence/skills.c`, `src/intelligence/skill_system.c`, `src/intelligence/reflection.c` |
| Voice              | `src/tts/cartesia.c`, `src/tts/audio_pipeline.c`, `src/tts/emotion_map.c`                       |
| Visual Content     | `src/visual/content.c`                                                                          |
| Daemon integration | `src/daemon.c` (key wiring for all systems)                                                     |
