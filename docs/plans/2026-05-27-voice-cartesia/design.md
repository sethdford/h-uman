# Voice via Cartesia + Twilio Voice — Design

**Status:** Draft (post-brainstorm 2026-05-27)
**Owner:** Seth
**Mission:** "Visceral superhuman moment" — the assistant picks up the phone, in Seth's cloned voice, remembering everything from the last call.
**Sibling specs:** [`2026-05-26-reflection-loop/`](../2026-05-26-reflection-loop/), [`2026-05-26-calibrated-uncertainty/`](../2026-05-26-calibrated-uncertainty/) (the `hedge_phrases` persona overlay banks ship spoken hedges to this voice channel).

## Why this is a SMALL spec, not a greenfield project

The recon during brainstorming turned up substantial existing infrastructure:

| Component | State | File(s) |
|---|---|---|
| Cartesia TTS API client | **Built** — `hu_cartesia_tts_synthesize` with model_id, voice_id, emotion, speed, volume, output_format | `include/human/tts/cartesia.h`, `src/tts/cartesia.c` |
| Cartesia streaming TTS | **Built** — `cartesia_stream.c` (376+ lines of streaming infrastructure) | `src/tts/cartesia_stream.c` |
| Cartesia STT (Ink Whisper) | **Built** — wired in `src/voice/voice.c` as optional STT provider | `src/voice/voice.c:58-66` |
| Emotion-from-context mapping | **Built** — `hu_cartesia_emotion_from_context` | `src/tts/emotion_map.c` |
| Voice cloning workflow | **Built** — `voice_clone.c` exists | `src/tts/voice_clone.c` |
| Channel-aware output format | **Built** — `hu_tts_format_for_channel` returns "caf" for iMessage, "ogg" for Telegram/Discord, "mp3" default | header line 33 |
| Voice channel (3 modes) | **Built** — `HU_VOICE_MODE_SONATA` (default), `HU_VOICE_MODE_REALTIME`, `HU_VOICE_MODE_WEBRTC` | `include/human/channels/voice_channel.h`, 376 LOC impl |
| Twilio SMS channel | **Built** — webhook, polling, send | `src/channels/twilio.c` (409 LOC) |
| Audio pipeline | **Built** — frame buffering, format conversion | `src/tts/audio_pipeline.c` |

What's MISSING and is the entirety of this spec's scope:

1. **Twilio Voice integration** — `twilio.c` handles SMS, not voice calls. Need TwiML response on inbound call, WebSocket Media Streams handler, μ-law ↔ PCM bridging.
2. **Voice channel mode = Cartesia** — voice_channel.h has THREE modes; Cartesia isn't one of them despite the TTS code being right there. Need `HU_VOICE_MODE_CARTESIA`.
3. **The wiring** that connects: inbound call → Twilio Media Stream WebSocket → Cartesia STT → agent turn → Cartesia streaming TTS → audio frames back to Twilio → caller hears Seth's voice.
4. **Session state** — mapping `twilio.call_sid` to an agent conversation context so a phone call IS a turn-sequence in the existing memory/persona/uncertainty pipeline.

## Goals (Phase 1 / Scope B from brainstorm)

1. Inbound phone calls answered automatically — caller dials a Twilio number, h-uman picks up.
2. Bidirectional streaming audio: caller speaks → Cartesia STT → agent turn → Cartesia TTS (Seth's voice) → caller hears response.
3. End-to-end latency target: ≤500ms from caller-stops-speaking to first-byte-of-response-audio. SOTA via Cartesia Sonic's ~90ms first-byte + Whisper/Ink Whisper STT (~200ms small audio) + agent (~100ms reflexive tier) + Cartesia streaming back (~90ms).
4. The phone call is a turn-sequence in the existing agent loop: persona overlay (with `hedge_phrases` from the calibrated-uncertainty sprint), memory loading, uncertainty signals, reflection patterns — all wired identically to text channels.
5. Hangup detection + clean session teardown.
6. Single voice_id for now (Seth's clone) hard-configured in `voice.cartesia.voice_id`. Per-overlay voice_ids deferred to Scope C.

## Non-goals (Phase 1)

- **Outbound calls** — h-uman calling someone proactively. Inbound only. Scope D.
- **Per-channel voice cloning workflow** — UX for recording reference audio + linking voice_id to persona overlay. Scope C.
- **Voicemail** — if h-uman is offline/busy, the call drops with no special handling. Future.
- **Voice biometric authentication** — anyone who dials the number gets through. Future.
- **Multiple concurrent calls** — single-call-at-a-time for Phase 1; reject new inbound while a call is active. Concurrency in scope C.
- **Voice continuity with text** — i.e., phone call references "what we texted yesterday." Memory IS shared (same `hu_personal_model_t`), but explicit cross-modal awareness is the cross-channel-synthesis sub-project (#2), not this one.

## Architecture overview

```
                  PSTN caller
                       │
                       ▼ (phone rings)
              ┌────────────────────┐
              │  Twilio (cloud)    │
              │  Voice number      │
              │  configured to     │
              │  webhook h-uman    │
              └─────────┬──────────┘
                        │ HTTPS POST /twilio/voice
                        │  (TwiML request)
                        ▼
              ┌────────────────────┐
              │  h-uman daemon     │
              │  HTTPS handler     │── responds with TwiML:
              │                    │   <Response>
              │                    │     <Connect>
              │                    │       <Stream url="wss://..."/>
              │                    │     </Connect>
              │                    │   </Response>
              └─────────┬──────────┘
                        │
                        ▼ (Twilio opens WebSocket to h-uman)
              ┌────────────────────┐
              │  Twilio Media      │
              │  Streams → wss     │  bidirectional audio (8kHz μ-law)
              └─────────┬──────────┘
                        │
                        ▼
              ┌────────────────────┐
              │  hu_voice_call_t   │  per-call session state
              │  (NEW)             │  - call_sid, agent context
              │                    │  - inbound utterance buffer
              │                    │  - VAD (silence-based for v1)
              │                    │  - outbound TTS stream queue
              └────┬───────────┬───┘
                   │           │
                   ▼           ▼
        ┌──────────────┐   ┌──────────────────┐
        │ Cartesia STT │   │ Cartesia TTS     │
        │ (per utt.)   │   │ streaming        │
        │ ~200ms       │   │ ~90ms first byte │
        └──────┬───────┘   └────────▲─────────┘
               │                    │
               ▼                    │
        ┌──────────────────────────┴────────┐
        │   Existing agent turn pipeline    │
        │   (memory, persona, uncertainty,  │
        │    reflection patterns, hedge_    │
        │    phrases from persona overlay)  │
        └───────────────────────────────────┘
```

## The SOTA latency math

Target: ≤500ms from "caller stops speaking" to "first byte of audio response back to caller."

| Stage | Budget | Notes |
|---|---|---|
| VAD detects end of utterance | 50ms | Silence threshold (300ms quiet) — but VAD-end detection itself fires ~50ms after silence starts |
| Cartesia STT round-trip | 200ms | Ink Whisper on a 2-3s utterance; cheap |
| Agent turn (reflexive tier) | 100ms | Gemini 3.1 Flash Lite preview, `thinkingBudget=0` per CLAUDE.md gotcha |
| Cartesia TTS first byte | 90ms | Cartesia Sonic SOTA streaming latency |
| Network egress to Twilio | 30ms | continental US |
| **Total** | **~470ms** | meets ≤500ms target |

Critical constraint per CLAUDE.md "Gemini 3.x thinking-token budget gotcha": the agent turn MUST pass `thinkingConfig.thinkingBudget=0` explicitly, or maxOutputTokens gets eaten by invisible thinking and the call returns empty after burning ~70 thinking tokens. Reflexive tier in the model router is the right shape; verify it's threading `thinkingBudget` correctly.

Slow path (if agent is not reflexive tier): >1s; user perceives lag but call doesn't drop. Backchannel filler (`"mm-hmm"`, `"let me think — "`) plays via TTS while the heavier turn computes. Existing `enable_backchanneling` field in `hu_channel_voice_config_t` hints this is anticipated.

## Components

### `include/human/channels/voice_channel.h` — MODIFY

Add `HU_VOICE_MODE_CARTESIA` to the enum. Extend `hu_channel_voice_config_t` with Cartesia-specific fields (already partly there via the existing config; just need `cartesia_api_key`, `cartesia_voice_id`, `cartesia_model_id`).

### `src/channels/voice_channel.c` — MODIFY

In whatever dispatch function picks among modes, add a Cartesia branch that calls into the new `hu_voice_call_*` API. Existing Sonata/Realtime/WebRTC paths unchanged.

### `src/channels/twilio.c` — MODIFY (extend, do not duplicate)

Add Twilio Voice handling alongside the existing SMS handling. New functions:

```c
/* Handle inbound voice webhook (POST /twilio/voice). Returns TwiML body
 * via out_body that the HTTPS handler sends back to Twilio. */
hu_error_t hu_twilio_voice_on_inbound(
    void *channel_ctx, hu_allocator_t *alloc,
    const char *form_body, size_t form_body_len,
    const char *ws_url, size_t ws_url_len,
    char **out_body, size_t *out_len);

/* Open a Media Streams WebSocket session. Called by the HTTPS handler
 * when Twilio connects via wss://. Owns the call lifecycle. */
hu_error_t hu_twilio_voice_session_open(
    void *channel_ctx, hu_allocator_t *alloc,
    void *ws_handle,  /* opaque WebSocket handle owned by HTTP server */
    const char *call_sid, size_t call_sid_len);

/* Pump one cycle: read inbound audio frames, run VAD, when utterance
 * complete invoke STT + agent + streaming TTS, write outbound frames. */
hu_error_t hu_twilio_voice_session_pump(void *session);

/* Tear down a call session (caller hung up, daemon shutting down, etc.) */
void hu_twilio_voice_session_close(void *session);
```

### `src/voice/call.c` — NEW

Per-call session state and the agent-loop integration:

```c
typedef struct hu_voice_call {
    hu_allocator_t *alloc;
    void *ws_handle;                   /* WebSocket I/O */
    char call_sid[64];

    /* Inbound audio */
    uint8_t  *inbound_mulaw_buf;       /* growable buffer for μ-law frames */
    size_t    inbound_mulaw_len;
    size_t    inbound_mulaw_cap;
    uint64_t  last_voice_ms;           /* last frame above silence threshold */
    bool      utterance_in_progress;

    /* Outbound audio (TTS stream → frames out) */
    void     *tts_stream;              /* hu_cartesia_stream_t opaque */
    bool      tts_streaming;

    /* Agent integration */
    hu_agent_t *agent;
    char        channel_name[32];      /* "voice_twilio" or similar */
    char        turn_id[64];           /* current turn — bumped per utterance */

    /* Latency tracking */
    uint64_t utterance_end_ms;
    uint64_t first_byte_out_ms;
} hu_voice_call_t;

hu_error_t hu_voice_call_create(
    hu_allocator_t *alloc, hu_agent_t *agent,
    void *ws_handle, const char *call_sid, size_t call_sid_len,
    hu_voice_call_t **out);

/* Pump one round-trip cycle */
hu_error_t hu_voice_call_pump(hu_voice_call_t *call);

void hu_voice_call_destroy(hu_voice_call_t *call);
```

### `src/voice/vad.c` — NEW

Voice activity detection. Phase 1: **silence-based** (cheap, deterministic):
- 20ms μ-law frames arrive at 8kHz
- Compute frame energy (sum of |sample - 0x7F| / frame_len)
- Voice if energy > threshold (start ~500 — tunable)
- Silence threshold: 300ms continuous quiet → utterance complete

```c
typedef enum {
    HU_VAD_SILENCE = 0,
    HU_VAD_VOICE_ACTIVE,
    HU_VAD_VOICE_ENDED,    /* trailing 300ms of silence after voice */
} hu_vad_state_t;

hu_vad_state_t hu_vad_update(
    hu_vad_t *vad, const uint8_t *mulaw_frame, size_t frame_len,
    uint64_t timestamp_ms);
```

Phase 2 (deferred to Scope C): WebRTC VAD or Silero ONNX for robustness against background noise.

### `src/voice/codec.c` — NEW (small)

μ-law ↔ PCM 16-bit conversion. Cartesia STT/TTS expect 16-bit PCM at 16-24kHz; Twilio gives us 8kHz μ-law. Resampling 8k → 16k for STT (cheap linear interp acceptable; SOTA upgrade is libsamplerate later).

```c
void hu_codec_mulaw_to_pcm16(
    const uint8_t *mulaw, size_t mulaw_len,
    int16_t *pcm, size_t *out_samples);

void hu_codec_pcm16_to_mulaw(
    const int16_t *pcm, size_t samples,
    uint8_t *mulaw, size_t *out_len);

void hu_codec_resample_8k_to_16k(
    const int16_t *src, size_t src_samples,
    int16_t *dst, size_t *out_samples);
```

These are tiny well-understood operations; reference implementations are in g711.h / ITU G.711.

### `src/http_server.c` (assumed existing — find via `grep -rn "http_server\|hu_http_listen" src/`)

Add two new routes:

| Path | Method | Handler |
|---|---|---|
| `/twilio/voice` | POST | `hu_twilio_voice_on_inbound`, returns TwiML XML |
| `/twilio/stream/:call_sid` | WSS | Upgrade to WebSocket, hands off to `hu_twilio_voice_session_open` |

The WSS upgrade is the new infrastructure if h-uman doesn't yet have a WebSocket server. If it does (likely — `mqtt.h` channel exists which suggests at least some pub/sub primitives), reuse.

### `include/human/config.h` — MODIFY

Add voice/Twilio voice config:

```c
typedef struct hu_twilio_voice_config {
    bool   enabled;                       /* default false (opt-in) */
    char   account_sid[64];               /* shares with twilio SMS config */
    char   auth_token[128];
    char   phone_number[32];              /* h-uman's Twilio number */
    char   public_base_url[256];          /* https://your-host for TwiML/wss URLs */

    char   cartesia_api_key[128];
    char   cartesia_voice_id[64];         /* Seth's cloned voice UUID */
    char   cartesia_model_id[64];         /* "sonic-3-2026-01-12" default */
    char   cartesia_emotion[32];          /* default "" (let emotion_map.c pick) */

    int    silence_threshold_ms;          /* default 300 */
    int    max_call_duration_seconds;     /* default 600 = 10 min */
    bool   enable_backchannel;            /* default true — fill while agent computes */
} hu_twilio_voice_config_t;
```

Root config gets `hu_twilio_voice_config_t twilio_voice;`. Defaults set in config_init.

## The end-to-end flow (concrete)

1. **Caller dials.** Twilio receives the call, calls h-uman's `/twilio/voice` webhook with form-encoded body (CallSid, From, To, etc.).
2. **TwiML response.** `hu_twilio_voice_on_inbound` constructs:
   ```xml
   <Response>
     <Connect>
       <Stream url="wss://<public_base_url>/twilio/stream/<call_sid>"/>
     </Connect>
   </Response>
   ```
   Returned with `Content-Type: text/xml`.
3. **Twilio opens WebSocket.** TLS-protected, sends a `connected` event then `start` event with call metadata, then continuous `media` events at 50/sec (20ms frames, μ-law 8kHz, base64).
4. **`hu_twilio_voice_session_open`** allocates `hu_voice_call_t`, attaches agent reference.
5. **Pump loop** runs continuously:
   - Decode incoming `media` event → μ-law frames → append to `inbound_mulaw_buf`
   - `hu_vad_update` per frame → if `HU_VAD_VOICE_ENDED`:
     - μ-law → PCM16, resample 8k → 16k, write to temp file (Cartesia STT API expects file)
     - Call `hu_cartesia_stt_transcribe` (~200ms)
     - Reset `inbound_mulaw_buf`
   - If transcript non-empty:
     - Start backchannel filler (~50ms TTS of "mm-hmm" or similar) if `enable_backchannel`
     - Construct turn input, call agent turn (existing pipeline — picks reflexive tier from model_router for voice channels)
     - Agent response → `hu_cartesia_stream_synthesize` (streaming TTS)
     - Each TTS audio chunk: PCM → μ-law downsample 16k → 8k → base64 → send `media` event back over WebSocket
   - Detect `stop` event (Twilio side hangup) or local timeout (`max_call_duration_seconds`)
6. **Hangup.** `hu_twilio_voice_session_close` flushes any pending audio, sends final `mark` event, closes WebSocket, frees `hu_voice_call_t`. Last turn's uncertainty log entry persisted as usual.

## Channel name + persona overlay

The voice channel registers as `"voice_twilio"` (or `"voice"` if we prefer generic). Persona overlay for this channel uses the same schema as Tier-1 text channels with hedge_phrases:

```json
{
  "channel": "voice_twilio",
  "formality": 0.4,
  "length_pref": "short",
  "hedge_phrases": {
    "HIGH": [""],
    "MEDIUM": ["pretty sure — ", "going off memory — "],
    "LOW": ["not certain on this — ", "could be off — "],
    "VERY_LOW": ["honestly guessing here — "]
  }
}
```

Voice hedge phrases tend to be SHORTER than text hedges (spoken cadence, mid-sentence). Default voice bank in code ships with abbreviated phrases.

## Failure handling

- **Twilio webhook unreachable / Twilio outage:** caller hears their carrier's "this number is unavailable" message. h-uman has nothing to do — fail open.
- **`hu_voice_call_create` fails (allocation, agent unavailable):** webhook returns TwiML `<Reject reason="busy"/>`. Caller hears busy tone.
- **Cartesia STT fails on an utterance:** log warning, agent turn skipped, backchannel filler plays "sorry — say that again?" via cached TTS. Tries next utterance fresh.
- **Cartesia TTS streaming fails mid-response:** truncate, send a `<Pause length="1"/>` mark, retry with shorter text. If second retry fails, hang up with a "sorry, I'm having trouble" cached audio.
- **WebSocket disconnects mid-call:** clean session teardown, log call duration + last agent turn, persist any partial state.
- **`max_call_duration_seconds` exceeded:** send a "I need to hop off — talk soon" cached audio, hangup gracefully.
- **Per `~/.claude/rules/silent-config-gated-subsystems.md`:** with `twilio_voice.enabled=false`, the `/twilio/voice` route 404s and emits one-shot log on first hit. Daemon startup emits one-shot enable log when configured.

## ECE-friendly logging integration

Every agent turn during a voice call writes to `uncertainty_evaluations` (from the calibrated-uncertainty spec) with `channel = "voice_twilio"`. Additionally, a NEW row goes into a `voice_calls` table:

```sql
CREATE TABLE IF NOT EXISTS voice_calls (
    call_sid              TEXT PRIMARY KEY,
    direction             TEXT NOT NULL,        -- 'inbound' (only for Phase 1)
    caller_number         TEXT,
    started_at_ms         INTEGER NOT NULL,
    ended_at_ms           INTEGER,
    end_reason            TEXT,                  -- 'caller_hangup' | 'agent_timeout' | 'error'
    turn_count            INTEGER NOT NULL DEFAULT 0,
    transcript_path       TEXT,                  -- full transcript dump path
    latency_p50_ms        INTEGER,               -- median caller-stops-to-first-byte
    latency_p95_ms        INTEGER,
    created_at_ms         INTEGER NOT NULL
);
```

Future eval work: plot per-call latency p50/p95 over time, watch for regressions when model router or Cartesia API changes.

## Testing strategy

**Unit tests:**
- `tests/test_voice_codec.c` — μ-law ↔ PCM round-trip preserves audio (within tolerance), resample 8→16 correct sample count
- `tests/test_voice_vad.c` — silence-only frames → SILENCE; energy spike → VOICE_ACTIVE; trailing 300ms silence → VOICE_ENDED
- `tests/test_twilio_voice.c` — TwiML response is well-formed XML with expected `<Stream url="...">`; webhook rejects malformed bodies

**Integration tests (gated on `HU_IS_TEST` per CLAUDE.md):**
- Mock WebSocket + mock Cartesia STT/TTS → simulate inbound media events → assert agent receives transcribed text, outbound media events have valid base64 μ-law
- Mock Twilio start/stop events → assert clean session teardown
- Forced STT failure → assert backchannel filler plays + no agent turn issued

**Latency test:**
- Mock end-to-end with timestamps at each stage → assert p95 < 500ms with mock providers; document real-world expectations with Cartesia + Vertex

**Gate symmetry per `test-source-gate-symmetry.md`:**
- `src/voice/call.c` + `tests/test_voice_call.c` — both ungated (always built; phone-call infra doesn't depend on SQLite although the voice_calls table does)
- `src/uncertainty_storage.c` link via the calibrated-uncertainty sprint stays gated on `HU_ENABLE_SQLITE`

## Sprint sequencing (~2 weeks)

**Week 1 — Foundation:**
- Day 1: Codec (μ-law/PCM/resample) + tests
- Day 2: VAD + tests
- Day 3-4: `hu_voice_call_t` session state, TTS streaming integration with existing `cartesia_stream.c`
- Day 5: Twilio Voice webhook (TwiML response) + tests

**Week 2 — Wiring:**
- Day 6: WebSocket server integration (extend existing HTTP server or add wss handler)
- Day 7-8: Full bidirectional pump loop, agent integration via reflexive tier
- Day 9: voice_calls table + logging + persona overlay channel registration
- Day 10: End-to-end manual smoke (Twilio sandbox, ngrok if no public_base_url yet), acceptance verification

## Acceptance criteria

- **AC-1:** Calling the configured Twilio number rings → h-uman answers within 2s
- **AC-2:** Caller speaks → STT transcription appears in agent turn input within 500ms of silence detected
- **AC-3:** Agent responds → first byte of TTS audio reaches caller within 500ms of utterance end (p50 over 10 test calls)
- **AC-4:** Cloned voice (configured `cartesia_voice_id`) matches Seth's reference recording (subjective listen test)
- **AC-5:** Caller hangs up → `voice_calls` row updated with `end_reason='caller_hangup'`, `ended_at_ms` populated, `turn_count` matches actual turns
- **AC-6:** With `twilio_voice.enabled=false`, the `/twilio/voice` route returns 404; daemon emits one-shot disabled log
- **AC-7:** All unit + integration tests pass with mock providers, 0 ASan errors. Gate-symmetry check passes.

## Risks

- **R1 — Twilio Voice account + phone number not yet provisioned.** *Mitigation:* user obtains Twilio account, buys a number (~$1/mo), provisions webhook URL. Documented as prerequisite in spec; testing uses Twilio's sandbox until production number ready.
- **R2 — h-uman daemon needs publicly-reachable HTTPS URL for Twilio webhook.** *Mitigation:* Phase 1 supports ngrok or any reverse proxy in front of the daemon's existing HTTP server. Production deployment is a separate concern (Cloudflare Tunnel, Tailscale Funnel, etc.).
- **R3 — Latency budget tight on slow paths.** *Mitigation:* backchannel filler buys ~2s of cover. Reflexive tier MUST be enforced via model_router for voice channels; CLAUDE.md "Gemini 3.x thinking-token gotcha" applies. AC-3 measurement validates per release.
- **R4 — WebSocket server doesn't exist in h-uman daemon.** *Mitigation:* recon during Task 6 — if `grep -rn "websocket\|wss\|upgrade.*http" src/` returns no infra, add a small WebSocket server (RFC 6455 is ~200 LOC for server-side accept + frame parse). Otherwise reuse.
- **R5 — Cartesia API key in plaintext config.** *Mitigation:* same posture as existing `cartesia_api_key` in `src/voice/voice.c` config; document secret rotation. Long-term: keychain integration.
- **R6 — Voice cloning quality unverified.** *Mitigation:* AC-4 is a subjective smoke test. The full voice-cloning workflow (recording reference audio, getting voice_id) is Scope C; Phase 1 assumes user has already obtained a voice_id from Cartesia's web UI and configured it.
- **R7 — Single-call-at-a-time limitation.** *Mitigation:* if a new call arrives while one is active, webhook returns `<Reject>`. Caller hears busy tone. Concurrency is Scope C work.
- **R8 — μ-law resampling 8k→16k is lossy.** *Mitigation:* acceptable for Phase 1 — Whisper handles 8kHz natively; resampling for Cartesia TTS direction is the lossy step but humans don't notice it. Upgrade to libsamplerate or sox is a future quality tweak.

## Open questions for the user / sprint kickoff

1. **Twilio account ready?** If not, blocked on account creation + number purchase + webhook config. Document the prerequisite checklist.
2. **Public URL strategy for Phase 1?** ngrok? Cloudflare Tunnel? Tailscale Funnel? Pick one for dev testing; production deployment is a separate concern.
3. **Voice cloning workflow timing:** Phase 1 requires a pre-existing Cartesia voice_id. Does Seth have one yet? If not, do that first (record 30s reference in Cartesia UI, copy UUID into config).
4. **Reflexive tier verification:** confirm `model_router` actually has a "voice" or "reflexive" tier that gates on `thinkingBudget=0` (per the CLAUDE.md Gemini gotcha). If the voice path defaults to the conversational tier, we'll hit empty replies and not know why. This is the biggest sleeper risk.

## Related rules

- `~/.claude/rules/silent-config-gated-subsystems.md` — twilio_voice gating with one-shot log
- `~/.claude/rules/quality-gates.md` — per-task gate: AC-3 latency p95 measurement is the verification step
- `~/.claude/CLAUDE.md` "Gemini 3.x thinking-token budget gotcha" — voice MUST use a tier with explicit thinkingBudget=0
- `~/.claude/CLAUDE.md` "AI Model Versions" — must verify Cartesia model_id is currently live at brief time (sonic-3-2026-01-12 in the existing config); web-search before locking
- `~/.claude/rules/security-predicate-extraction.md` — VAD state machine is a clean predicate target (testable in isolation)
- `~/.claude/rules/asan-pthread-stack-aliasing-darwin.md` — voice_call session state spans the WebSocket I/O thread + the agent turn thread; allocate `hu_voice_call_t` on the heap (NOT stack-local in the WebSocket handler) per this rule
