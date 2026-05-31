# Local-First Voice — Design

> Companion to [requirements.md](requirements.md). Slice 1 (local STT) is
> implemented; later slices are sketched at the end.

## Boundary decision: HTTP to a local server, not an embedded FFI

The STT engine (whisper.cpp) is C++ with heavy compute and model-file
dependencies. h-uman keeps it **behind an HTTP boundary on `127.0.0.1`** rather
than linking it in:

- **Why not FFI/embed.** Embedding Whisper would pull a large C++/model
  dependency into a runtime whose moat is a ~23 MB, near-zero-dependency binary
  with a <30 ms startup. It would also couple our process lifetime and memory
  budget to a model that the user may want to swap, GPU-accelerate, or run on a
  different box. This is exactly the cross-language coupling the
  **cross-language-via-HTTP** rule exists to prevent: keep language boundaries as
  a local HTTP call, not an in-process binding.
- **Privacy is preserved**, because the boundary is **loopback**. Audio bytes go
  to `127.0.0.1:8080`, never to a cloud. "Local-first" does not require
  "in-process"; it requires "on-device". A loopback HTTP hop satisfies the
  privacy thesis while keeping the runtime lean and the engine swappable.
- **Transport mechanism.** We shell out to `curl` via `hu_process_run` (the same
  approach `local_tts.c` and several providers already use), not libcurl — so the
  feature works in the zero-extra-dependency build.

## whisper.cpp `/inference` contract (verified from source, 2026-05-31)

From `examples/server/server.cpp` in `ggml-org/whisper.cpp`:

| Aspect | Value | Consequence for us |
|---|---|---|
| Method | `POST` | matches |
| Audio field | multipart `file` | we send `-F file=@<path>` |
| `response_format` default | `json` | response is `{"text": <results>}` |
| `model` form field | **none** (model set at startup via `-m`) | send `model` only when configured |
| `language` form field | **none** on `/inference` (auto-detect / `-l` at startup) | harmless if sent; sent only when configured |
| Default listen | `127.0.0.1:8080` | our documented default endpoint is `http://127.0.0.1:8080/inference` |
| Unknown form fields | ignored by cpp-httplib | extra fields never error |

Key insight: the **pre-existing** `local_stt.c` already worked against
whisper.cpp — `file=@` is read, `model`/`language` are ignored, and the default
`response_format` already yields `{"text": ...}`. Slice 1 makes that
*intentional and robust* rather than *accidental*:

1. **Pin `response_format=json`** explicitly so a future server whose default is
   `text` cannot break our JSON parse.
2. **Emit `model`/`language` only when configured** — correct for whisper.cpp
   (per-request `model` is meaningless there) and still correct for
   OpenAI-compatible servers (the user sets `voice.stt_model` for those).
3. **Make the request construction testable** (next section).

OpenAI-compatible servers (speaches, faster-whisper-server, LocalAI) expose
`POST /v1/audio/transcriptions` with the same multipart `file` + a *required*
`model` and an optional `language`, also defaulting to `{"text": ...}`. The same
builder serves both; the only difference is whether the user sets `stt_model`.

## Request-builder extraction (the core of slice 1)

Before: `hu_local_stt_transcribe` built the `curl` argv inline in its
`#else` (non-test) branch, and the `#if HU_IS_TEST` branch returned a mock
*before any argv existed*. Net effect: the request shape had **zero test
coverage** — the one thing most likely to silently drift.

After: the construction is a pure, side-effect-free function compiled in **all**
builds, following the **security-predicate-extraction** pattern (extract the
hard-to-test decision into a pure function callable from tests without crossing
the spawn/network boundary):

```c
hu_error_t hu_local_stt_build_request(hu_allocator_t *alloc,
                                      const hu_local_stt_config_t *config,
                                      const char *audio_path,
                                      hu_local_stt_request_t *out_req);
void       hu_local_stt_request_free(hu_allocator_t *alloc,
                                     hu_local_stt_request_t *req);
```

`hu_local_stt_request_t` holds a NULL-terminated `argv` plus the heap strings it
owns (bounded by `HU_LOCAL_STT_MAX_OWNED`, so `argv` can never exceed
`HU_LOCAL_STT_MAX_ARGV`). `hu_local_stt_transcribe`'s real branch now calls the
builder, hands `req.argv` to `hu_process_run`, frees the request, and parses the
response exactly as before. The unit tests call `hu_local_stt_build_request`
directly under `HU_IS_TEST` and assert the form fields and endpoint — no socket,
no spawn. (The tests reference the production symbol, satisfying the
test-references-production-symbol rule.)

Built argv:
`curl -s -X POST -F file=@<path> -F response_format=json [-F model=<m>] [-F language=<l>] <endpoint>`

## Configuration flow

```
~/.human/config.json
  voice.local_stt_endpoint  ─┐
  voice.stt_model           ─┤ parse_voice() → hu_voice_settings_t
  voice.stt_language (new)  ─┘            │
                                          ▼ hu_voice_config_from_settings()
                                   hu_voice_config_t {endpoint, stt_model, language}
                                          │  hu_voice_stt_file()
                                          ▼
                                   hu_local_stt_config_t {endpoint, model, language}
                                          │  hu_local_stt_build_request()
                                          ▼      curl argv
```

`local_stt_endpoint` and `stt_model` already existed and parsed. Slice 1 adds
`voice.stt_language` (parse in `config_parse.c`, allow-list in
`config_validate.c`, map to `hu_voice_config_t.language` in `voice_config.c`),
because the `language` field of `hu_local_stt_config_t` was reachable in code but
had **no config key feeding it** — dead plumbing now made usable. Config strings
live in the config arena, so no per-field free path changes.

`local_stt_endpoint` remains **opt-in**: when unset, h-uman keeps its current
cloud-first behavior and the local path is skipped (the existing graceful
fallback in `hu_voice_stt_file` is unchanged). Turnkey ≠ on-by-default; we do not
silently point every user at a server they have not started.

## Latency / quality bar (target, validated in later slices)

For a natural back-and-forth, STT should not be the bottleneck:

- **Quality:** `base.en`/`small.en` for English chat; `large-v3` when accuracy
  matters and the user has the compute. The model is a server-side `-m` choice,
  so users tune it without touching h-uman.
- **Latency:** target **< ~700 ms** STT for a short utterance on Apple-silicon
  `small`-class models — comfortably under the human turn-taking gap. This is a
  *server/model* property; h-uman's contribution (argv build + curl + JSON parse)
  is sub-millisecond and not the bottleneck. End-to-end latency budgeting against
  duplex/barge-in is **slice 3**, not slice 1.

## Is Opus needed for v1? No.

`src/voice/opus.c` is a stub with zero callers. The Gemini Live path frames its
own audio; the local-server path sends a file/PCM the server decodes. A
client-side Opus codec is only required to run a **self-hosted WebRTC voice
call** end-to-end — a later, optional slice. Un-stubbing Opus now would be
YAGNI: code with no caller, carrying real cryptographic/packetization risk.
**Deferred to slice 4 (optional).**

## Slice roadmap

> **Provider-tiering update (2026-05-31):** the default TTS for the product was
> subsequently decided to be **Cartesia Sonic (cloud)** with local Kokoro as a
> privacy mode, while STT stays local — see
> [ADR — Voice provider tiering](../adr/2026-05-31-voice-provider-cartesia-default.md).
> The local TTS path below remains the privacy-mode / fallback engine.

| Slice | Scope | Status |
|---|---|---|
| **1** | Local **STT** turnkey: contract pinned, `stt_language` key, request-builder + tests, guide | **this PR** |
| 2 | Local **TTS** turnkey: same treatment for `local_tts.c` (extract+test JSON body builder, guide) | planned |
| 3 | Duplex / barge-in polish over the local path; end-to-end latency budget | planned |
| 4 | Optional: Opus codec for self-hosted WebRTC voice calls (un-stub `opus.c`) | optional |

See [tasks.md](tasks.md) for the per-slice task list.
