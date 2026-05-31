---
title: Run a Local STT Server (Private Voice)
---

# Run a Local STT Server (Private Voice)

h-uman can transcribe your speech **entirely on your own machine** — audio never
leaves `127.0.0.1`. This is the privacy-first alternative to cloud speech-to-text
(OpenAI Realtime, Gemini Live, etc.). You run a small local server; h-uman talks
to it over loopback HTTP.

This guide covers **speech-to-text (STT)**. The recommended engine is
[whisper.cpp](https://github.com/ggml-org/whisper.cpp)'s bundled server.

## 1. Build and start the server (one time + one command)

```bash
# Build whisper.cpp and its server, once:
git clone https://github.com/ggml-org/whisper.cpp && cd whisper.cpp
cmake -B build && cmake --build build --config Release

# Download a model (small.en is a good speed/quality default for English chat):
sh ./models/download-ggml-model.sh small.en

# Start the server — this is the one command to run private voice:
./build/bin/whisper-server -m models/ggml-small.en.bin
```

The server listens on `127.0.0.1:8080` by default and exposes
`POST /inference`. Pick a bigger model (`-m models/ggml-large-v3.bin`) for higher
accuracy if your hardware can afford it.

## 2. Point h-uman at it

Add a `voice` block to `~/.human/config.json`:

```json
{
  "voice": {
    "local_stt_endpoint": "http://127.0.0.1:8080/inference",
    "stt_language": "en"
  }
}
```

That is all that is required. Notes:

- `local_stt_endpoint` is **opt-in**: with it set, h-uman tries local STT first
  and falls back to cloud only if the local server is unreachable. Leave it unset
  to keep cloud-only behavior.
- `stt_language` is an optional BCP-47 hint (e.g. `"en"`). Omit it to let the
  engine auto-detect.
- `stt_model` is **not needed for whisper.cpp** — the model is chosen by the
  server's `-m` flag at startup. Only set `stt_model` if you point
  `local_stt_endpoint` at an OpenAI-compatible server (see below).

## 3. Verify

With the server running, check it directly:

```bash
curl -s -F file=@/path/to/clip.wav -F response_format=json \
     http://127.0.0.1:8080/inference
# → {"text":" your transcribed words"}
```

If you get a `{"text": ...}` body, h-uman will too — that is the exact request it
builds (`file=@…` + `response_format=json`, plus `model`/`language` only when you
configure them).

## OpenAI-compatible servers (alternative)

If you prefer an OpenAI-compatible transcription server
(e.g. [speaches](https://github.com/speaches-ai/speaches),
faster-whisper-server, LocalAI), point the endpoint at its
`/v1/audio/transcriptions` route and set `stt_model` (those servers require a
model name):

```json
{
  "voice": {
    "local_stt_endpoint": "http://127.0.0.1:8000/v1/audio/transcriptions",
    "stt_model": "whisper-large-v3",
    "stt_language": "en"
  }
}
```

## Troubleshooting

- **No transcription / falls back to cloud.** Confirm the server is up
  (`curl` step above). h-uman silently falls back to cloud STT when the local
  endpoint is unreachable, so a misconfigured port looks like "voice still works
  but isn't private."
- **Empty `text`.** The audio may be silent or in an unsupported format. Feed
  16 kHz mono WAV when in doubt.
- **Wrong language.** Set `stt_language` explicitly instead of relying on
  auto-detect.

## How it fits together

The cross-language boundary here is **HTTP to a local server**, not an embedded
Whisper binding — h-uman stays a small, dependency-light binary and you can swap
or GPU-accelerate the engine independently. The full rationale and the request
contract are in the spec:
[Local-First Voice — Design](../plans/2026-05-31-local-first-voice/design.md).
