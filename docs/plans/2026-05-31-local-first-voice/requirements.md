# Local-First Voice — Requirements

> **Status:** Slice 1 (local STT, turnkey) SHIPPING in this PR. Slices 2–4
> (TTS turnkey, duplex/barge-in polish, optional self-hosted-WebRTC Opus) are
> specified here but out of scope for the first PR. Authored 2026-05-31 from a
> verify-first read of `src/voice/` at HEAD `0f285d8`.

## Why (the product reason)

h-uman's thesis is **privacy by architecture** — a personal AI that runs on your
hardware and never ships your identity to a cloud. Voice is the sharpest place
this either holds or breaks: today the only *working* voice path is **cloud**
(`src/voice/gemini_live.c`, Gemini Live over WebSocket). Every credible
competitor in realtime voice — OpenAI Realtime, Sesame, ElevenLabs — is
cloud-only. **A working local voice path is the one differentiator none of them
can copy without abandoning their business model.** That is the WHY; this spec
is the HOW.

## Background (verified against code, 2026-05-31)

What is real today:

- **Cloud STT/TTS + realtime** all work: `src/voice/gemini_live.c` (55 KB),
  `realtime.c`, `session.c`, `duplex.c`, `semantic_eot.c`, the WebRTC transport
  (`webrtc.c`, `webrtc_ice.c`, `webrtc_dtls.c`, `webrtc_srtp.c`). Voice is wired
  as a channel: `src/channels/voice_channel.c`.
- **Local STT/TTS exist but are not turnkey.** `src/voice/local_stt.c` and
  `local_tts.c` HTTP-POST to a local STT/TTS server the user must run
  themselves. The STT path uses a `curl -F file=@<path>` multipart upload and
  parses `{"text": ...}` — which already matches whisper.cpp's `/inference`
  contract, but it was undocumented, had no default endpoint guidance, and its
  request construction was **untested** (the `HU_IS_TEST` branch short-circuits
  to a mock before any argv is built).
- **Opus is a stub.** `src/voice/opus.c` (45 lines): every function returns
  `HU_ERR_NOT_SUPPORTED`. `grep` confirms **zero callers** — neither the Gemini
  Live path nor the local-server path needs a client-side codec. It would only
  be needed for a self-hosted WebRTC voice *call*, which is a later slice.

The gap, precisely: **the local path is real plumbing but not a product.** A
user cannot today read a doc, run one command, set one config key, and talk to
their assistant privately. Slice 1 closes that for STT.

## What "turnkey local voice" means

We adopt the **user-run local server** model, not a bundled binary:

- h-uman speaks HTTP to a **local** STT/TTS server on `127.0.0.1`. It does **not**
  embed Whisper via FFI (see [design.md](design.md) — the cross-language-via-HTTP
  boundary), and it does **not** ship a model binary inside the ~23 MB executable.
- "Turnkey" = (1) the request contract matches a popular local server out of the
  box, (2) there is a one-command guide to start that server, (3) one config key
  switches h-uman onto it, and (4) the request construction is unit-tested so it
  cannot silently drift. Privacy is preserved because audio never leaves the
  machine: both endpoints are loopback.

Recommended engine: **whisper.cpp's bundled `whisper-server`**. Rationale: it is
the most widely deployed local STT server, ships in the same repo as the model
tooling, listens on loopback by default, and its `POST /inference` multipart
contract is exactly what `local_stt.c` already builds.

## Functional requirements (slice 1 — STT)

- **R1 — Contract correctness.** The STT request MUST be valid against
  whisper.cpp `/inference`: multipart `file` field, and the response parsed as
  `{"text": ...}`. The request MUST pin `response_format=json` rather than rely
  on the server default.
- **R2 — Per-request fields only when meaningful.** `model` MUST be sent only
  when explicitly configured (whisper.cpp loads its model at startup via `-m`
  and ignores a per-request `model`; OpenAI-compatible servers need it).
  `language` likewise only when configured.
- **R3 — Configurable.** The local STT server URL, model, and language MUST be
  settable from `~/.human/config.json` under `voice` and flow to the request.
- **R4 — Testable without a network.** Request construction MUST be exercised by
  a `HU_IS_TEST`-safe unit test that asserts the exact form fields and endpoint —
  no real socket, no process spawn.
- **R5 — Documented.** A guide MUST give the one command to start the server and
  the config to point h-uman at it.

### Acceptance criteria

- **AC-1 (R1/R2):** building a request with only an endpoint yields argv
  containing `file=@<path>` and `response_format=json`, **no** `model=`/
  `language=`, with the endpoint as the final argument. *(Test:
  `local_stt_build_request_whispercpp_default_fields`.)*
- **AC-2 (R2):** with `model` and `language` set, both appear as `-F` fields.
  *(Test: `local_stt_build_request_includes_model_and_language_when_set`.)*
- **AC-3 (R3):** `voice.local_stt_endpoint`, `voice.stt_model`, and the new
  `voice.stt_language` parse and round-trip. *(Test:
  `test_config_parse_voice_section`.)*
- **AC-4 (R4):** all of the above run under the test binary with zero network/
  spawn and zero ASan findings.
- **AC-5 (R5):** [docs/guides/local-stt-server.md](../../guides/local-stt-server.md)
  exists with a copy-pasteable start command + config snippet.

## Non-goals (explicitly out of scope for slice 1)

- **TTS turnkey** — `local_tts.c` already POSTs JSON to an OpenAI-compatible
  `/v1/audio/speech`; making it turnkey + tested is **slice 2**.
- **Opus codec** — defer. Zero callers today; needed only for a future
  self-hosted WebRTC call path (**slice 4**, optional). Do not un-stub it now.
- **Embedding Whisper in C** — rejected on principle; see design.md.
- **Bundling/auto-installing the STT server** — out of scope; the guide tells the
  user the one command. Auto-provisioning can be revisited later.
- **Duplex/barge-in tuning over the local path** — **slice 3**.

See [tasks.md](tasks.md) for the slice breakdown and [design.md](design.md) for
the engineering decisions.
