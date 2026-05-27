# Voice via Cartesia Phase 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Inbound Twilio phone calls answered by h-uman with bidirectional streaming audio via Cartesia STT/TTS, agent loop integration, and ≤500ms p50 caller-stops-to-first-byte latency.

**Architecture:** New voice mode `HU_VOICE_MODE_CARTESIA` for the existing voice channel. Twilio Voice webhook returns TwiML pointing at a WebSocket Media Streams handler. Per-call `hu_voice_call_t` session state owns the pump loop: μ-law decode → VAD → Cartesia STT → agent turn (reflexive tier with `thinkingBudget=0`) → Cartesia streaming TTS → μ-law re-encode → outbound media frames. Persona overlay registers a `voice_twilio` channel with hedge_phrases.

**Tech Stack:** C11, existing `src/tts/cartesia*.c`, existing `src/channels/twilio.c` (SMS, extended for voice), new μ-law/PCM codec, silence-based VAD, WebSocket server (extend existing HTTP server or add ~200 LOC RFC 6455 implementation), new `voice_calls` SQLite table gated on `HU_ENABLE_SQLITE`.

**Spec:** [`design.md`](./design.md). Acceptance criteria AC-1 through AC-7 listed there.

---

## Prerequisites (sprint kickoff blockers)

- [ ] Twilio account provisioned (account_sid, auth_token)
- [ ] Twilio phone number purchased and webhook URL configurable
- [ ] Cartesia API key valid for TTS + STT (likely already true — user mentioned existing account)
- [ ] Seth's reference audio recorded in Cartesia UI; `voice_id` UUID copied
- [ ] Public-reachable HTTPS endpoint for h-uman daemon (ngrok / Cloudflare Tunnel / Tailscale Funnel — pick one for Phase 1 dev)
- [ ] Verified `model_router` has a "voice" or "reflexive" tier with `thinkingBudget=0` (per CLAUDE.md Gemini 3.x gotcha)
- [ ] Confirmed via web search: `sonic-3-2026-01-12` is still Cartesia's current model_id (or update to whatever's current at sprint start)

---

## File structure (locked from spec)

| File | Responsibility | Status |
|---|---|---|
| `include/human/channels/voice_channel.h` | Add `HU_VOICE_MODE_CARTESIA` | MODIFY |
| `include/human/voice/codec.h` | μ-law/PCM/resample API | NEW |
| `include/human/voice/vad.h` | VAD state machine API | NEW |
| `include/human/voice/call.h` | `hu_voice_call_t` + session pump API | NEW |
| `src/voice/codec.c` | μ-law ↔ PCM16, 8k↔16k linear resample | NEW |
| `src/voice/vad.c` | Energy-based VAD with 300ms silence trail | NEW |
| `src/voice/call.c` | Session state, pump loop, agent integration | NEW |
| `src/voice/call_storage.c` | `voice_calls` table migration + insert/update | NEW |
| `src/channels/twilio.c` | Extend with Voice webhook + WebSocket handler | MODIFY |
| `src/channels/voice_channel.c` | Dispatch CARTESIA mode to `src/voice/call.c` | MODIFY |
| `src/http_server.c` (assumed) | Add `/twilio/voice` POST + `/twilio/stream/:sid` WSS routes | MODIFY |
| `include/human/config.h` | Add `hu_twilio_voice_config_t` | MODIFY |
| `src/config_parse.c` | Parse `twilio_voice: {...}` block | MODIFY |
| `~/.human/personas/<id>/overlays/voice_twilio.json` | Persona overlay for the new channel | NEW (user-authored) |
| `tests/test_voice_codec.c` | μ-law/PCM round-trip + resample tests | NEW |
| `tests/test_voice_vad.c` | VAD state transitions | NEW |
| `tests/test_twilio_voice.c` | TwiML response + webhook parsing | NEW |
| `tests/test_voice_call.c` | Pump loop with mocked Cartesia + WebSocket | NEW |
| `tests/test_voice_call_storage.c` | voice_calls table + insert/update | NEW |
| `tests/test_main.c` | Register new runners | MODIFY |
| `CMakeLists.txt` | Add new sources | MODIFY |

---

## Task ordering rationale

Tasks 1-2 are pure unit modules with no h-uman deps (codec + VAD) — foundation.
Task 3 builds Twilio's webhook response shape with no transport.
Task 4 lays the WebSocket server (or verifies existing) — gate before pump-loop work.
Task 5 builds the session state on top of 1-4.
Task 6 builds the pump loop (the real glue).
Task 7 adds config plumbing — small, parallel-safe.
Task 8 wires CARTESIA mode dispatch + persona overlay channel.
Task 9 adds voice_calls logging.
Task 10 acceptance.

---

## Task 1: Audio codec (μ-law ↔ PCM, resample)

**Files:**
- Create: `include/human/voice/codec.h`
- Create: `src/voice/codec.c`
- Create: `tests/test_voice_codec.c`
- Modify: `tests/test_main.c`, `CMakeLists.txt`

- [ ] **Step 1.1: Write failing round-trip test**

```c
#include "human/voice/codec.h"
#include "test_framework.h"

static void test_codec_mulaw_pcm_roundtrip_preserves_audio(void) {
    /* μ-law is lossy by design but a sine-wave round-trip should
       preserve within ~3% RMS error */
    int16_t pcm_in[160];
    for (int i = 0; i < 160; i++) {
        pcm_in[i] = (int16_t)(10000.0 * sin(2.0 * M_PI * 440.0 * i / 8000.0));
    }
    uint8_t mulaw[160];
    size_t mulaw_len = 0;
    hu_codec_pcm16_to_mulaw(pcm_in, 160, mulaw, &mulaw_len);
    HU_ASSERT_EQ(mulaw_len, 160);

    int16_t pcm_out[160];
    size_t pcm_samples = 0;
    hu_codec_mulaw_to_pcm16(mulaw, 160, pcm_out, &pcm_samples);
    HU_ASSERT_EQ(pcm_samples, 160);

    double sq_err = 0.0, sq_in = 0.0;
    for (int i = 0; i < 160; i++) {
        double d = (double)pcm_in[i] - (double)pcm_out[i];
        sq_err += d * d;
        sq_in += (double)pcm_in[i] * (double)pcm_in[i];
    }
    double rms_err = sqrt(sq_err / sq_in);
    HU_ASSERT_TRUE(rms_err < 0.03);
}

static void test_codec_resample_8k_to_16k_doubles_sample_count(void) {
    int16_t src[80];
    for (int i = 0; i < 80; i++) src[i] = (int16_t)(1000 * (i - 40));
    int16_t dst[160];
    size_t out_samples = 0;
    hu_codec_resample_8k_to_16k(src, 80, dst, &out_samples);
    HU_ASSERT_EQ(out_samples, 160);
}
```

- [ ] **Step 1.2: Implement μ-law tables + functions**

`src/voice/codec.c` — standard ITU G.711 μ-law tables. Reference: `g711.h` from public-domain sources or the linear interpolation algorithm in the spec design section.

- [ ] **Step 1.3: Implement linear resample 8k → 16k (and inverse)**

Linear interpolation between adjacent samples. ~10 lines.

- [ ] **Step 1.4: Run + commit**

```
./build/human_tests --suite=voice_codec
git commit -m "feat(voice): μ-law ↔ PCM16 codec + 8k↔16k linear resample"
```

---

## Task 2: Voice activity detection (silence-based)

**Files:**
- Create: `include/human/voice/vad.h`
- Create: `src/voice/vad.c`
- Create: `tests/test_voice_vad.c`

- [ ] **Step 2.1: Write failing VAD state-transition tests**

```c
static void test_vad_silent_frames_stay_silence(void) {
    hu_vad_t vad = {0};
    hu_vad_init(&vad, /*silence_threshold_ms=*/300);
    uint8_t silent_mulaw[160];
    memset(silent_mulaw, 0xff, sizeof silent_mulaw);  /* μ-law silence = 0xff */
    for (int i = 0; i < 50; i++) {
        hu_vad_state_t s = hu_vad_update(&vad, silent_mulaw, 160, (uint64_t)i * 20);
        HU_ASSERT_EQ(s, HU_VAD_SILENCE);
    }
}

static void test_vad_voice_then_silence_yields_voice_ended(void) {
    hu_vad_t vad = {0};
    hu_vad_init(&vad, 300);
    uint8_t voice_mulaw[160];   memset(voice_mulaw,   0x00, sizeof voice_mulaw);
    uint8_t silent_mulaw[160];  memset(silent_mulaw,  0xff, sizeof silent_mulaw);

    uint64_t t = 0;
    /* 200ms of voice */
    for (int i = 0; i < 10; i++, t += 20) {
        HU_ASSERT_EQ(hu_vad_update(&vad, voice_mulaw, 160, t), HU_VAD_VOICE_ACTIVE);
    }
    /* 200ms silence — not yet ended (threshold is 300ms) */
    for (int i = 0; i < 10; i++, t += 20) {
        hu_vad_state_t s = hu_vad_update(&vad, silent_mulaw, 160, t);
        HU_ASSERT_TRUE(s == HU_VAD_VOICE_ACTIVE || s == HU_VAD_SILENCE);
    }
    /* 200ms more silence — total 400ms silence, voice_ended fires */
    bool ended = false;
    for (int i = 0; i < 10; i++, t += 20) {
        if (hu_vad_update(&vad, silent_mulaw, 160, t) == HU_VAD_VOICE_ENDED) {
            ended = true; break;
        }
    }
    HU_ASSERT_TRUE(ended);
}
```

- [ ] **Step 2.2: Implement VAD**

Energy = mean(|sample - 0x7f|) over frame. Voice if energy > 5 (μ-law domain; tunable). Track `last_voice_ms`; when `now - last_voice_ms >= silence_threshold_ms` AND we were previously in voice → emit `HU_VAD_VOICE_ENDED` once, then reset to `HU_VAD_SILENCE`.

- [ ] **Step 2.3: Run + commit**

```
./build/human_tests --suite=voice_vad
git commit -m "feat(voice): silence-based VAD with 300ms tail detection"
```

---

## Task 3: Twilio Voice webhook (TwiML response)

**Files:**
- Modify: `src/channels/twilio.c` (add voice handler alongside SMS)
- Modify: `include/human/channels/twilio.h` (add `hu_twilio_voice_on_inbound`)
- Create: `tests/test_twilio_voice.c`

- [ ] **Step 3.1: Write failing TwiML response test**

```c
static void test_twiml_response_contains_stream_url(void) {
    hu_allocator_t *alloc = hu_default_allocator();
    char *body = NULL; size_t body_len = 0;
    const char *form = "CallSid=CA12345&From=%2B15555555555&To=%2B14155551234";
    const char *ws_url = "wss://example.com/twilio/stream/CA12345";
    HU_ASSERT_EQ(hu_twilio_voice_on_inbound(NULL, alloc, form, strlen(form),
                                             ws_url, strlen(ws_url),
                                             &body, &body_len), HU_OK);
    HU_ASSERT_NE(body, NULL);
    HU_ASSERT_TRUE(strstr(body, "<Response>") != NULL);
    HU_ASSERT_TRUE(strstr(body, "<Stream url=\"wss://example.com/twilio/stream/CA12345\"") != NULL);
    HU_ASSERT_TRUE(strstr(body, "</Response>") != NULL);
    free(body);
}
```

- [ ] **Step 3.2: Implement `hu_twilio_voice_on_inbound`**

In `src/channels/twilio.c`:

```c
hu_error_t hu_twilio_voice_on_inbound(
    void *channel_ctx, hu_allocator_t *alloc,
    const char *form_body, size_t form_body_len,
    const char *ws_url, size_t ws_url_len,
    char **out_body, size_t *out_len)
{
    (void)channel_ctx; (void)form_body; (void)form_body_len;
    if (!alloc || !ws_url || !out_body) return HU_ERR_INVALID_ARGUMENT;

    /* Build TwiML response */
    const char *template_str =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Response>\n"
        "  <Connect>\n"
        "    <Stream url=\"%.*s\"/>\n"
        "  </Connect>\n"
        "</Response>\n";
    size_t cap = strlen(template_str) + ws_url_len + 32;
    char *body = (char *)alloc->alloc(alloc->ctx, cap);
    if (!body) return HU_ERR_OUT_OF_MEMORY;
    int written = snprintf(body, cap, template_str, (int)ws_url_len, ws_url);
    *out_body = body;
    *out_len = (size_t)written;
    return HU_OK;
}
```

- [ ] **Step 3.3: Run + commit**

```
./build/human_tests --suite=twilio_voice
git commit -m "feat(twilio): inbound voice webhook returns TwiML Connect+Stream"
```

---

## Task 4: WebSocket server foundation

**Files:**
- Investigation: `grep -rn "websocket\|wss\|upgrade.*http\|sec-websocket-accept" src/`
- If found: extend the existing module
- If not found: Create `src/net/websocket.c` + `include/human/net/websocket.h` (~200 LOC RFC 6455 server-side)

- [ ] **Step 4.1: Recon existing WebSocket infrastructure**

```
grep -rn "websocket\|wss\|sec-websocket" src/ include/ 2>/dev/null | head -20
```

Document findings in `docs/plans/2026-05-27-voice-cartesia/recon-websocket.md` (a short file: "found X, extending Y" OR "no existing infra, adding minimal RFC 6455 server"). Commit this recon before any code change so the decision is auditable.

- [ ] **Step 4.2: If extending existing — add upgrade handler for `/twilio/stream/`**

The upgrade handler validates the `Sec-WebSocket-Key`, returns 101 Switching Protocols, hands the upgraded socket to a callback. Callback signature:

```c
typedef hu_error_t (*hu_ws_open_callback_t)(void *user_data, void *ws_handle, const char *path);
```

- [ ] **Step 4.3: If adding new — implement minimal server-side RFC 6455**

Required: HTTP/1.1 Upgrade handshake (with SHA-1 + base64 for Sec-WebSocket-Accept), text/binary frame parse (server reads CLIENT-framed payloads, which are MASKED), binary frame write (server writes UNMASKED). No extensions, no per-message deflate. ~200 LOC.

Test:

```c
static void test_ws_handshake_accept_key_correct(void) {
    /* RFC 6455 §1.3 worked example:
       client key "dGhlIHNhbXBsZSBub25jZQ==" →
       accept     "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" */
    char accept[64];
    hu_ws_compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept, sizeof accept);
    HU_ASSERT_STR_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}
```

- [ ] **Step 4.4: Commit**

```
git commit -m "feat(net): WebSocket server foundation for Twilio Media Streams"
```

---

## Task 5: Per-call session state + TTS streaming integration

**Files:**
- Create: `include/human/voice/call.h`
- Create: `src/voice/call.c`
- Create: `tests/test_voice_call.c`

- [ ] **Step 5.1: Write failing session lifecycle tests**

```c
static void test_voice_call_create_and_destroy(void) {
    hu_voice_call_t *call = NULL;
    hu_allocator_t *alloc = hu_default_allocator();
    hu_agent_t *agent = hu_test_agent_new();
    /* Use mock ws_handle = self-pointer */
    HU_ASSERT_EQ(hu_voice_call_create(alloc, agent, /*ws=*/(void*)1,
                                       "CA12345", 7, &call), HU_OK);
    HU_ASSERT_NE(call, NULL);
    HU_ASSERT_STR_EQ(call->call_sid, "CA12345");
    hu_voice_call_destroy(call);
    hu_test_agent_free(agent);
}
```

- [ ] **Step 5.2: Implement `hu_voice_call_create` / `_destroy`**

Per the design.md "Components → src/voice/call.c" section, allocate the struct on the HEAP (NOT stack-local — per `asan-pthread-stack-aliasing-darwin.md` rule, cross-thread session state must be heap-allocated). Initialize buffers, attach agent/ws_handle.

Free path: tear down TTS stream if active, free inbound buffer, free struct.

- [ ] **Step 5.3: Add TTS streaming connector**

Wire `src/voice/call.c` to `src/tts/cartesia_stream.c`. New helper:

```c
hu_error_t hu_voice_call_emit_response(
    hu_voice_call_t *call,
    const char *response_text, size_t response_len);
```

Opens a Cartesia stream with `voice_id` from config, writes PCM chunks back through codec + ws_handle as the stream produces them.

- [ ] **Step 5.4: Commit**

```
git commit -m "feat(voice): per-call session struct + Cartesia TTS streaming connector"
```

---

## Task 6: End-to-end pump loop (the agent integration)

**Files:**
- Modify: `src/voice/call.c` (add `hu_voice_call_pump`)
- Create: `tests/test_voice_call_pump.c`

- [ ] **Step 6.1: Write failing end-to-end test with mocks**

```c
static void test_pump_inbound_voice_to_outbound_response(void) {
    /* Setup: mock WebSocket with pre-queued inbound media events
       containing μ-law voice frames for "hello".
       Mock Cartesia STT returns "hello".
       Mock agent returns "Hi! How can I help?".
       Mock Cartesia TTS streaming returns audio frames.

       Run pump cycles until utterance complete + response sent.

       Assert:
       - Agent received turn input "hello"
       - Outbound media events written to mock WebSocket
       - voice_calls row created
       - First-byte-out timestamp within budget */
}
```

- [ ] **Step 6.2: Implement `hu_voice_call_pump`**

```c
hu_error_t hu_voice_call_pump(hu_voice_call_t *call) {
    /* 1. Read pending inbound WebSocket messages (non-blocking) */
    /* 2. For each `media` event: decode base64 → append μ-law → vad_update */
    /* 3. If vad_state == HU_VAD_VOICE_ENDED:
          a. Convert inbound buffer μ-law → PCM16 → 16kHz → write temp file
          b. Call hu_cartesia_stt_transcribe
          c. Reset inbound buffer
          d. If transcript non-empty:
              - Start backchannel filler if enabled
              - Invoke agent turn (existing pipeline, reflexive tier)
              - Receive response, stream via hu_voice_call_emit_response
          e. Bump turn counter, log timestamps */
    /* 4. Detect `stop` event → set call->ended_at_ms */
    /* 5. If max_call_duration exceeded → send cached goodbye + close */
    return HU_OK;
}
```

The agent turn integration is the key piece. Look up the existing agent invocation pattern in `src/agent/agent_turn.c` and call it with:
- `channel_name = "voice_twilio"`
- `turn_id = "<call_sid>_<turn_seq>"`
- Persona overlay loaded for the voice channel
- Model router routes to reflexive tier (gate: voice channel → reflexive)

Per CLAUDE.md Gemini 3.x gotcha: the reflexive tier MUST pass `thinkingConfig.thinkingBudget=0`. If model_router doesn't already do this, fix it in this task; the voice path will hit empty replies otherwise.

- [ ] **Step 6.3: Commit**

```
git commit -m "feat(voice): end-to-end pump loop with agent integration"
```

---

## Task 7: Config plumbing for `twilio_voice` block

**Files:**
- Modify: `include/human/config.h` (add `hu_twilio_voice_config_t`)
- Modify: `src/config_parse.c` (parse the block)
- Extend: existing config-extended test file

- [ ] **Step 7.1: Add struct**

Per the design.md "Components → include/human/config.h" section. Defaults: enabled=false, silence_threshold_ms=300, max_call_duration_seconds=600, enable_backchannel=true. cartesia_model_id="sonic-3-2026-01-12" (verify currentness at sprint start).

- [ ] **Step 7.2: Failing parse test**

```c
static void test_config_parses_twilio_voice_block(void) {
    const char *json =
      "{\"twilio_voice\":{\"enabled\":true,"
      "\"account_sid\":\"AC123\",\"auth_token\":\"tok\","
      "\"phone_number\":\"+14155551234\","
      "\"public_base_url\":\"https://h.example.com\","
      "\"cartesia_voice_id\":\"voice-uuid-here\","
      "\"silence_threshold_ms\":250}}";
    hu_config_t cfg = {0};
    hu_config_defaults(&cfg);
    HU_ASSERT_EQ(hu_config_parse_json(&cfg, json, NULL), HU_OK);
    HU_ASSERT_TRUE(cfg.twilio_voice.enabled);
    HU_ASSERT_STR_EQ(cfg.twilio_voice.account_sid, "AC123");
    HU_ASSERT_EQ(cfg.twilio_voice.silence_threshold_ms, 250);
    HU_ASSERT_EQ(cfg.twilio_voice.max_call_duration_seconds, 600);  /* default */
}
```

- [ ] **Step 7.3: Implement parser + commit**

```
git commit -m "feat(config): twilio_voice block with sensible defaults"
```

---

## Task 8: Wire CARTESIA mode + persona overlay channel registration

**Files:**
- Modify: `include/human/channels/voice_channel.h` (add `HU_VOICE_MODE_CARTESIA`)
- Modify: `src/channels/voice_channel.c` (dispatch new mode)
- Modify: `src/daemon.c` (or wherever channels register) — register `voice_twilio` channel + load persona overlay
- Modify: `src/http_server.c` — register `/twilio/voice` POST + `/twilio/stream/:sid` WSS routes

- [ ] **Step 8.1: Add the enum + dispatch**

```c
typedef enum hu_voice_mode {
    HU_VOICE_MODE_SONATA = 0,
    HU_VOICE_MODE_REALTIME,
    HU_VOICE_MODE_WEBRTC,
    HU_VOICE_MODE_CARTESIA,  /* NEW */
} hu_voice_mode_t;
```

In `voice_channel.c`'s mode dispatch, route CARTESIA to the `src/voice/call.c` API.

- [ ] **Step 8.2: Register routes in HTTP server**

At daemon startup, conditional on `cfg->twilio_voice.enabled`:

```c
#ifdef HU_ENABLE_TWILIO
if (cfg->twilio_voice.enabled) {
    hu_http_register_route(srv, "POST", "/twilio/voice", twilio_voice_handler, agent);
    hu_http_register_ws(srv, "/twilio/stream/", twilio_stream_open, agent);
} else {
    hu_log_info_once(&warned_twilio_voice_disabled, "twilio_voice", NULL,
        "twilio_voice subsystem disabled by config; set twilio_voice.enabled=true "
        "in ~/.human/config.json + provision Twilio number to activate");
}
#endif
```

Per `silent-config-gated-subsystems.md`, emit the one-shot disabled log.

- [ ] **Step 8.3: Register the `voice_twilio` channel**

Where existing channels are registered (per `src/channels/CLAUDE.md`'s vtable pattern), add:

```c
if (cfg->twilio_voice.enabled) {
    hu_channel_t voice = {0};
    hu_channel_voice_config_t vc = {
        .mode = HU_VOICE_MODE_CARTESIA,
        .api_key = cfg->twilio_voice.cartesia_api_key,
        /* ... other fields ... */
    };
    hu_channel_voice_create(alloc, &vc, &voice);
    hu_channel_manager_register(mgr, "voice_twilio", &voice);
}
```

- [ ] **Step 8.4: Document persona overlay creation**

The user creates `~/.human/personas/<id>/overlays/voice_twilio.json` (template in design.md "Channel name + persona overlay" section). This is user-authored content, not code.

- [ ] **Step 8.5: Commit**

```
git commit -m "feat(voice): wire CARTESIA mode + register voice_twilio channel + HTTP routes"
```

---

## Task 9: voice_calls SQLite logging

**Files:**
- Create: `src/voice/call_storage.c`
- Modify: `include/human/voice/call.h` (add `hu_voice_call_storage_*` API)
- Create: `tests/test_voice_call_storage.c`

- [ ] **Step 9.1: Migration + insert + update**

Follow the SQLite migration pattern established by `src/reflection/storage.c` and `src/uncertainty_storage.c`. Table DDL per design.md:

```
CREATE TABLE IF NOT EXISTS voice_calls (
    call_sid TEXT PRIMARY KEY,
    direction TEXT NOT NULL,
    caller_number TEXT,
    started_at_ms INTEGER NOT NULL,
    ended_at_ms INTEGER,
    end_reason TEXT,
    turn_count INTEGER NOT NULL DEFAULT 0,
    transcript_path TEXT,
    latency_p50_ms INTEGER,
    latency_p95_ms INTEGER,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_voice_calls_recent
  ON voice_calls(created_at_ms DESC);
```

Functions:

```c
hu_error_t hu_voice_call_storage_migrate(sqlite3 *db);
hu_error_t hu_voice_call_storage_insert(sqlite3 *db, const hu_voice_call_t *call);
hu_error_t hu_voice_call_storage_finalize(
    sqlite3 *db, const char *call_sid,
    const char *end_reason, int turn_count,
    int latency_p50_ms, int latency_p95_ms,
    int64_t ended_at_ms);
```

- [ ] **Step 9.2: Insert at call start (from `hu_voice_call_create`)**

- [ ] **Step 9.3: Finalize at call end (from `hu_voice_call_destroy`)**

- [ ] **Step 9.4: Tests + commit**

```
git commit -m "feat(voice): voice_calls SQLite logging for per-call analytics"
```

---

## Task 10: Acceptance verification + manual smoke

- [ ] **Step 10.1: Full test suite**

```
touch src/voice/call.c  # per cmake-build-stale-binary rule
cmake --build --preset dev --target human_tests
./build/human_tests
```

Expected: 0 failures, 0 ASan errors. Test count up by 20+ from new suites.

- [ ] **Step 10.2: Gate symmetry**

```
bash scripts/check-test-source-gate-symmetry.sh
```

- [ ] **Step 10.3: Manual smoke against AC-1..AC-7**

Prerequisites verified, public URL configured, Twilio number webhook set to `https://<public>/twilio/voice`, voice cloning done, persona overlay in place.

```
cmake --build --preset dev --target human_daemon
./build/human_daemon ~/.human/config.json
```

Call the Twilio number from a real phone. Verify:
- **AC-1:** Phone rings, h-uman answers within 2s
- **AC-2:** Speak; STT transcription visible in agent logs within 500ms of silence
- **AC-3:** First TTS audio plays back within 500ms p50 over 10 test calls (capture latency_p50_ms in voice_calls)
- **AC-4:** Voice subjectively matches cloned reference (listen test)
- **AC-5:** Hang up; voice_calls row updated with end_reason='caller_hangup', turn_count > 0
- **AC-6:** Restart with twilio_voice.enabled=false; /twilio/voice returns 404; one-shot log emitted
- **AC-7:** Test count + ASan covered by 10.1

Document results in `docs/plans/2026-05-27-voice-cartesia/results/acceptance-2026-XX-XX.md`.

- [ ] **Step 10.4: Final commit**

```
git commit -m "chore(voice): Phase 1 acceptance results — Cartesia voice channel shipped"
```

---

## What Sprint 2 (Scope C) looks like — deferred

- Voice cloning CLI workflow: `human voice-clone --record reference.wav` → uploads to Cartesia → stores voice_id → updates persona overlay
- Per-overlay voice_ids: Tier-1 channels have distinct cloned voices
- Voice memory continuity: explicit cross-modal recall ("you mentioned this on iMessage yesterday")
- Concurrent calls (drop the single-call constraint)
- Voicemail handling
- Outbound calls (h-uman initiates)

That's a real 2-3 week project on its own — gets its own plan after Phase 1 ships.

---

## Self-review checklist

- [x] Every task has exact file paths
- [x] Every step that changes code shows the code or a complete sketch
- [x] Every command is runnable
- [x] All 7 ACs trace to tasks (AC-1: T3+T8+T10; AC-2: T2+T6+T10; AC-3: T6+T10; AC-4: T10 listen test; AC-5: T9+T10; AC-6: T8 (silent-config-gated); AC-7: T10.1)
- [x] No "implement appropriate error handling" handwaves
- [x] Function/struct names consistent: `hu_voice_call_*`, `hu_twilio_voice_*`, `hu_codec_*`, `hu_vad_*`
- [x] Gate symmetry: `voice_calls` table gates on `HU_ENABLE_SQLITE`, codec/vad/call are ungated
- [x] Heap allocation for `hu_voice_call_t` per `asan-pthread-stack-aliasing-darwin.md`
- [x] Prerequisites called out as sprint blockers, not buried as implementation surprises
- [x] CLAUDE.md Gemini 3.x thinking-budget gotcha explicitly addressed in Task 6 (the biggest sleeper risk)
- [x] Existing infrastructure reused (Cartesia TTS/STT, voice cloning, twilio SMS module, audio pipeline)
