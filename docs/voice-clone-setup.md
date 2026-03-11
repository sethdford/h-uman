# Cartesia Voice Clone Setup

This guide explains how to create and configure a cloned voice for use with Human's Cartesia TTS integration.

## Prerequisites

- Cartesia API key ([cartesia.ai](https://cartesia.ai))
- `HU_ENABLE_CARTESIA=ON` and `HU_ENABLE_CURL=ON` in your build

## Step 1: Record a Voice Sample

1. Record 30–60 seconds of clear speech in a quiet environment.
2. Use a good microphone; avoid background noise and reverb.
3. Speak naturally at a consistent pace. Include varied intonation.
4. Save as WAV or MP3 (mono, 16 kHz or 44.1 kHz recommended).

## Step 2: Upload to Cartesia

1. Log in to the [Cartesia dashboard](https://play.cartesia.ai).
2. Go to **Voices** → **Create Voice** → **Instant Voice Clone**.
3. Upload your audio file.
4. Wait for processing (usually under a minute).
5. Copy the **Voice ID** (UUID format, e.g. `a0e99841-438c-4a64-b679-ae501e7d6091`).

## Step 3: Add to Persona Config

Add a `voice` block to your persona JSON (`~/.human/personas/<name>.json`):

```json
{
  "version": 1,
  "name": "my-persona",
  "core": { "identity": "...", "traits": [...] },
  "voice": {
    "provider": "cartesia",
    "voice_id": "a0e99841-438c-4a64-b679-ae501e7d6091",
    "model": "sonic-3-2026-01-12",
    "default_emotion": "content",
    "default_speed": 0.95,
    "nonverbals": true
  }
}
```

| Field             | Description                    | Default                |
| ----------------- | ------------------------------ | ---------------------- |
| `provider`        | TTS provider (`"cartesia"`)    | `"cartesia"`           |
| `voice_id`        | Cartesia voice UUID from clone | (required for TTS)     |
| `model`           | Cartesia model ID              | `"sonic-3-2026-01-12"` |
| `default_emotion` | Base emotion tag               | `"content"`            |
| `default_speed`   | Playback speed (0.5–2.0)       | `0.95`                 |
| `nonverbals`      | Enable `[laughter]` etc.       | `true`                 |

## Step 4: Set API Key

Configure your Cartesia API key in `~/.human/config.json` or via the `CARTESIA_API_KEY` environment variable.

## Troubleshooting

- **No voice output**: Ensure `voice_id` is set and `HU_ENABLE_CARTESIA=ON`.
- **Auth errors**: Verify API key and that the voice belongs to your account.
- **Poor quality**: Re-record with less noise; use a higher sample rate.
