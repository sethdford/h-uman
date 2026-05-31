# Local-First Voice — Tasks

Maps tasks to the acceptance criteria in [requirements.md](requirements.md).
Build/verify against the **dev** preset (ASan). Each behavior change ships its
safe-default test (per `tests-that-pin-bugs.md` — assert the contract, never pin
a bug).

## Slice 1 — Local STT turnkey (THIS PR)

| # | Task | ACs | Status |
|---|------|-----|--------|
| 1 | Extract a pure request builder `hu_local_stt_build_request` + `hu_local_stt_request_free` + `hu_local_stt_request_t` in `src/voice/local_stt.c` / `include/human/voice/local_stt.h`. Pin `response_format=json`; emit `model`/`language` only when configured. `hu_local_stt_transcribe`'s real branch delegates to the builder. | R1, R2 / AC-1, AC-2 | ✅ done |
| 2 | Add `voice.stt_language` config key: field in `hu_voice_settings_t` (`config.h`), parse in `config_parse.c`, allow-list in `config_validate.c`, map to `hu_voice_config_t.language` in `voice_config.c`. | R3 / AC-3 | ✅ done |
| 3 | Unit tests (no network/spawn): default-fields, model+language-when-set, null-args, in `tests/test_local_voice.c`; extend `tests/test_config_parse.c` voice section to assert `stt_language` round-trips. | R4 / AC-1..4 | ✅ done |
| 4 | Guide [docs/guides/local-stt-server.md](../../guides/local-stt-server.md): one command to start `whisper-server`, the config snippet, the verification one-liner. | R5 / AC-5 | ✅ done |

**Verify (slice 1):**

```bash
cmake --build build --target human_tests -j"$(nproc)"
./build/human_tests --suite=LocalVoice
./build/human_tests --filter=config_parse_voice
./build/human_tests | grep Results:          # full suite, 0 failures, 0 ASan
cmake --build build --target human -j"$(nproc)"   # production #else path compiles
```

**Coverage:** AC-1→{1,3}; AC-2→{1,3}; AC-3→{2,3}; AC-4→{3}; AC-5→{4}. No orphans.

## Slice 2 — Local TTS turnkey (THIS PR — stacked on slice 1)

| # | Task | Notes | Status |
|---|------|-------|--------|
| 5 | Extract a pure JSON-body builder `hu_local_tts_build_body` from `local_tts.c` so the body is testable without spawning curl. **Fixes a real bug:** the prior inline builder prepended `"model":"` (trailing quote) then `hu_json_append_string` (which emits the value already quoted) → malformed `"model":""kokoro"",`. Kokoro requires `voice`, so every turnkey request was rejected. The builder emits well-formed `{"model":…,"voice":…,"input":…}`. | OpenAI-compatible `/v1/audio/speech` | ✅ done |
| 6 | Confirm the contract against Kokoro-FastAPI (verified: `POST /v1/audio/speech`, JSON `{model,input,voice}`, port 8880); document the one docker command. | [docs/guides/local-tts-server.md](../../guides/local-tts-server.md) | ✅ done |
| 7 | Tests (`local_tts_build_body_{input_only, model_and_voice_well_formed, escapes_input, null_args}`) + guide. The well-formed test is a regression pin for the bug above. | no network/spawn | ✅ done |

**Verify (slice 2):** `./build/human_tests --suite=LocalVoice` (11/11); full suite
13198/13198, 0 ASan; `human` production binary links clean. No new config key —
`local_tts_endpoint` / `tts_model` / `tts_voice` already parse and map.

## Slice 3 — Duplex / barge-in polish over local path (SCOPED / DEFERRED)

A verify-first audit found `duplex.c` and `semantic_eot.c` are mature and pure,
with no concrete defect — and that the eventual local-path tuning needs a
**real-audio test harness** (not unit tests). Slice 3 is therefore scoped into
precise, verifiable tasks and **deferred**; see
[slice-3-duplex-barge-in.md](slice-3-duplex-barge-in.md) for the findings, the
`silence_ms = 0` anti-pattern warning, and tasks 3a–3d.

| # | Task | Notes | Status |
|---|------|-------|--------|
| 8 | End-to-end latency budget for the local STT→LLM→TTS loop; tune `semantic_eot` thresholds for the local path. | → tasks 3b/3c in findings doc | scoped, deferred (needs real-audio harness) |
| 9 | Barge-in correctness when the local server is slower than cloud. | → tasks 3a/3d in findings doc | scoped, deferred (needs real-audio harness) |

## Slice 4 — Optional: Opus for self-hosted WebRTC (optional)

| # | Task | Notes |
|---|------|-------|
| 10 | Un-stub `src/voice/opus.c` (encode/decode) **only if** a self-hosted WebRTC call path is pursued. | zero callers today; do not pre-build |
