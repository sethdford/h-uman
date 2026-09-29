---
title: Voice Cloning Setup
description: Clone your voice for Cartesia TTS and use it across all channels.
category: guides
---

# Voice Cloning Setup

Clone your voice for use with Cartesia TTS. Once cloned, Human uses your voice for all text-to-speech output — including voice messages sent via iMessage, Telegram, Discord, and other channels.

## Prerequisites

- **Cartesia API key**: Set `CARTESIA_API_KEY` environment variable, or add it to `~/.human/config.json` under `providers.cartesia.api_key`.
- **Build with Cartesia**: `cmake --preset dev -DHU_ENABLE_CARTESIA=ON` (or any preset with Cartesia enabled).
- **Audio sample**: 5–10 seconds of clear speech in a quiet environment. Supported formats: WAV, MP3, M4A, CAF, OGG.

## Quick Start (CLI)

```bash
# Record a 10-second sample (use any recording tool)
# Then clone it and assign to your persona:
human voice clone --file ~/my-voice-sample.wav --name "My Voice" --persona seth

# Verify the persona was updated:
human persona show seth | grep voice_id
```

## CLI Reference

```
human voice clone --file <path> [--name <name>] [--lang <code>] [--persona <name>]
```

| Flag | Description | Default |
| ------------ | ---------------------------------------- | ------------ |
| `--file` | Path to audio file (required) | — |
| `--name` | Name for the cloned voice | "My Voice" |
| `--lang` | ISO 639-1 language code | "en" |
| `--persona` | Auto-update this persona's voice config | (none) |

## Preview a Reply (hear what the daemon would send)

```bash
human voice preview --persona seth --text "ok so i thought about it more. that trip sounds amazing! we should totally do it" --incoming "wanna go to zion in october" --out ~/Desktop/preview.caf
```

This runs the exact daemon pipeline — transcript prep (sentence breaks, per-sentence emotion, thinking sounds, nonverbals), the persona's model/speed, and the channel's audio container (CAF for iMessage) — and prints the annotated transcript plus the generation config before writing the file. Use it to tune `voice.default_speed` / `voice.model` without waiting for the daemon's voice roll.

## Measure It: Voice A/B Drip

```bash
python3 scripts/blind_ab/voice_ab.py gen --pairs 12      # clips from your real texts, one axis per pair
python3 scripts/blind_ab/voice_ab.py tick                 # one pair to your self-chat (launchd: ai.human.voice-ab)
python3 scripts/blind_ab/voice_ab.py score                # per-axis preference + 95% CI -> ~/.human/voice_ab/verdict.json
```

Axes: model (sonic-3.6 vs sonic-3), speed (0.85 vs 0.95), prep (on vs off). Reply "A" or "B" (optionally + 1-5) on your phone. Promote a model/speed/clone change only when its axis clears 0.5 with the CI.

Every voice/text decision the daemon makes is logged into `proactive_decisions` with `trigger='voice_reply'` (decision, reason, sent) so `scripts/eval_when_to_speak.py` can score voice timing the way it scores proactive sends.

## Pro Voice Clone (30+ min of your audio)

```bash
python3 scripts/voice/pro_clone.py prepare --in ~/Recordings/   # -> 44.1k mono WAV + manifest, checks the 30-min floor
python3 scripts/voice/pro_clone.py submit --name "Seth Pro"     # datasets -> files -> fine-tune (Startup plan)
python3 scripts/voice/pro_clone.py status                       # poll until completed (up to ~3h)
python3 scripts/voice/pro_clone.py adopt --persona seth         # writes the new voice_id (old kept as previous_voice_id)
```

Then restart the daemon and A/B the new id against the old one before trusting it.

## Gateway API

The `voice.clone` JSON-RPC method accepts base64-encoded audio:

```json
{
  "method": "voice.clone",
  "params": {
    "audio": "<base64-encoded-audio>",
    "mimeType": "audio/wav",
    "name": "My Voice",
    "language": "en",
    "persona": "seth"
  }
}
```

Response:

```json
{
  "voice_id": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "name": "My Voice",
  "language": "en"
}
```

## Web Dashboard

1. Navigate to the **Voice** view.
2. Click **Clone Voice** in the status bar.
3. Enter a voice name and optionally a persona name.
4. Click **Start Recording** and speak for 5–10 seconds.
5. Click **Stop & Clone** to upload and create the voice.

## Agent Tool

The agent has access to a `voice_clone` tool. It can be invoked by the LLM:

```json
{
  "tool": "voice_clone",
  "args": {
    "file_path": "/path/to/audio.wav",
    "name": "User Voice",
    "language": "en",
    "persona": "seth"
  }
}
```

## How It Works

1. **Upload**: Audio is sent to Cartesia's `POST /voices/clone` endpoint.
2. **Clone**: Cartesia processes the audio and returns a voice UUID.
3. **Persona Update**: If a persona name is provided, the voice UUID is written into `~/.human/personas/<name>.json` under `voice.voice_id` with `voice.provider = "cartesia"`.
4. **TTS Usage**: All subsequent TTS synthesis uses the cloned voice ID. The daemon's outbound voice pipeline reads `voice_id` from the active persona and passes it to `hu_cartesia_tts_synthesize()`.

## Persona JSON Structure

After cloning, the persona file includes:

```json
{
  "identity": { ... },
  "voice": {
    "voice_id": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
    "provider": "cartesia"
  }
}
```

## Troubleshooting

- **"voice cloning not available in this build"**: Rebuild with `-DHU_ENABLE_CARTESIA=ON -DHU_ENABLE_CURL=ON`.
- **"CARTESIA_API_KEY environment variable not set"**: Export the key or add it to config.
- **Clone fails with error**: Ensure the audio is 5–10 seconds of clear speech. Very short clips (<3s) or noisy recordings may fail.
- **Voice sounds wrong**: Re-record in a quiet environment. Speak naturally at your normal pace and volume.

## Architecture

```
CLI (human voice clone)
        │
        ▼
hu_voice_clone_from_file()  ─── src/tts/voice_clone.c
        │
        ├─ curl POST /voices/clone (Cartesia API)
        │
        ▼
hu_persona_set_voice_id()   ─── src/tts/voice_clone.c
        │
        ▼
~/.human/personas/<name>.json updated

Gateway (voice.clone RPC)
        │
        ▼
cp_voice_clone()            ─── src/gateway/cp_voice_clone.c
        │
        ├─ base64 decode → hu_voice_clone_from_bytes()
        │
        ▼
Same flow as CLI

Agent Tool (voice_clone)
        │
        ▼
src/tools/voice_clone.c     ─── delegates to hu_voice_clone_from_file()
```

## Related Files

| File | Purpose |
| ------------------------------------ | ------------------------------- |
| `include/human/tts/voice_clone.h` | Public API |
| `src/tts/voice_clone.c` | Core implementation |
| `src/gateway/cp_voice_clone.c` | Gateway RPC handler |
| `src/tools/voice_clone.c` | Agent tool |
| `tests/test_voice_clone.c` | Unit tests |
| `ui/src/components/hu-voice-clone.ts` | Web dashboard component |
