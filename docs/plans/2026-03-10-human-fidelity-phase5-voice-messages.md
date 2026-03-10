---
title: "Human Fidelity Phase 5 — Voice Messages via Cartesia"
created: 2026-03-10
status: draft
scope: TTS, iMessage, daemon, persona, conversation intelligence
phase: 5
features: [F34, F35, F36, F37, F38, F39]
parent: 2026-03-10-human-fidelity-design.md
---

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

# Human Fidelity Phase 5 — Voice Messages via Cartesia

Phase 5 of the Human Fidelity project. Implements the "better than human" differentiator: voice memos in Seth's cloned voice via iMessage. Cartesia Sonic-3 TTS, audio format pipeline, voice message decision engine, emotion-modulated voice, and nonverbal sound injection.

**Reference:** `docs/plans/2026-03-10-human-fidelity-design.md`

---

## Technical Context

### Cartesia API

| Item         | Value                                    |
| ------------ | ---------------------------------------- |
| Endpoint     | `POST https://api.cartesia.ai/tts/bytes` |
| Model        | `sonic-3-2026-01-12`                     |
| Auth         | `X-API-Key: <key>` header                |
| Version      | `Cartesia-Version: 2024-06-10`           |
| Content-Type | `application/json`                       |

**Request body:**

```json
{
  "model_id": "sonic-3-2026-01-12",
  "transcript": "Hello, how are you?",
  "voice": { "mode": "id", "id": "voice-uuid-here" },
  "output_format": {
    "container": "mp3",
    "encoding": "mp3",
    "sample_rate": 44100
  },
  "generation_config": {
    "speed": 0.95,
    "emotion": "content"
  }
}
```

**Response:** Raw audio bytes (MP3).

**Primary emotions (best quality):** neutral, angry, excited, content, sad, scared

**Full emotion list:** happy, excited, enthusiastic, elated, euphoric, triumphant, amazed, surprised, flirtatious, joking/comedic, curious, content, peaceful, serene, calm, grateful, affectionate, trust, sympathetic, anticipation, mysterious, angry, mad, outraged, frustrated, agitated, threatened, disgusted, contempt, envious, sarcastic, ironic, sad, dejected, melancholic, disappointed, hurt, guilty, bored, tired, rejected, nostalgic, wistful, apologetic, hesitant, insecure, confused, resigned, anxious, panicked, alarmed, scared, neutral, proud, confident, distant, skeptical, contemplative, determined

**Nonverbals:** `[laughter]` tag supported in transcript.

**Speed:** 0.6–1.5; **Volume:** 0.5–2.0.

### macOS Audio Conversion

```bash
afconvert -f caff -d aac -b 128000 input.mp3 output.caf
```

**Fallback:** Send MP3 directly (works as iMessage attachment, not native voice memo format).

### iMessage Attachment Sending (Existing)

```applescript
tell application "Messages"
  set targetService to 1st service whose service type = iMessage
  set targetBuddy to buddy "%s" of targetService
  send POSIX file "%s" to targetBuddy
end tell
```

**Existing:** `imessage_send` already supports `media` array with local file paths; sends each via `send POSIX file`.

### Key Files

| File                         | Purpose                                                 |
| ---------------------------- | ------------------------------------------------------- |
| `src/core/http.c`            | `hu_http_post_json_ex`, `hu_http_request` for API calls |
| `src/channels/imessage.c`    | `imessage_send` — media array with local paths          |
| `src/daemon.c`               | Message processing loop, send call                      |
| `src/context/conversation.c` | Emotion detection, energy classifiers                   |
| `include/human/persona.h`  | Persona struct, contact profiles                        |
| `src/persona/persona.c`      | JSON parsing                                            |

### API Key Resolution

- **Config:** Add `providers` entry for `cartesia` with `api_key`, or add `tts.cartesia.api_key` to config schema
- **Env:** `CARTESIA_API_KEY` (fallback, via `getenv`)
- **Secrets:** If `~/.human/secrets.json` exists and exposes `cartesia_api_key`, use it (follow existing provider key resolution in `src/providers/from_config.c`)

---

## Task 1: F34 — Cartesia TTS module (header + implementation)

**Description:** Add a new C module for Cartesia Sonic-3 TTS API. Calls `POST /tts/bytes`, returns raw MP3 bytes.

**Files:**

- Create: `include/human/tts/cartesia.h`
- Create: `src/tts/cartesia.c`
- Modify: `CMakeLists.txt` — add `HU_ENABLE_CARTESIA` option, conditional compilation

**Steps:**

1. **Create `include/human/tts/cartesia.h`:**

   ```c
   #ifndef HU_TTS_CARTESIA_H
   #define HU_TTS_CARTESIA_H

   #include "human/core/allocator.h"
   #include "human/core/error.h"
   #include <stddef.h>
   #include <stdbool.h>

   typedef struct hu_cartesia_tts_config {
       const char *model_id;      /* "sonic-3-2026-01-12" */
       const char *voice_id;      /* cloned voice UUID */
       const char *emotion;       /* "content", "excited", etc. */
       float speed;              /* 0.6–1.5; default 0.95 */
       float volume;             /* 0.5–2.0; default 1.0 */
       bool nonverbals;           /* allow [laughter] etc. */
   } hu_cartesia_tts_config_t;

   /* Synthesize audio from transcript. Returns MP3 bytes; caller owns out. */
   hu_error_t hu_cartesia_tts_synthesize(hu_allocator_t *alloc,
       const char *api_key, size_t api_key_len,
       const char *transcript, size_t transcript_len,
       const hu_cartesia_tts_config_t *config,
       unsigned char **out_bytes, size_t *out_len);

   void hu_cartesia_tts_free_bytes(hu_allocator_t *alloc, unsigned char *bytes, size_t len);

   #endif
   ```

2. **Create `src/tts/cartesia.c`:**
   - Guard entire file with `#if HU_ENABLE_CARTESIA && HU_ENABLE_CURL`
   - Build JSON body: `model_id`, `transcript`, `voice: { mode: "id", id: voice_id }`, `output_format: { container: "mp3", encoding: "mp3", sample_rate: 44100 }`, `generation_config: { speed, emotion }`
   - Headers: `X-API-Key: <key>`, `Cartesia-Version: 2024-06-10`, `Content-Type: application/json`
   - Use `hu_http_post_json_ex` or `hu_http_request` with `POST`; Cartesia returns raw bytes, not JSON — check `Content-Type` of response; if `Content-Type: audio/mpeg`, body is raw MP3
   - **Note:** Cartesia `/tts/bytes` returns raw MP3 bytes (`application/octet-stream` or `audio/mpeg`). Use `hu_http_request` with `POST`, JSON body, and capture response body — `hu_http_response_t.body` holds raw bytes; `body_len` is byte count. No JSON parsing of response.
   - **JSON body:** Escape transcript for JSON (double-quote `"`, backslash `\`, newline `\n`, control chars). Use `hu_json_*` or manual escaping.
   - Error handling: 401 → `HU_ERR_PROVIDER_AUTH`, 429 → `HU_ERR_RATE_LIMIT`, 4xx/5xx → `HU_ERR_HTTP`
   - `HU_IS_TEST`: return mock success (empty or minimal bytes) with no network call

3. **HTTP client for binary response:** `hu_http_response_t` has `body` (char*) and `body_len`. For MP3, `body` is the raw bytes; treat as `unsigned char*`. No need to change HTTP client.

4. **JSON body construction:** Use `snprintf` or `hu_json_*` to build request. Escape transcript for JSON (double-quote, backslash, newline).

**Tests:**

- `tests/test_cartesia.c`: `HU_IS_TEST` path — mock returns success; no network
- Test `hu_cartesia_tts_synthesize` with empty transcript → error
- Test config defaults (speed 0.95, emotion "content")

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors
- Manual: with valid API key, synthesize "Hello" → MP3 bytes

---

## Task 2: CMake integration — HU_ENABLE_CARTESIA

**Description:** Add CMake option `HU_ENABLE_CARTESIA=ON` (default OFF). Only compiles TTS code when enabled. Requires `HU_ENABLE_CURL`.

**Files:**

- Modify: `CMakeLists.txt` — add option, conditional source, conditional include
- Modify: `src/CMakeLists.txt` or `src/tts/CMakeLists.txt` if exists

**Steps:**

1. **Add option** (near other optional modules):

   ```cmake
   option(HU_ENABLE_CARTESIA "Enable Cartesia TTS for voice messages" OFF)
   ```

2. **Add source** when enabled:

   ```cmake
   if(HU_ENABLE_CARTESIA AND HU_ENABLE_CURL)
     list(APPEND HU_SOURCES src/tts/cartesia.c)
     add_compile_definitions(HU_ENABLE_CARTESIA=1)
   endif()
   ```

3. **Create stub when disabled:** If `include/human/tts/cartesia.h` is always included, provide stub that returns `HU_ERR_NOT_SUPPORTED` when `!HU_ENABLE_CARTESIA`. Or: only include header when enabled; callers must `#if HU_ENABLE_CARTESIA`.

4. **Dependency:** `HU_ENABLE_CARTESIA` implies `HU_ENABLE_CURL=ON`; fail configure if Cartesia ON but CURL OFF.

**Tests:**

- Build with `-DSC_ENABLE_CARTESIA=OFF` → no cartesia.c
- Build with `-DSC_ENABLE_CARTESIA=ON -DSC_ENABLE_CURL=ON` → cartesia.c compiled

**Validation:**

- Both builds succeed

---

## Task 3: F35 — Voice clone setup (external)

**Description:** Document steps to create Seth voice clone on Cartesia platform. Store voice UUID in persona JSON.

**Files:**

- Create: `docs/voice-clone-setup.md`
- Modify: `include/human/persona.h` — add `hu_voice_config_t`
- Modify: `src/persona/persona.c` — parse `voice` block

**Steps:**

1. **Create `docs/voice-clone-setup.md`:**
   - Go to Cartesia platform (e.g. app.cartesia.ai)
   - Record 10-second clear voice sample (no background noise)
   - Upload to Cartesia voice cloning
   - Copy voice UUID

2. **Persona JSON `voice` block:**

   ```json
   "voice": {
     "provider": "cartesia",
     "voice_id": "cloned-voice-uuid",
     "model": "sonic-3-2026-01-12",
     "default_emotion": "content",
     "default_speed": 0.95,
     "nonverbals": true
   }
   ```

3. **Add `hu_voice_config_t`** in `persona.h`:

   ```c
   typedef struct hu_voice_config {
       char *provider;       /* "cartesia" */
       char *voice_id;       /* UUID */
       char *model;         /* "sonic-3-2026-01-12" */
       char *default_emotion;
       float default_speed;
       bool nonverbals;
   } hu_voice_config_t;
   ```

4. **Parse in persona.c:** Read `voice` object; populate `hu_voice_config_t`. Defaults: `default_emotion="content"`, `default_speed=0.95`, `nonverbals=true`.

**Tests:**

- Load persona JSON with `voice` block → assert `voice_id`, `model`, `default_emotion` set
- Load without `voice` → NULL or defaults

**Validation:**

- `./build/human_tests` — 0 failures

---

## Task 4: F36 — Audio format pipeline (MP3 → CAF)

**Description:** Convert Cartesia MP3 to `.caf` (Core Audio Format) for iMessage native voice memo. Use `afconvert` on macOS. Fallback: send MP3 directly.

**Files:**

- Create: `include/human/tts/audio_pipeline.h`
- Create: `src/tts/audio_pipeline.c`
- Modify: `CMakeLists.txt` — add when `HU_ENABLE_CARTESIA`

**Steps:**

1. **Create `include/human/tts/audio_pipeline.h`:**

   ```c
   #ifndef HU_TTS_AUDIO_PIPELINE_H
   #define HU_TTS_AUDIO_PIPELINE_H

   #include "human/core/allocator.h"
   #include "human/core/error.h"
   #include <stddef.h>

   /* Write MP3 bytes to temp file, convert to CAF for iMessage.
    * Returns path to .caf file (caller must free; use mkstemp pattern).
    * On failure, returns NULL and err is set. */
   hu_error_t hu_audio_mp3_to_caf(hu_allocator_t *alloc,
       const unsigned char *mp3_bytes, size_t mp3_len,
       char *out_caf_path, size_t out_caf_path_cap);

   /* Clean up temp files. Call when done sending. */
   void hu_audio_cleanup_temp(const char *caf_path);

   #endif
   ```

2. **Implement `hu_audio_mp3_to_caf`:**
   - `mkstemp` for `/tmp/human-voice-XXXXXX.mp3`
   - Write `mp3_bytes` to file
   - `afconvert -f caff -d aac -b 128000 input.mp3 output.caf`
   - Use `hu_process_run` or `popen` to run `afconvert`
   - `mkstemp` for output: `/tmp/human-voice-XXXXXX.caf`
   - On success: copy `output.caf` path to `out_caf_path`
   - On `afconvert` failure: fallback — copy `input.mp3` path to `out_caf_path` (caller will send MP3; rename param to `out_audio_path` if we want to support both)
   - Simpler: always return .caf path; if afconvert fails, return .mp3 path and document that iMessage accepts MP3 too

3. **Implementation detail:**

   ```c
   /* 1. mkstemp /tmp/human-voice-XXXXXX.mp3 */
   /* 2. write mp3_bytes */
   /* 3. close fd */
   /* 4. snprintf caf_path from mp3_path, replace .mp3 with .caf */
   /* 5. hu_process_run: afconvert -f caff -d aac -b 128000 mp3_path caf_path */
   /* 6. if success: out_caf_path = caf_path; else: out_caf_path = mp3_path (fallback) */
   ```

4. **Platform:** Only on `__APPLE__`; otherwise return `HU_ERR_NOT_SUPPORTED`.

5. **`hu_audio_cleanup_temp`:** `unlink(caf_path)` and `unlink(mp3_path)` if we kept mp3_path. Store both paths in a struct or pass mp3_path too.

6. **`HU_IS_TEST`:** Skip `afconvert`; write mock file, return path; test cleanup.

**Tests:**

- `hu_audio_mp3_to_caf` with mock MP3 bytes → file created (or mock path in HU_IS_TEST)
- Cleanup removes temp files

**Validation:**

- `./build/human_tests` — 0 failures
- Manual: real MP3 → CAF → send via iMessage

---

## Task 5: F37 — Voice message decision engine

**Description:** Classifier decides when to send voice vs text. Per-contact config: `voice_messages.enabled`, `frequency`, `prefer_for`, `never_for`, `max_duration_sec`.

**Files:**

- Create: `include/human/context/voice_decision.h`
- Create: `src/context/voice_decision.c`
- Modify: `include/human/persona.h` — add `hu_voice_messages_config_t`
- Modify: `src/persona/persona.c` — parse `voice_messages` in contact profile
- Modify: `CMakeLists.txt` — add voice_decision.c (when HU_ENABLE_CARTESIA or always)

**Steps:**

1. **Add `hu_voice_messages_config_t`** in persona.h or voice_decision.h:

   ```c
   typedef struct hu_voice_messages_config {
       bool enabled;
       char *frequency;           /* "rare", "occasional", "frequent" */
       char **prefer_for;         /* ["emotional", "late_night", "long_response", "comfort"] */
       size_t prefer_for_count;
       char **never_for;           /* ["questions", "logistics", "quick_ack"] */
       size_t never_for_count;
       uint32_t max_duration_sec;  /* 30 */
   } hu_voice_messages_config_t;
   ```

2. **Add to contact profile:** `hu_contact_profile_t` gains `hu_voice_messages_config_t *voice_messages` (nullable).

3. **Implement classifier** in `voice_decision.c`:

   ```c
   typedef enum hu_voice_decision {
       HU_VOICE_SEND_TEXT,
       HU_VOICE_SEND_VOICE,
   } hu_voice_decision_t;

   hu_voice_decision_t hu_voice_decision_classify(
       const char *response_text, size_t response_len,
       const hu_channel_history_entry_t *entries, size_t entry_count,
       const hu_contact_profile_t *contact,
       const hu_voice_config_t *voice_config,
       int hour_local,  /* 0-23 */
       uint32_t seed);
   ```

4. **Classifier logic:**
   - If `!voice_config` or `!voice_config->voice_id` → return `HU_VOICE_SEND_TEXT`
   - If `!contact->voice_messages` or `!contact->voice_messages->enabled` → `HU_VOICE_SEND_TEXT`
   - **Never_for:** If last incoming message is question (ends with `?`) → `HU_VOICE_SEND_TEXT`
   - **Never_for:** If response is quick ack (< 20 chars, "ok", "sure", "yeah") → `HU_VOICE_SEND_TEXT`
   - **Never_for:** If logistics (keywords: "what time", "where", "when", "address") → `HU_VOICE_SEND_TEXT`
   - **Prefer_for:** If `emotional` (from `hu_conversation_detect_emotion` — high intensity) → `HU_VOICE_SEND_VOICE` (if frequency roll passes)
   - **Prefer_for:** If `late_night` (hour 22–6) and close contact → `HU_VOICE_SEND_VOICE`
   - **Prefer_for:** If `long_response` (response_len > 150) → `HU_VOICE_SEND_VOICE`
   - **Prefer_for:** If `comfort` (they expressed sadness, we're comforting) → `HU_VOICE_SEND_VOICE`
   - **Prefer_for:** If they sent voice memo recently (last entry has `from_me=false` and we could add `was_voice` flag — Phase 5 may not have this; skip for now) → mirror
   - **Frequency:** `rare` = 5% probability; `occasional` = 15%; `frequent` = 30%
   - **max_duration_sec:** If estimated duration > max (e.g. ~150 chars/30 sec), truncate or send text

5. **Seed:** Use for probabilistic roll when frequency applies.

**Tests:**

- Question from them → `HU_VOICE_SEND_TEXT`
- Short "ok" response → `HU_VOICE_SEND_TEXT`
- Long emotional response (150+ chars), `prefer_for` includes emotional → `HU_VOICE_SEND_VOICE` (with frequency roll)
- Late night (23:00), contact enabled → `HU_VOICE_SEND_VOICE` (with roll)

**Validation:**

- `./build/human_tests` — 0 failures

---

## Task 6: F38 — Emotion mapping from conversation context

**Description:** Map conversation context (emotion, energy, topic) to Cartesia's 60+ emotions. Use `hu_conversation_detect_emotion`, `hu_conversation_detect_energy`, and context modifiers.

**Files:**

- Modify: `src/tts/cartesia.c` or create `src/tts/emotion_map.c`
- Modify: `include/human/tts/cartesia.h` — add `hu_cartesia_emotion_from_context`

**Steps:**

1. **Add enum and mapping function:**

   ```c
   /* In cartesia.h or emotion_map.h */
   const char *hu_cartesia_emotion_from_context(
       const hu_emotional_state_t *emo,
       hu_energy_level_t energy,
       const hu_channel_history_entry_t *entries, size_t count);
   ```

2. **Mapping logic:**

   | Context                                     | Cartesia emotion                |
   | ------------------------------------------- | ------------------------------- |
   | Comforting (high negative valence, empathy) | sympathetic, affectionate       |
   | Congratulating (positive valence)           | excited, enthusiastic           |
   | Late night casual                           | content, calm                   |
   | Playful / teasing                           | joking/comedic, flirtatious     |
   | Serious / heavy topic                       | contemplative, determined       |
   | Sad / down                                  | sympathetic, sad                |
   | Anxious / worried                           | calm, reassuring (we calm them) |
   | Default                                     | content                         |

3. **Use primary emotions when possible:** neutral, angry, excited, content, sad, scared — best quality

4. **Return:** `const char*` to static string or emotion table lookup.

**Tests:**

- High negative emotion → `sympathetic` or `affectionate`
- HU_ENERGY_EXCITED → `excited`
- HU_ENERGY_NEUTRAL, default → `content`

**Validation:**

- `./build/human_tests` — 0 failures

---

## Task 7: F39 — Nonverbal sound injection

**Description:** Inject `[laughter]`, "Hmm...", pauses before TTS. Configurable via `voice.nonverbals`. Max 1 per message.

**Files:**

- Modify: `src/context/conversation.c` or create `src/tts/nonverbals.c`
- Modify: `include/human/context/conversation.h` or tts header

**Steps:**

1. **Add function:**

   ```c
   size_t hu_conversation_inject_nonverbals(char *buf, size_t len, size_t cap,
       uint32_t seed, bool enabled, const hu_contact_profile_t *contact);
   ```

2. **Logic:**
   - If `!enabled` → return `len` unchanged
   - Roll with ~15% probability (seed)
   - If pass:
     - 50%: `[laughter]` — insert after first sentence or before "lol"/"haha"
     - 30%: "Hmm... " — prepend at start
     - 20%: "..." — insert after first clause (before second sentence)
   - Ensure buffer has capacity; truncate if needed
   - Max 1 insertion per message

3. **Call site:** After LLM response, before TTS. If voice decision is voice, call `hu_conversation_inject_nonverbals` on the transcript.

**Tests:**

- `enabled` false → no change
- `enabled` true, seed 0 → one of [laughter], Hmm..., or ... inserted
- Buffer capacity edge case

**Validation:**

- `./build/human_tests` — 0 failures

---

## Task 8: iMessage voice message sending

**Description:** When voice decision is HU_VOICE_SEND_VOICE, synthesize audio, convert to CAF, send via iMessage as attachment. Use existing `imessage_send` with media array.

**Files:**

- Modify: `src/daemon.c` — voice decision branch, TTS call, media send
- Modify: `src/channels/imessage.c` — extend media send to support voice-only (empty message + media)

**Steps:**

1. **Daemon flow:**
   - After LLM response, before `ch->channel->vtable->send`:
     - If `hu_voice_decision_classify` returns `HU_VOICE_SEND_VOICE`:
       - Get API key: `hu_config_get_provider_key(cfg, "cartesia")` or add `hu_config_get_tts_key`
       - Get voice config from persona
       - Call `hu_cartesia_tts_synthesize` with response text (after nonverbal injection)
       - Call `hu_audio_mp3_to_caf` to get audio path
       - Call `ch->channel->vtable->send(ctx, target, len, "", 0, &audio_path, 1)` — empty message, media = path
       - Call `hu_audio_cleanup_temp(audio_path)`
     - Else: normal text send

2. **iMessage send with empty message + media:**
   - Current `imessage_send` sends text first, then media. For voice-only: send empty string `""` and media array with path. Check imessage.c: if `message_len == 0` and `media_count > 0`, skip text send, send media only.

3. **Modify imessage_send:** When `message_len == 0` and `media_count > 0`, skip the text AppleScript block entirely; go straight to media loop. This avoids sending an empty bubble. Voice-only = media-only send.

**Tests:**

- HU_IS_TEST: voice path records that media was sent (extend mock to capture last_media)
- Manual: voice message appears in iMessage

**Validation:**

- Voice message sent as attachment
- Temp files cleaned up

---

## Task 9: Daemon integration — full flow

**Description:** Wire voice decision, TTS, audio pipeline, and send into the daemon message processing loop. Ensure correct placement relative to tapback, correction, and fragmentation.

**Files:**

- Modify: `src/daemon.c` — main send block (~line 3500)

**Steps:**

1. **Locate send block:** After `hu_agent_turn` returns response, before `ch->channel->vtable->send`.

2. **Insert voice decision:**

   ```c
   bool sent_voice = false;
   #if HU_ENABLE_CARTESIA
   hu_voice_decision_t voice_dec = hu_voice_decision_classify(
       response, response_len, entries, entry_count, contact, voice_config, hour_local, seed);
   if (voice_dec == HU_VOICE_SEND_VOICE && voice_config && voice_config->voice_id && api_key) {
       hu_cartesia_tts_config_t tts_cfg = { .model_id = "sonic-3-2026-01-12", .voice_id = voice_config->voice_id, ... };
       unsigned char *mp3 = NULL;
       size_t mp3_len = 0;
       hu_error_t tts_err = hu_cartesia_tts_synthesize(alloc, api_key, api_key_len, response, response_len, &tts_cfg, &mp3, &mp3_len);
       if (tts_err == HU_OK && mp3 && mp3_len > 0) {
           char audio_path[256];
           hu_error_t pipe_err = hu_audio_mp3_to_caf(alloc, mp3, mp3_len, audio_path, sizeof(audio_path));
           hu_cartesia_tts_free_bytes(alloc, mp3, mp3_len);
           if (pipe_err == HU_OK) {
               const char *media_paths[] = { audio_path };
               ch->channel->vtable->send(..., "", 0, media_paths, 1);
               hu_audio_cleanup_temp(audio_path);
               sent_voice = true;
           }
       }
   }
   #endif
   if (!sent_voice)
       ch->channel->vtable->send(..., response, response_len, NULL, 0);
   ```

3. **Flow:**
   - Voice path: if TTS + pipeline succeed, send voice only; set `sent_voice = true`
   - If `!sent_voice`: send text (normal path or fallback)
   - Never send both voice and text for the same response

4. **Get contact, voice_config, hour_local:** Pass through daemon context. `contact` from `hu_persona_get_contact_for_id`. `voice_config` from persona root. `hour_local` from `localtime`.

5. **API key:** `hu_config_get_provider_key(cfg, "cartesia")` or add config key for TTS. Add to config schema: `tts.cartesia.api_key` or `providers.cartesia.api_key`.

6. **Nonverbal injection:** When voice decision is voice, call `hu_conversation_inject_nonverbals` on the response text _before_ passing to TTS. Use a mutable copy; ensure buffer capacity.

7. **Emotion mapping:** Call `hu_cartesia_emotion_from_context(emo, energy, entries, count)` to get Cartesia emotion string. Set `tts_cfg.emotion` before `hu_cartesia_tts_synthesize`. Use `hu_conversation_detect_emotion` and `hu_conversation_detect_energy` (from Phase 2) if available.

**Tests:**

- HU_IS_TEST: when voice decision is voice, mock TTS returns bytes, mock send receives media
- Fallback: TTS fails → text sent

**Validation:**

- Daemon sends voice when appropriate
- No double-send (voice + text)

---

## Task 10: Testing strategy

**Description:** Comprehensive tests for mock Cartesia API, audio pipeline, decision engine, and voice path.

**Files:**

- Create: `tests/test_cartesia.c`
- Create: `tests/test_audio_pipeline.c`
- Create: `tests/test_voice_decision.c`
- Modify: `tests/CMakeLists.txt`

**Steps:**

1. **test_cartesia.c:**
   - `HU_IS_TEST`: `hu_cartesia_tts_synthesize` returns mock bytes (no network)
   - `hu_cartesia_tts_synthesize` with NULL api_key → error
   - `hu_cartesia_tts_synthesize` with empty transcript → error
   - Config validation

2. **test_audio_pipeline.c:**
   - `hu_audio_mp3_to_caf` with mock MP3 bytes → file created (or mock in HU_IS_TEST)
   - `hu_audio_cleanup_temp` removes file
   - `HU_IS_TEST`: skip `afconvert`, use mock

3. **test_voice_decision.c:**
   - Question → HU_VOICE_SEND_TEXT
   - Quick ack → HU_VOICE_SEND_TEXT
   - Long emotional response → HU_VOICE_SEND_VOICE (with frequency)
   - Disabled contact → HU_VOICE_SEND_TEXT
   - No voice config → HU_VOICE_SEND_TEXT

4. **Daemon integration test:**
   - Add to `test_daemon.c` or `test_channel_all.c`: when voice path triggered, verify send receives media array (mock channel)

**Validation:**

- `./build/human_tests` — 0 failures, 0 ASan errors

---

## Implementation Order

Recommended sequence:

1. **Task 2** (CMake) — enables build
2. **Task 1** (Cartesia TTS) — core API
3. **Task 3** (Voice config) — persona parsing
4. **Task 4** (Audio pipeline) — MP3 → CAF
5. **Task 5** (Voice decision) — classifier
6. **Task 6** (Emotion mapping) — emotion from context
7. **Task 7** (Nonverbals) — injection
8. **Task 8** (iMessage voice) — media-only send
9. **Task 9** (Daemon integration) — full flow
10. **Task 10** (Tests) — throughout

---

## Validation Matrix

| Check          | Command / Action                                                                                                           |
| -------------- | -------------------------------------------------------------------------------------------------------------------------- |
| Build          | `cmake -B build -DSC_ENABLE_CARTESIA=ON -DSC_ENABLE_CURL=ON -DSC_ENABLE_ALL_CHANNELS=ON && cmake --build build -j$(nproc)` |
| Tests          | `./build/human_tests` — 0 failures, 0 ASan errors                                                                        |
| Voice TTS      | Manual: synthesize "Hello" → MP3 bytes                                                                                     |
| Audio pipeline | Manual: MP3 → CAF → send via iMessage                                                                                      |
| Voice decision | Unit tests for classifier                                                                                                  |
| Daemon         | Manual: run daemon, send emotional message → voice memo                                                                    |

---

## Risk Notes

- **Cartesia API changes:** Model ID and endpoint may change; document version pinning.
- **afconvert availability:** macOS only; Linux/Windows fallback to MP3.
- **Voice clone quality:** External step; test extensively before enabling.
- **Binary size:** `HU_ENABLE_CARTESIA=OFF` by default keeps size minimal.
