---
title: Run a Local TTS Server (Private Voice)
---

# Run a Local TTS Server (Private Voice)

h-uman can speak replies **entirely on your own machine** — text never leaves
`127.0.0.1`. This is the privacy-first alternative to cloud text-to-speech
(OpenAI, ElevenLabs, Cartesia). You run a small local server; h-uman POSTs to it
over loopback HTTP and plays the audio it returns.

This guide covers **text-to-speech (TTS)**. For the speech-to-text side, see
[Run a Local STT Server](local-stt-server.md). The recommended engine is
[Kokoro-FastAPI](https://github.com/remsky/Kokoro-FastAPI), an OpenAI-compatible
TTS server with natural voices.

## 1. Start the server (one command)

```bash
# CPU (no GPU required):
docker run -p 8880:8880 ghcr.io/remsky/kokoro-fastapi-cpu:latest

# NVIDIA GPU:
docker run --gpus all -p 8880:8880 ghcr.io/remsky/kokoro-fastapi-gpu:latest
```

The server listens on `127.0.0.1:8880` and exposes `POST /v1/audio/speech`.

## 2. Point h-uman at it

Add (or extend) the `voice` block in `~/.human/config.json`:

```json
{
  "voice": {
    "local_tts_endpoint": "http://127.0.0.1:8880/v1/audio/speech",
    "tts_model": "kokoro",
    "tts_voice": "af_heart"
  }
}
```

Notes:

- `local_tts_endpoint` is **opt-in**: with it set, h-uman synthesizes locally and
  falls back to cloud TTS only if the local server is unreachable. Leave it unset
  to keep cloud-only behavior.
- `tts_voice` is **required by Kokoro** — pick any of its voices (`af_heart`,
  `af_bella`, `am_adam`, …). `tts_model` is `"kokoro"` for this server.
- These two keys are sent as the JSON body `{"model","voice","input"}`; both are
  omitted when unset, so an OpenAI-compatible server can use its own defaults.

## 3. Verify

With the server running, synthesize directly:

```bash
curl -s -X POST http://127.0.0.1:8880/v1/audio/speech \
  -H 'Content-Type: application/json' \
  -d '{"model":"kokoro","voice":"af_heart","input":"hello from a local voice"}' \
  -o /tmp/out.mp3 && file /tmp/out.mp3
# → /tmp/out.mp3: Audio file ...
```

That is the exact request shape h-uman builds (`{"model":…,"voice":…,"input":…}`,
with `model`/`voice` present only when you configure them).

## OpenAI-compatible servers (alternative)

Any server implementing OpenAI's `/v1/audio/speech` works — point the endpoint at
it and set `tts_model` / `tts_voice` to values that server understands:

```json
{
  "voice": {
    "local_tts_endpoint": "http://127.0.0.1:8000/v1/audio/speech",
    "tts_model": "tts-1",
    "tts_voice": "alloy"
  }
}
```

## Troubleshooting

- **No audio / falls back to cloud.** Confirm the server is up (the `curl` step
  above). h-uman silently falls back to cloud TTS when the local endpoint is
  unreachable, so a wrong port looks like "voice still works but isn't private."
- **Server returns 400.** Kokoro requires a `voice`; make sure `tts_voice` is set
  to a voice the server knows.
- **Wrong/garbled audio format.** Kokoro defaults to MP3; most players auto-detect
  it. Use a different `response_format` server-side if you need WAV/PCM.

## How it fits together

The cross-language boundary here is **HTTP to a local server on loopback**, not
an embedded TTS binding — h-uman stays a small, dependency-light binary and you
can swap or GPU-accelerate the engine independently. Rationale and the request
contract are in the spec:
[Local-First Voice — Design](../plans/2026-05-31-local-first-voice/design.md).
