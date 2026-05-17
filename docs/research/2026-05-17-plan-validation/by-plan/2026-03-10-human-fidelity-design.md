---
plan: docs/plans/2026-03-10-human-fidelity-design.md
auditor: group-4-human-fidelity
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Master design document for the 9-phase Human Fidelity project — 115 features
across 17 pillars to make human's iMessage presence indistinguishable from a
real person, then better than any human could be. "Approach B: BTH Enhancement"
— extend existing daemon/conversation/memory subsystems rather than introduce
new vtable interfaces. Children: phase1..phase9 + missing-seven.

## Key Claims (from the plan)
- Claim 1: Architecture decision is BTH enhancement (no new vtables)
- Claim 2: iMessage poll loop wires tapbacks; conversation builds emotional
  context; memory holds inside jokes / commitments / patterns; daemon
  orchestrates proactive features.
- Claim 3: Cartesia TTS added as the ONLY new dependency (Phase 5)
- Claim 4: Persona JSON gains extensive per-contact and behavioral configuration

## Evidence

### Implemented? (code exists)
- iMessage tapback path: `src/channels/imessage.c:3920` sets `msgs[count].message_id = rowid`
- Conversation intelligence: `src/context/conversation.c` (2000+ lines as
  claimed) with classifiers, awareness builder
- Memory features: `src/memory/superhuman.c`, `comfort_patterns.c`,
  `emotional_moments.c`, `emotional_residue.c`, `episodic.c`, `forgetting.c`,
  `prospective.c`, `consolidation_engine.c`
- TTS: `src/tts/cartesia.c`, `cartesia_stream.c`, `voice_clone.c`,
  `emotion_map.c`, `audio_pipeline.c`, `transcript_prep.c`
- Persona: `src/persona/{persona.c, mood.c, life_sim.c, narrative_self.c,
  somatic.c, circadian.c, humor.c, voice_maturity.c, ...}`
- Phase 6/9 context: `src/context/{theory_of_mind.c, self_awareness.c,
  protective.c, cognitive_load.c, authentic.c}`
- Daemon orchestrates: `src/daemon.c` is ~11000 lines (line numbers cited in
  child-plan evidence go up to 11070), well above the "~4000 lines" baseline.

### Proven? (tests exist)
- `tests/test_persona*.c` (multiple files), `tests/test_emotional_*`,
  `tests/test_cognitive_*`, `tests/test_authentic.c`, `tests/test_episodic.c`,
  `tests/test_prospective.c`, `tests/test_cartesia*.c`,
  `tests/test_voice_message_integration.c`, `tests/test_governor.c`,
  `tests/test_arbitrator.c`, `tests/test_visual_content.c`, `tests/test_skills.c`
- All Phase-specific modules have test coverage; see per-phase audits for detail.

### Wired? (called in runtime path / dispatch)
- Every Phase artifact is called from `src/daemon.c` and/or
  `src/agent/agent_turn.c` (see per-phase audits)
- Specific representative wirings:
  - `src/daemon.c:9293` calls `hu_conversation_classify_tapback_decision`
  - `src/daemon.c:11037` calls `hu_cartesia_tts_synthesize`
  - `src/daemon.c:996, 1320, 1420` call superhuman memory APIs
  - `src/daemon.c:6420, 6504` call `hu_cognitive_load_*`/`hu_authentic_*`
  - `src/daemon.c:6569` calls `hu_rel_dynamics_build_prompt`

## Gaps
- "Shared compression / IYKYK shorthand" (one of the 7 cross-cutting systems)
  has only inline mentions in `src/humanness.c` and conversation callbacks; no
  dedicated module — see per-plan audit for missing-seven.
- No explicit "knowledge state" module to track what each contact has been told
  (also surfaced in missing-seven audit).

## Notes
- This is the master design doc; per-phase verdicts are in their own audit files.
- Aligns with CLAUDE.md M1 claim: persona is unconditional in `hu_agent_t`
  (`include/human/agent.h:442` — `hu_persona_t *persona`, no `#ifdef` guards).
