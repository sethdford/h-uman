---
plan: docs/plans/2026-03-10-human-fidelity-phase5-voice-messages.md
auditor: group-4-human-fidelity
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Phase 5: voice memos in the persona owner's cloned voice via iMessage, using
Cartesia Sonic-3 TTS. Audio format pipeline (MP3 → CAF via afconvert), voice
message decision engine, emotion-modulated voice, nonverbal sound injection.
Features F34-F39.

## Key Claims (from the plan)
- Claim 1: Cartesia TTS client `src/tts/cartesia.c`
- Claim 2: Emotion map: friendly emotion strings → Cartesia primary emotions
- Claim 3: Voice message decision engine selects when to send voice vs text
- Claim 4: Daemon sends audio attachment via iMessage send path

## Evidence

### Implemented? (code exists)
- `src/tts/cartesia.c`, `src/tts/cartesia_stream.c` — Cartesia REST/streaming
- `src/tts/voice_clone.c` — voice cloning support
- `src/tts/emotion_map.c` — emotion mapping
- `src/tts/audio_pipeline.c` — format conversion
- `src/tts/transcript_prep.c` — text prep before TTS
- `src/tools/send_voice_message.c` — tool that ships voice message
- `src/context/voice_decision.c` — when-to-voice decision engine

### Proven? (tests exist)
- `tests/test_cartesia.c`, `tests/test_cartesia_stream.c`
- `tests/test_emotion_map.c`
- `tests/test_voice_clone.c`
- `tests/test_voice_decision.c`
- `tests/test_voice_message_integration.c` (end-to-end)
- `tests/test_send_voice_message.c`
- `tests/test_voice_maturity.c`, `test_voice.c`, `test_voice_factory_e2e.c`

### Wired? (called in runtime path / dispatch)
- `src/daemon.c:11006` — `hu_cartesia_emotion_from_context(...)`
- `src/daemon.c:11023` — `hu_cartesia_tts_config_t tts_cfg = {...}`
- `src/daemon.c:11037` — `hu_cartesia_tts_synthesize(...)`
- `src/daemon.c:11059,11070` — `hu_cartesia_tts_free_bytes(...)`
- Send path uses tool `send_voice_message` registered in `src/tools/factory.c`

## Gaps
- None material. End-to-end implementation, schema, decision engine, and
  daemon wiring are all present and tested.

## Notes
- Phase 5 introduced the only new external dependency (Cartesia HTTP) per
  plan; libcurl path is HU_IS_TEST guarded by convention.
