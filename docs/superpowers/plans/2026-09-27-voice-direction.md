---
title: Directed voice memos — implementation plan
date: 2026-09-27
status: draft — awaiting Seth's review
---

# Directed Voice Memos Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Voice memos that the language model performs as Seth (scene-cast, self-directed with Cartesia's full palette, safety-validated), and inbound voice memos the daemon can actually hear.

**Architecture:** Inbound: read iOS's own `IMAudioTranscription` from chat.db first, local Whisper second, cloud last. Outbound: D1 (one LLM call casts the model as Seth in a scene and returns a tagged line) → D2 (pure C parser/validator: palette, clamps, budgets) → S3 drift + S4 safety gates on the words → D3 (canonical Cartesia tags re-rendered from parsed values, text normalized per segment). `HU_SPEECH_DIRECTION=off|shadow|live`, default off; OFF is today's behavior exactly.

**Tech Stack:** C11 (h-uman daemon, `-Werror`), SQLite (chat.db, read-only), Cartesia sonic-3.6, Python 3 stdlib + `mlx_whisper` for the local STT server.

**Spec:** `docs/superpowers/specs/2026-09-27-voice-direction-design.md` (approved 2026-09-27; decisions: local model performs; Mindy rates W5).

## Global Constraints

- Default for every new switch is OFF; OFF must be byte-identical to today's behavior.
- Palette only: `<emotion value>` (Cartesia's documented list), `<speed ratio>`, `<volume ratio>`, `<break time>`, `[laughter]`. Anything else in a model line is rejected, never spoken.
- Clamp speed to 0.85–1.10, volume to 0.85–1.15, breaks to ≤ 800 ms.
- Budgets: emotion changes ≤ 3 per memo and ≤ max(1, ⌈sentences/2⌉); ≤ 1 laugh; ≤ 1 speed tag; ≤ 1 volume tag.
- The performance may change delivery, never content: S3 drift guard + S4 outbound safety gates run on the tag-stripped words.
- D1 uses the daemon's own reply provider (`agent->provider`, the local model). No new network egress.
- Logs never contain message text: counts, verdicts and reason names only.
- `HU_IS_TEST` guards on every network, process or file side effect; tests use mocks.
- Every new public `hu_*` function has a production caller or a test reference by its task's commit (dead-strip ratchet B); if the clone ratchet refuses, reuse the existing code it matched rather than raising the ceiling.
- Conventional commits; commit from the worktree `/Users/sethford/Projects/h-uman/.claude/worktrees/voice-memo-realism`; nothing is pushed or deployed by this plan (see Human steps).

## Review Focus

- A model line with an unclosed tag or a tag inside a word → rejected, never spoken as literal markup (Task 3: `test_direction_rejects_unclosed_tag`).
- Numbers inside tag attributes are never normalized into words, while numbers in the spoken words are (Task 4: `test_direction_render_normalizes_words_not_tags`).
- A truncated `attributedBody`, or one without the end marker after the transcript, yields no transcript rather than garbage (Task 1: `test_audio_transcription_requires_end_marker`).
- An inbound memo that already carries iOS's transcript is not transcribed again, even when local STT is configured (Task 2: `test_inbound_audio_route_skips_when_transcript_present`).
- A directed line whose words fail the safety gates is not spoken; the undirected cleaned text is (Task 6: `test_voice_reply_directed_line_that_trips_moderation_is_not_spoken`).

---

### Task 1: Hear iOS's own transcript of an inbound voice memo

**Files:**
- Modify: `include/human/channels/imessage.h` (next to `hu_imessage_extract_attributed_body`, line ~224)
- Modify: `src/util/typedstream.c` (new function after `hu_imessage_extract_attributed_body`, line ~466)
- Modify: `src/channels/imessage.c:5911-5920` (poll: after the attributedBody extraction)
- Test: `tests/test_typedstream.c` (+ register in its `run_typedstream_tests`, line ~314)

**Interfaces:**
- Produces: `size_t hu_imessage_extract_audio_transcription(const unsigned char *blob, size_t blob_len, char *out, size_t cap);` — copies the `IMAudioTranscription` value (UTF-8, NUL-terminated, never split mid-character), returns its length or 0.

Framing observed in chat.db on 2026-09-27 (both audio messages that carry the key): key, then `86 92 84 96 96`, then a typedstream length (`0x81` + u16 LE for these; one byte when < 0x80; `0x82` + u32 LE), the UTF-8 bytes, then `0x86` (end of object). Requiring that end marker is what tells a real length from a stray byte.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_typedstream.c` before `run_typedstream_tests`:

```c
/* Framing observed in chat.db 2026-09-27: key, 86 92 84 96 96, a typedstream
 * length, the UTF-8 text, then 0x86. */
static size_t build_audio_blob(unsigned char *b, const char *text, bool with_end) {
    static const unsigned char head[] = {0x04, 0x0b, 's', 't', 'r', 'e', 'a', 'm', 't',
                                         'y',  'p',  'e', 'd', 0x81, 0xe8, 0x03};
    static const unsigned char pre[] = {0x86, 0x92, 0x84, 0x96, 0x96};
    static const char key[] = "IMAudioTranscription";
    size_t o = 0, n = strlen(text);
    memcpy(b + o, head, sizeof(head));
    o += sizeof(head);
    memcpy(b + o, key, sizeof(key) - 1);
    o += sizeof(key) - 1;
    memcpy(b + o, pre, sizeof(pre));
    o += sizeof(pre);
    if (n < 0x80) {
        b[o++] = (unsigned char)n;
    } else {
        b[o++] = 0x81;
        b[o++] = (unsigned char)(n & 0xff);
        b[o++] = (unsigned char)(n >> 8);
    }
    memcpy(b + o, text, n);
    o += n;
    if (with_end)
        b[o++] = 0x86;
    b[o++] = 0x92;
    b[o++] = 0x84;
    return o;
}

static void test_audio_transcription_extracts_the_ios_transcript(void) {
    unsigned char b[1024];
    size_t n = build_audio_blob(b, "OK, see you at church", true);
    char out[256];
    HU_ASSERT_EQ(hu_imessage_extract_audio_transcription(b, n, out, sizeof(out)), 21);
    HU_ASSERT_STR_EQ(out, "OK, see you at church");
}

static void test_audio_transcription_long_length_prefix(void) {
    char text[301];
    memset(text, 'a', 300);
    text[300] = '\0';
    unsigned char b[1024];
    size_t n = build_audio_blob(b, text, true);
    char out[512];
    HU_ASSERT_EQ(hu_imessage_extract_audio_transcription(b, n, out, sizeof(out)), 300);
    HU_ASSERT_STR_EQ(out, text);
}

static void test_audio_transcription_absent_key_returns_zero(void) {
    static const unsigned char b[] = {0x04, 0x0b, 'N', 'S', 'S', 't', 'r', 'i', 'n', 'g', 0x86};
    char out[64] = "x";
    HU_ASSERT_EQ(hu_imessage_extract_audio_transcription(b, sizeof(b), out, sizeof(out)), 0);
    HU_ASSERT_STR_EQ(out, "");
}

static void test_audio_transcription_requires_end_marker(void) {
    unsigned char b[1024];
    size_t n = build_audio_blob(b, "no end marker here", false);
    char out[256];
    HU_ASSERT_EQ(hu_imessage_extract_audio_transcription(b, n, out, sizeof(out)), 0);
    /* truncated blob: the length points past the end */
    n = build_audio_blob(b, "cut short", true);
    HU_ASSERT_EQ(hu_imessage_extract_audio_transcription(b, n - 6, out, sizeof(out)), 0);
}

static void test_audio_transcription_truncates_on_a_character_boundary(void) {
    unsigned char b[1024];
    size_t n = build_audio_blob(b, "caf\xC3\xA9 later", true); /* "café later" */
    char out[5];                                                /* room for "caf" + 1 byte */
    HU_ASSERT_EQ(hu_imessage_extract_audio_transcription(b, n, out, sizeof(out)), 3);
    HU_ASSERT_STR_EQ(out, "caf");
}
```

Register in `run_typedstream_tests`:

```c
    HU_RUN_TEST(test_audio_transcription_extracts_the_ios_transcript);
    HU_RUN_TEST(test_audio_transcription_long_length_prefix);
    HU_RUN_TEST(test_audio_transcription_absent_key_returns_zero);
    HU_RUN_TEST(test_audio_transcription_requires_end_marker);
    HU_RUN_TEST(test_audio_transcription_truncates_on_a_character_boundary);
```

Add the prototype to `include/human/channels/imessage.h` after `hu_imessage_extract_attributed_body`:

```c
/* Messages stores iOS's transcript of an audio message as the
 * IMAudioTranscription attribute of attributedBody. Copies it (UTF-8,
 * NUL-terminated, never split mid-character) and returns its length, or 0. */
size_t hu_imessage_extract_audio_transcription(const unsigned char *blob, size_t blob_len,
                                               char *out, size_t cap);
```

- [ ] **Step 2: Run to verify RED**

Run: `cmake --build build --target human_tests -j10 2>&1 | grep -E " error" ; ./build/human_tests --filter=audio_transcription`
Expected: link error `undefined symbol: _hu_imessage_extract_audio_transcription` (then, once a `return 0;` stub is added, 4 of 5 FAIL).

- [ ] **Step 3: Implement** in `src/util/typedstream.c` (reuses the file's `find_substring`):

```c
/* Typedstream length prefix: one byte (< 0x80), 0x81 + u16 LE, or 0x82 + u32
 * LE. Returns the header size, 0 when `at` is not a length. */
static size_t read_ts_length(const unsigned char *b, size_t n, size_t at, size_t *len_out) {
    if (at >= n)
        return 0;
    unsigned char c = b[at];
    if (c > 0 && c < 0x80) {
        *len_out = c;
        return 1;
    }
    if (c == 0x81 && at + 3 <= n) {
        *len_out = (size_t)b[at + 1] | ((size_t)b[at + 2] << 8);
        return 3;
    }
    if (c == 0x82 && at + 5 <= n) {
        *len_out = (size_t)b[at + 1] | ((size_t)b[at + 2] << 8) | ((size_t)b[at + 3] << 16) |
                   ((size_t)b[at + 4] << 24);
        return 5;
    }
    return 0;
}

size_t hu_imessage_extract_audio_transcription(const unsigned char *blob, size_t blob_len,
                                               char *out, size_t cap) {
    static const char key[] = "IMAudioTranscription";
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!blob || cap < 2)
        return 0;
    size_t k = find_substring(blob, blob_len, 0, key);
    if (k == SIZE_MAX)
        return 0;
    size_t from = k + sizeof(key) - 1;
    for (size_t o = from; o < from + 16 && o < blob_len; o++) {
        size_t len = 0, h = read_ts_length(blob, blob_len, o, &len);
        if (h == 0 || len == 0)
            continue;
        size_t s = o + h;
        if (s + len >= blob_len || blob[s + len] != 0x86)
            continue; /* not a real length: the value must end at the object marker */
        size_t n = len < cap - 1 ? len : cap - 1;
        while (n > 0 && n < len && (blob[s + n] & 0xC0) == 0x80)
            n--; /* never split a UTF-8 character */
        memcpy(out, blob + s, n);
        out[n] = '\0';
        return n;
    }
    return 0;
}
```

- [ ] **Step 4: Run to verify GREEN**

Run: `./build/human_tests --filter=audio_transcription`
Expected: `5/5 passed`.

- [ ] **Step 5: Wire into the poll** — `src/channels/imessage.c`, immediately after the attributedBody block that ends at line ~5920 (`text = attr_text_buf; } }`), insert:

```c
        /* A voice memo: Messages keeps iOS's transcript of it in attributedBody
         * (IMAudioTranscription). Reply to what they said, not to "[Audio]" —
         * 2026-09-27 the daemon answered Mindy's memo without hearing it. */
        char audio_text_buf[4200];
        if (has_audio) {
            const unsigned char *ab = sqlite3_column_blob(stmt, 10);
            int abl = sqlite3_column_bytes(stmt, 10);
            const char pre[] = "[Audio transcription: ";
            size_t pl = sizeof(pre) - 1;
            memcpy(audio_text_buf, pre, pl);
            size_t tn = (ab && abl > 0) ? hu_imessage_extract_audio_transcription(
                                              ab, (size_t)abl, audio_text_buf + pl,
                                              sizeof(audio_text_buf) - pl - 1)
                                        : 0;
            if (tn > 0) {
                audio_text_buf[pl + tn] = ']';
                audio_text_buf[pl + tn + 1] = '\0';
                text = audio_text_buf;
            }
        }
```

Verify the caller: `grep -rn "hu_imessage_extract_audio_transcription" src | grep -v typedstream.c` → one hit in `src/channels/imessage.c`.

- [ ] **Step 6: Full suite, then commit**

Run: `touch src/channels/imessage.c src/util/typedstream.c && cmake --build build --target human human_tests -j10 && ./build/human_tests 2>&1 | grep Results`
Expected: all pass.

```bash
git add include/human/channels/imessage.h src/util/typedstream.c src/channels/imessage.c tests/test_typedstream.c
git commit -m "feat(imessage): hear inbound voice memos via iOS's own transcript"
```

---

### Task 2: Local Whisper fallback for inbound audio

**Files:**
- Create: `include/human/daemon/inbound_audio.h`, `src/daemon/daemon_inbound_audio.c`
- Create: `scripts/stt/mlx_whisper_server.py`, `scripts/stt/test_mlx_whisper_server.py`
- Modify: `src/daemon.c` (the `is_audio || is_video` branch, line ~2913), `CMakeLists.txt` (add `src/daemon/daemon_inbound_audio.c` after `src/daemon/daemon_voice_reply.c` at line ~1280; add `tests/test_daemon_inbound_audio.c` after `tests/test_daemon_voice_reply.c` at line ~4262), `tests/test_main.c`
- Test: `tests/test_daemon_inbound_audio.c`

**Interfaces:**
- Consumes: `hu_local_stt_transcribe` (`include/human/voice/local_stt.h`; returns `"Hello world"` under `HU_IS_TEST`), `hu_multimodal_route_local_media` (`include/human/multimodal.h`), `config->voice.local_stt_endpoint|stt_model|stt_language`.
- Produces: `hu_inbound_audio_route_t hu_inbound_audio_route(const char *content, size_t content_len, const char *local_stt_endpoint);` and `hu_error_t hu_inbound_audio_transcribe(hu_allocator_t *alloc, const hu_config_t *config, hu_provider_t *provider, const char *model, size_t model_len, const char *path, size_t path_len, const char *content, size_t content_len, char **out, size_t *out_len);` (`HU_OK` with `*out == NULL` = nothing to add).

- [ ] **Step 1: Write the failing tests** — `tests/test_daemon_inbound_audio.c`:

```c
/* Inbound voice memos: iOS transcript first, local Whisper second, cloud last. */
#include "human/daemon/inbound_audio.h"
#include "test_framework.h"

#include <string.h>

static void test_inbound_audio_route_skips_when_transcript_present(void) {
    const char *c = "[Audio transcription: see you at church]";
    HU_ASSERT_EQ(hu_inbound_audio_route(c, strlen(c), "http://127.0.0.1:8761/v1/audio/transcriptions"),
                 HU_INBOUND_AUDIO_SKIP);
}

static void test_inbound_audio_route_prefers_local_endpoint(void) {
    HU_ASSERT_EQ(hu_inbound_audio_route("[Audio]", 7, "http://127.0.0.1:8761/v1/audio/transcriptions"),
                 HU_INBOUND_AUDIO_LOCAL);
    HU_ASSERT_EQ(hu_inbound_audio_route("[Audio]", 7, NULL), HU_INBOUND_AUDIO_CLOUD);
    HU_ASSERT_EQ(hu_inbound_audio_route("[Audio]", 7, ""), HU_INBOUND_AUDIO_CLOUD);
}

static void test_inbound_audio_transcribe_uses_local_stt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static hu_config_t config;
    memset(&config, 0, sizeof(config));
    config.voice.local_stt_endpoint = "http://127.0.0.1:8761/v1/audio/transcriptions";
    char *out = NULL;
    size_t n = 0;
    const char *p = "/tmp/Audio Message.caf";
    HU_ASSERT_EQ(hu_inbound_audio_transcribe(&alloc, &config, NULL, NULL, 0, p, strlen(p),
                                             "[Audio]", 7, &out, &n),
                 HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "Hello world"); /* local_stt's HU_IS_TEST reply */
    alloc.free(alloc.ctx, out, n + 1);
}

static void test_inbound_audio_transcribe_adds_nothing_when_transcript_present(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static hu_config_t config;
    memset(&config, 0, sizeof(config));
    config.voice.local_stt_endpoint = "http://127.0.0.1:8761/v1/audio/transcriptions";
    char *out = (char *)1;
    size_t n = 99;
    const char *c = "[Audio transcription: hi]";
    HU_ASSERT_EQ(hu_inbound_audio_transcribe(&alloc, &config, NULL, NULL, 0, "/tmp/a.caf", 10, c,
                                             strlen(c), &out, &n),
                 HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(n, 0);
}

void run_daemon_inbound_audio_tests(void) {
    HU_TEST_SUITE("daemon inbound audio");
    HU_RUN_TEST(test_inbound_audio_route_skips_when_transcript_present);
    HU_RUN_TEST(test_inbound_audio_route_prefers_local_endpoint);
    HU_RUN_TEST(test_inbound_audio_transcribe_uses_local_stt);
    HU_RUN_TEST(test_inbound_audio_transcribe_adds_nothing_when_transcript_present);
}
```

Register in `tests/test_main.c` (declaration next to `run_daemon_voice_reply_tests`, call after it): `void run_daemon_inbound_audio_tests(void);` / `run_daemon_inbound_audio_tests();`. Add both new files to `CMakeLists.txt` as listed above.

- [ ] **Step 2: RED** — Run: `cmake --build build --target human_tests -j10 2>&1 | grep -E " error" | head -3`. Expected: `'human/daemon/inbound_audio.h' file not found`.

- [ ] **Step 3: Implement** — `include/human/daemon/inbound_audio.h`:

```c
#ifndef HU_DAEMON_INBOUND_AUDIO_H
#define HU_DAEMON_INBOUND_AUDIO_H
/* Inbound voice memos (spec 2026-09-27 voice direction): the reply should hear
 * what was said. iOS's own transcript (already in the text) wins; then a
 * local Whisper endpoint (voice.local_stt_endpoint, private); then the
 * existing cloud multimodal route. */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stddef.h>

typedef enum {
    HU_INBOUND_AUDIO_SKIP = 0, /* the text already carries a transcript */
    HU_INBOUND_AUDIO_LOCAL,    /* on this Mac */
    HU_INBOUND_AUDIO_CLOUD,    /* the existing multimodal route */
} hu_inbound_audio_route_t;

hu_inbound_audio_route_t hu_inbound_audio_route(const char *content, size_t content_len,
                                                const char *local_stt_endpoint);

/* HU_OK with *out == NULL: nothing to add. Caller frees *out (len + 1). */
hu_error_t hu_inbound_audio_transcribe(hu_allocator_t *alloc, const hu_config_t *config,
                                       hu_provider_t *provider, const char *model,
                                       size_t model_len, const char *path, size_t path_len,
                                       const char *content, size_t content_len, char **out,
                                       size_t *out_len);
#endif
```

`src/daemon/daemon_inbound_audio.c`:

```c
/* Inbound voice memos; see include/human/daemon/inbound_audio.h. */
#include "human/daemon/inbound_audio.h"
#include "human/multimodal.h"
#include "human/voice/local_stt.h"

#include <string.h>

hu_inbound_audio_route_t hu_inbound_audio_route(const char *content, size_t content_len,
                                                const char *local_stt_endpoint) {
    static const char tag[] = "[Audio transcription:";
    size_t tl = sizeof(tag) - 1;
    for (size_t i = 0; content && i + tl <= content_len; i++)
        if (memcmp(content + i, tag, tl) == 0)
            return HU_INBOUND_AUDIO_SKIP;
    if (local_stt_endpoint && local_stt_endpoint[0])
        return HU_INBOUND_AUDIO_LOCAL;
    return HU_INBOUND_AUDIO_CLOUD;
}

hu_error_t hu_inbound_audio_transcribe(hu_allocator_t *alloc, const hu_config_t *config,
                                       hu_provider_t *provider, const char *model,
                                       size_t model_len, const char *path, size_t path_len,
                                       const char *content, size_t content_len, char **out,
                                       size_t *out_len) {
    if (!alloc || !path || path_len == 0 || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    const char *ep = config ? config->voice.local_stt_endpoint : NULL;
    hu_inbound_audio_route_t r = hu_inbound_audio_route(content, content_len, ep);
    if (r == HU_INBOUND_AUDIO_SKIP)
        return HU_OK;
    if (r == HU_INBOUND_AUDIO_LOCAL) {
        char p[1024];
        if (path_len >= sizeof(p))
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(p, path, path_len);
        p[path_len] = '\0';
        hu_local_stt_config_t lc = {.endpoint = ep,
                                    .model = config->voice.stt_model,
                                    .language = config->voice.stt_language};
        if (hu_local_stt_transcribe(alloc, &lc, p, out, out_len) == HU_OK && *out)
            return HU_OK;
        /* local server down: fall through to the cloud route */
    }
    if (!provider)
        return HU_ERR_NOT_SUPPORTED;
    return hu_multimodal_route_local_media(alloc, path, path_len, provider, model, model_len, out,
                                           out_len);
}
```

- [ ] **Step 4: Wire into `src/daemon.c`** — add `#include "human/daemon/inbound_audio.h"` with the other daemon includes, and in the `if (is_audio || is_video)` branch replace:

```c
                                if (hu_multimodal_route_local_media(
                                        alloc, path, plen, &agent->provider, model, model_len,
                                        &media_desc, &media_desc_len) == HU_OK &&
                                    media_desc && media_desc_len > 0) {
```

with:

```c
                                hu_error_t merr =
                                    is_audio
                                        ? hu_inbound_audio_transcribe(
                                              alloc, config, &agent->provider, model, model_len,
                                              path, plen, content_to_add, mlen, &media_desc,
                                              &media_desc_len)
                                        : hu_multimodal_route_local_media(
                                              alloc, path, plen, &agent->provider, model,
                                              model_len, &media_desc, &media_desc_len);
                                if (merr == HU_OK && media_desc && media_desc_len > 0) {
```

Caller check: `grep -rn "hu_inbound_audio_transcribe" src | grep -v daemon_inbound_audio.c` → `src/daemon.c`.

- [ ] **Step 5: GREEN** — Run: `touch src/daemon.c && cmake --build build --target human human_tests -j10 && ./build/human_tests --suite="daemon inbound audio"`. Expected: `4/4 passed`.

- [ ] **Step 6: The local server** — `scripts/stt/mlx_whisper_server.py`:

```python
"""Local speech-to-text for h-uman: POST /v1/audio/transcriptions (OpenAI shape)
or /inference (whisper.cpp shape) on loopback, backed by mlx_whisper with a model
already cached under ~/.cache/huggingface. Audio never leaves this Mac, and
neither audio nor text is logged."""
import argparse
import json
import os
import tempfile
from email.parser import BytesParser
from email.policy import default
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODEL = "mlx-community/whisper-large-v3-turbo"
MAX_BYTES = 25 * 1024 * 1024
PATHS = ("/v1/audio/transcriptions", "/inference")


def parse_multipart(content_type, body):
    msg = BytesParser(policy=default).parsebytes(
        b"Content-Type: " + content_type.encode("latin-1") + b"\r\n\r\n" + body)
    fields, files = {}, {}
    for part in msg.iter_parts():
        name = part.get_param("name", header="content-disposition")
        if part.get_filename() is not None:
            files[name] = (part.get_filename(), part.get_payload(decode=True))
        else:
            fields[name] = part.get_payload(decode=True).decode("utf-8", "replace")
    return fields, files


def mlx_transcribe(path, language):
    import mlx_whisper
    result = mlx_whisper.transcribe(path, path_or_hf_repo=MODEL, language=language)
    return (result.get("text") or "").strip()


class Handler(BaseHTTPRequestHandler):
    transcribe = staticmethod(mlx_transcribe)

    def do_POST(self):
        if self.path not in PATHS:
            return self._send(404, {"error": "not found"})
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > MAX_BYTES:
            return self._send(413, {"error": "bad size"})
        body = self.rfile.read(n)
        try:
            fields, files = parse_multipart(self.headers.get("Content-Type", ""), body)
        except Exception:
            return self._send(400, {"error": "bad multipart"})
        if "file" not in files:
            return self._send(400, {"error": "missing file"})
        name, data = files["file"]
        suffix = os.path.splitext(name or "")[1] or ".caf"
        with tempfile.NamedTemporaryFile(suffix=suffix) as f:
            f.write(data)
            f.flush()
            try:
                text = type(self).transcribe(f.name, fields.get("language") or None)
            except Exception as e:
                return self._send(500, {"error": type(e).__name__})
        return self._send(200, {"text": text})

    def _send(self, code, obj):
        raw = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *args):
        pass  # never log requests: they carry people's voice memos


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8761)
    a = ap.parse_args()
    ThreadingHTTPServer(("127.0.0.1", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
```

`scripts/stt/test_mlx_whisper_server.py` (no model load — the transcriber is stubbed):

```python
import json
import os
import sys
import threading
import unittest
import urllib.request
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(__file__))
import mlx_whisper_server as srv  # noqa: E402


def multipart(fields, fname, data, boundary="XyZ"):
    out = b""
    for k, v in fields.items():
        out += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n").encode()
    out += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{fname}\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n").encode() + data + b"\r\n"
    return out + f"--{boundary}--\r\n".encode(), f"multipart/form-data; boundary={boundary}"


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.seen = {}

        def stub(path, language):
            with open(path, "rb") as f:
                self.seen["bytes"] = f.read()
            self.seen["suffix"] = os.path.splitext(path)[1]
            self.seen["language"] = language
            return "stub transcript"

        srv.Handler.transcribe = staticmethod(stub)
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), srv.Handler)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def tearDown(self):
        self.httpd.shutdown()

    def post(self, path, body, ctype):
        req = urllib.request.Request(self.url + path, data=body, headers={"Content-Type": ctype})
        try:
            with urllib.request.urlopen(req) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_binary_audio_round_trips_and_text_comes_back(self):
        audio = bytes(range(256)) * 4 + b"\r\n--XyZ-not-a-boundary\x00"
        body, ctype = multipart({"response_format": "json", "language": "en"}, "Audio Message.caf", audio)
        code, obj = self.post("/v1/audio/transcriptions", body, ctype)
        self.assertEqual(code, 200)
        self.assertEqual(obj, {"text": "stub transcript"})
        self.assertEqual(self.seen["bytes"], audio)
        self.assertEqual(self.seen["suffix"], ".caf")
        self.assertEqual(self.seen["language"], "en")

    def test_whisper_cpp_path_also_works(self):
        body, ctype = multipart({}, "a.wav", b"RIFF....")
        self.assertEqual(self.post("/inference", body, ctype)[0], 200)

    def test_missing_file_is_400_and_unknown_path_is_404(self):
        body, ctype = multipart({"x": "y"}, "a.wav", b"")
        body = body.replace(b'name="file"', b'name="other"')
        self.assertEqual(self.post("/v1/audio/transcriptions", body, ctype)[0], 400)
        self.assertEqual(self.post("/nope", b"x", "text/plain")[0], 404)


if __name__ == "__main__":
    unittest.main()
```

Run: `python3 -m unittest discover -s scripts/stt -p 'test_*.py' -v`
Expected: 3 tests OK (write the test first and watch it fail with `ModuleNotFoundError` before creating the server file).

- [ ] **Step 7: Full suite, commit**

Run: `./build/human_tests 2>&1 | grep Results` → all pass.

```bash
git add include/human/daemon/inbound_audio.h src/daemon/daemon_inbound_audio.c src/daemon.c CMakeLists.txt tests/test_daemon_inbound_audio.c tests/test_main.c scripts/stt/
git commit -m "feat(voice): local Whisper fallback for inbound voice memos"
```

---

### Task 3: D2 — parse and validate a directed line (pure)

**Files:**
- Create: `include/human/tts/speech_direction.h`, `src/tts/speech_direction.c`
- Modify: `CMakeLists.txt` (after `list(APPEND HU_CORE_SOURCES src/tts/speech_rewrite.c)` at line ~1638: `list(APPEND HU_CORE_SOURCES src/tts/speech_direction.c)`; after `tests/test_speech_rewrite.c` at line ~4069: `list(APPEND HU_TEST_SOURCES tests/test_speech_direction.c)`), `tests/test_main.c`
- Test: `tests/test_speech_direction.c`

**Interfaces:**
- Produces (header below): `hu_direction_segment_t`, `hu_direction_t`, `hu_direction_limits_t`, `hu_direction_verdict_t`, `hu_direction_default_limits`, `hu_direction_emotion_valid`, `hu_direction_emotion_count`, `hu_direction_emotion_at`, `hu_direction_parse`, `hu_direction_verdict_name`, `hu_direction_first_emotion`.

- [ ] **Step 1: Write the header** `include/human/tts/speech_direction.h`:

```c
#ifndef HU_TTS_SPEECH_DIRECTION_H
#define HU_TTS_SPEECH_DIRECTION_H
/*
 * Voice direction D2/D3 (spec 2026-09-27 voice direction). A directed line is
 * the model's spoken words with Cartesia tags placed before the words they
 * affect. D2 parses it into segments, rejecting anything outside the palette
 * and clamping values; D3 (hu_direction_render) re-emits canonical tags from
 * the parsed values — model markup is never passed through.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_DIRECTION_MAX_SEGMENTS 12
#define HU_DIRECTION_TEXT_CAP     600
#define HU_DIRECTION_WORDS_CAP    2048
#define HU_DIRECTION_RENDER_CAP   4096

typedef struct {
    char text[HU_DIRECTION_TEXT_CAP];
    size_t text_len;
    char emotion[24]; /* "" = unchanged */
    float speed;      /* 0 = unchanged */
    float volume;     /* 0 = unchanged */
    uint16_t break_ms; /* pause before this segment */
    bool laugh;        /* a laugh before this segment */
} hu_direction_segment_t;

typedef struct {
    hu_direction_segment_t seg[HU_DIRECTION_MAX_SEGMENTS];
    size_t count;
    char words[HU_DIRECTION_WORDS_CAP]; /* spoken words, tags removed */
    size_t words_len;
    size_t sentences;
} hu_direction_t;

typedef struct {
    float speed_min, speed_max, volume_min, volume_max;
    uint16_t break_max_ms;
    uint8_t max_emotion_changes, max_laughs, max_speed_tags, max_volume_tags;
} hu_direction_limits_t;

typedef enum {
    HU_DIRECTION_OK = 0,
    HU_DIRECTION_EMPTY,
    HU_DIRECTION_BAD_TAG,
    HU_DIRECTION_BAD_EMOTION,
    HU_DIRECTION_STAGE_DIRECTION,
    HU_DIRECTION_EMOJI,
    HU_DIRECTION_OVER_BUDGET,
    HU_DIRECTION_TOO_LONG,
} hu_direction_verdict_t;

void hu_direction_default_limits(hu_direction_limits_t *out);
bool hu_direction_emotion_valid(const char *s, size_t n);
size_t hu_direction_emotion_count(void);
const char *hu_direction_emotion_at(size_t i);
/* lim NULL = defaults. `out` is fully overwritten. */
hu_direction_verdict_t hu_direction_parse(const char *line, size_t len,
                                          const hu_direction_limits_t *lim, hu_direction_t *out);
const char *hu_direction_verdict_name(hu_direction_verdict_t v);
/* The first segment's emotion, or NULL. */
const char *hu_direction_first_emotion(const hu_direction_t *d);
#endif
```

- [ ] **Step 2: Write the failing tests** — `tests/test_speech_direction.c`:

```c
/* D2: a directed line is spoken only if every tag is in Cartesia's palette,
 * values are in range and the budgets hold. */
#include "human/tts/speech_direction.h"
#include "test_framework.h"

#include <string.h>

static hu_direction_verdict_t parse(const char *s, hu_direction_t *d) {
    return hu_direction_parse(s, strlen(s), NULL, d);
}

static void test_direction_parses_a_valid_line(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("<emotion value=\"excited\"/>Wait, that's amazing! <break time=\"250ms\"/>"
                       "<emotion value=\"proud\"/>I'm so proud of you.",
                       &d),
                 HU_DIRECTION_OK);
    HU_ASSERT_EQ(d.count, 2);
    HU_ASSERT_STR_EQ(d.seg[0].emotion, "excited");
    HU_ASSERT_STR_EQ(d.seg[1].emotion, "proud");
    HU_ASSERT_EQ(d.seg[1].break_ms, 250);
    HU_ASSERT_STR_EQ(d.words, "Wait, that's amazing! I'm so proud of you.");
    HU_ASSERT_EQ(d.sentences, 2);
    HU_ASSERT_STR_EQ(hu_direction_first_emotion(&d), "excited");
}

static void test_direction_rejects_tags_outside_the_palette(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("<prosody rate=\"slow\">hey</prosody>", &d), HU_DIRECTION_BAD_TAG);
    HU_ASSERT_EQ(parse("<emotion value=\"joyful\"/>hey there", &d), HU_DIRECTION_BAD_EMOTION);
    HU_ASSERT_EQ(parse("<speed ratio=\"fast\"/>hey", &d), HU_DIRECTION_BAD_TAG);
}

static void test_direction_rejects_unclosed_tag(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("<emotion value=\"sad\" hey there", &d), HU_DIRECTION_BAD_TAG);
    HU_ASSERT_EQ(parse("hey the<break time=\"200ms\"re", &d), HU_DIRECTION_BAD_TAG);
}

static void test_direction_rejects_stage_directions_and_emoji(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("*laughs* that's great", &d), HU_DIRECTION_STAGE_DIRECTION);
    HU_ASSERT_EQ(parse("(sighs) fine", &d), HU_DIRECTION_STAGE_DIRECTION);
    HU_ASSERT_EQ(parse("[pause] sure", &d), HU_DIRECTION_STAGE_DIRECTION);
    HU_ASSERT_EQ(parse("love you \xF0\x9F\x98\x8D", &d), HU_DIRECTION_EMOJI);
}

static void test_direction_clamps_values(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("<speed ratio=\"2.0\"/><volume ratio=\"0.2\"/><break time=\"3s\"/>ok then",
                       &d),
                 HU_DIRECTION_OK);
    HU_ASSERT_TRUE(d.seg[0].speed > 1.09f && d.seg[0].speed < 1.11f);
    HU_ASSERT_TRUE(d.seg[0].volume > 0.84f && d.seg[0].volume < 0.86f);
    HU_ASSERT_EQ(d.seg[0].break_ms, 800);
}

static void test_direction_enforces_budgets(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("[laughter] that's hilarious. [laughter] stop it.", &d),
                 HU_DIRECTION_OVER_BUDGET);
    HU_ASSERT_EQ(parse("<emotion value=\"sad\"/>oh no. <emotion value=\"excited\"/>wait "
                       "<emotion value=\"calm\"/>what!",
                       &d),
                 HU_DIRECTION_OVER_BUDGET); /* 2 changes in 2 sentences > max(1, 1) */
    HU_ASSERT_EQ(parse("<speed ratio=\"0.9\"/>slow. <speed ratio=\"1.05\"/>fast.", &d),
                 HU_DIRECTION_OVER_BUDGET);
    HU_ASSERT_EQ(parse("[laughter] that's hilarious, you're ridiculous.", &d), HU_DIRECTION_OK);
}

static void test_direction_empty_and_tags_only(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("", &d), HU_DIRECTION_EMPTY);
    HU_ASSERT_EQ(parse("<emotion value=\"calm\"/><break time=\"200ms\"/>", &d), HU_DIRECTION_EMPTY);
}

static void test_direction_emotion_list_is_cartesias(void) {
    HU_ASSERT_EQ(hu_direction_emotion_count(), 58); /* docs list 58 names */
    HU_ASSERT_TRUE(hu_direction_emotion_valid("affectionate", 12));
    HU_ASSERT_TRUE(hu_direction_emotion_valid("Excited", 7));
    HU_ASSERT_FALSE(hu_direction_emotion_valid("joyful", 6));
    HU_ASSERT_FALSE(hu_direction_emotion_valid("sad", 2)); /* exact length, no prefixes */
}

static void test_direction_verdict_names_are_distinct(void) {
    HU_ASSERT_STR_EQ(hu_direction_verdict_name(HU_DIRECTION_OK), "ok");
    HU_ASSERT_STR_EQ(hu_direction_verdict_name(HU_DIRECTION_BAD_TAG), "bad_tag");
    HU_ASSERT_STR_EQ(hu_direction_verdict_name(HU_DIRECTION_OVER_BUDGET), "over_budget");
}

void run_speech_direction_tests(void) {
    HU_TEST_SUITE("speech direction (D2)");
    HU_RUN_TEST(test_direction_parses_a_valid_line);
    HU_RUN_TEST(test_direction_rejects_tags_outside_the_palette);
    HU_RUN_TEST(test_direction_rejects_unclosed_tag);
    HU_RUN_TEST(test_direction_rejects_stage_directions_and_emoji);
    HU_RUN_TEST(test_direction_clamps_values);
    HU_RUN_TEST(test_direction_enforces_budgets);
    HU_RUN_TEST(test_direction_empty_and_tags_only);
    HU_RUN_TEST(test_direction_emotion_list_is_cartesias);
    HU_RUN_TEST(test_direction_verdict_names_are_distinct);
}
```

Register `run_speech_direction_tests` in `tests/test_main.c` next to `run_speech_rewrite_tests`.

- [ ] **Step 3: RED** — create `src/tts/speech_direction.c` with only `#include "human/tts/speech_direction.h"` and stubs returning `HU_DIRECTION_EMPTY` / `false` / `0` / `NULL` / `"ok"`. Run: `cmake --build build --target human_tests -j10 && ./build/human_tests --suite="speech direction"`. Expected: most tests FAIL on assertions.

- [ ] **Step 4: Implement** `src/tts/speech_direction.c`:

```c
/* Voice direction D2 (+ D3 in Task 4); see include/human/tts/speech_direction.h. */
#include "human/tts/speech_direction.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Cartesia Sonic's emotions, docs.cartesia.ai capability-guides/volume-speed-emotion
 * (read 2026-09-27). Best supported: neutral, calm, angry, content, sad, scared. */
static const char *const k_emotions[] = {
    "neutral",      "happy",       "excited",     "enthusiastic", "elated",     "euphoric",
    "triumphant",   "amazed",      "surprised",   "flirtatious",  "curious",    "content",
    "peaceful",     "serene",      "calm",        "grateful",     "affectionate", "trust",
    "sympathetic",  "anticipation", "mysterious", "angry",        "mad",        "outraged",
    "frustrated",   "agitated",    "threatened",  "disgusted",    "contempt",   "envious",
    "sarcastic",    "ironic",      "sad",         "dejected",     "melancholic", "disappointed",
    "hurt",         "guilty",      "bored",       "tired",        "rejected",   "nostalgic",
    "wistful",      "apologetic",  "hesitant",    "insecure",     "confused",   "resigned",
    "anxious",      "panicked",    "alarmed",     "scared",       "proud",      "confident",
    "distant",      "skeptical",   "contemplative", "determined",
};
#define EMOTION_COUNT (sizeof(k_emotions) / sizeof(k_emotions[0]))

void hu_direction_default_limits(hu_direction_limits_t *o) {
    if (!o)
        return;
    o->speed_min = 0.85f;
    o->speed_max = 1.10f;
    o->volume_min = 0.85f;
    o->volume_max = 1.15f;
    o->break_max_ms = 800;
    o->max_emotion_changes = 3;
    o->max_laughs = 1;
    o->max_speed_tags = 1;
    o->max_volume_tags = 1;
}

bool hu_direction_emotion_valid(const char *s, size_t n) {
    for (size_t i = 0; s && i < EMOTION_COUNT; i++)
        if (strlen(k_emotions[i]) == n && strncasecmp(s, k_emotions[i], n) == 0)
            return true;
    return false;
}

size_t hu_direction_emotion_count(void) {
    return EMOTION_COUNT;
}

const char *hu_direction_emotion_at(size_t i) {
    return i < EMOTION_COUNT ? k_emotions[i] : NULL;
}

const char *hu_direction_verdict_name(hu_direction_verdict_t v) {
    switch (v) {
    case HU_DIRECTION_OK:
        return "ok";
    case HU_DIRECTION_EMPTY:
        return "empty";
    case HU_DIRECTION_BAD_TAG:
        return "bad_tag";
    case HU_DIRECTION_BAD_EMOTION:
        return "bad_emotion";
    case HU_DIRECTION_STAGE_DIRECTION:
        return "stage_direction";
    case HU_DIRECTION_EMOJI:
        return "emoji";
    case HU_DIRECTION_OVER_BUDGET:
        return "over_budget";
    case HU_DIRECTION_TOO_LONG:
        return "too_long";
    }
    return "unknown";
}

const char *hu_direction_first_emotion(const hu_direction_t *d) {
    for (size_t i = 0; d && i < d->count; i++)
        if (d->seg[i].emotion[0])
            return d->seg[i].emotion;
    return NULL;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* attr="value" inside a tag body. */
static bool tag_attr(const char *body, size_t n, const char *attr, char *val, size_t cap) {
    size_t al = strlen(attr);
    for (size_t i = 0; i + al + 2 <= n; i++) {
        if (strncmp(body + i, attr, al) != 0 || body[i + al] != '=' || body[i + al + 1] != '"')
            continue;
        size_t s = i + al + 2, e = s;
        while (e < n && body[e] != '"')
            e++;
        if (e >= n || e == s || e - s >= cap)
            return false;
        memcpy(val, body + s, e - s);
        val[e - s] = '\0';
        return true;
    }
    return false;
}

static bool parse_float(const char *v, float *out) {
    char *end = NULL;
    float f = strtof(v, &end);
    if (end == v || *end != '\0')
        return false;
    *out = f;
    return true;
}

static bool parse_ms(const char *v, uint16_t *out) {
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v || d < 0)
        return false;
    if (strcmp(end, "s") == 0)
        d *= 1000.0;
    else if (strcmp(end, "ms") != 0)
        return false;
    *out = (uint16_t)(d > 60000.0 ? 60000.0 : d);
    return true;
}

static bool is_markup(char c) {
    return c == '<' || c == '>' || c == '[' || c == ']' || c == '*' || c == '(' || c == ')';
}

static size_t count_sentences(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if ((s[i] == '.' || s[i] == '!' || s[i] == '?') &&
            (i + 1 == n || (s[i + 1] != '.' && s[i + 1] != '!' && s[i + 1] != '?')))
            c++;
    return c ? c : 1;
}

/* One tag at line[i] == '<'. Updates `pend`; returns the index past '>' or 0. */
static size_t read_tag(const char *line, size_t len, size_t i, const hu_direction_limits_t *lim,
                       hu_direction_segment_t *pend, char *last_emotion, unsigned *emotions,
                       unsigned *speeds, unsigned *volumes, hu_direction_verdict_t *v) {
    const char *gt = memchr(line + i, '>', len - i);
    const char *lt = memchr(line + i + 1, '<', len - i - 1);
    if (!gt || (lt && lt < gt)) {
        *v = HU_DIRECTION_BAD_TAG;
        return 0;
    }
    const char *body = line + i + 1;
    size_t bn = (size_t)(gt - body);
    if (bn > 0 && body[bn - 1] == '/')
        bn--;
    char val[32];
    float f = 0.f;
    if (bn > 8 && strncmp(body, "emotion ", 8) == 0 && tag_attr(body, bn, "value", val, sizeof(val))) {
        for (char *p = val; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        if (!hu_direction_emotion_valid(val, strlen(val))) {
            *v = HU_DIRECTION_BAD_EMOTION;
            return 0;
        }
        if (strcmp(val, last_emotion) != 0) {
            if (last_emotion[0]) /* setting the opening emotion is not a change */
                (*emotions)++;
            snprintf(pend->emotion, sizeof(pend->emotion), "%s", val);
            snprintf(last_emotion, 24, "%s", val);
        }
    } else if (bn > 6 && strncmp(body, "speed ", 6) == 0 &&
               tag_attr(body, bn, "ratio", val, sizeof(val)) && parse_float(val, &f)) {
        pend->speed = clampf(f, lim->speed_min, lim->speed_max);
        (*speeds)++;
    } else if (bn > 7 && strncmp(body, "volume ", 7) == 0 &&
               tag_attr(body, bn, "ratio", val, sizeof(val)) && parse_float(val, &f)) {
        pend->volume = clampf(f, lim->volume_min, lim->volume_max);
        (*volumes)++;
    } else if (bn > 6 && strncmp(body, "break ", 6) == 0 &&
               tag_attr(body, bn, "time", val, sizeof(val))) {
        uint16_t ms = 0;
        if (!parse_ms(val, &ms)) {
            *v = HU_DIRECTION_BAD_TAG;
            return 0;
        }
        pend->break_ms = ms > lim->break_max_ms ? lim->break_max_ms : ms;
    } else {
        *v = HU_DIRECTION_BAD_TAG;
        return 0;
    }
    return (size_t)(gt - line) + 1;
}

hu_direction_verdict_t hu_direction_parse(const char *line, size_t len,
                                          const hu_direction_limits_t *lim, hu_direction_t *d) {
    hu_direction_limits_t def;
    if (!lim) {
        hu_direction_default_limits(&def);
        lim = &def;
    }
    if (!d)
        return HU_DIRECTION_EMPTY;
    memset(d, 0, sizeof(*d));
    if (!line || len == 0)
        return HU_DIRECTION_EMPTY;
    hu_direction_segment_t pend;
    memset(&pend, 0, sizeof(pend));
    bool pending = false;
    char last_emotion[24] = "";
    unsigned emotions = 0, laughs = 0, speeds = 0, volumes = 0;
    hu_direction_verdict_t v = HU_DIRECTION_OK;
    for (size_t i = 0; i < len;) {
        char c = line[i];
        if (c == '<') {
            i = read_tag(line, len, i, lim, &pend, last_emotion, &emotions, &speeds, &volumes, &v);
            if (!i)
                return v;
            pending = true;
            continue;
        }
        if (c == '[') {
            static const char laugh[] = "[laughter]";
            if (len - i >= sizeof(laugh) - 1 && strncasecmp(line + i, laugh, sizeof(laugh) - 1) == 0) {
                pend.laugh = true;
                pending = true;
                laughs++;
                i += sizeof(laugh) - 1;
                continue;
            }
            return HU_DIRECTION_STAGE_DIRECTION;
        }
        if (is_markup(c))
            return c == '>' ? HU_DIRECTION_BAD_TAG : HU_DIRECTION_STAGE_DIRECTION;
        if ((unsigned char)c == 0xF0)
            return HU_DIRECTION_EMOJI;
        size_t s = i;
        while (i < len && !is_markup(line[i]) && (unsigned char)line[i] != 0xF0)
            i++;
        const char *t = line + s;
        size_t tn = i - s;
        bool blank = true;
        for (size_t k = 0; k < tn; k++)
            if (!isspace((unsigned char)t[k]))
                blank = false;
        if (blank && (pending || d->count == 0))
            continue; /* whitespace between tags */
        if (pending || d->count == 0) {
            if (d->count == HU_DIRECTION_MAX_SEGMENTS)
                return HU_DIRECTION_TOO_LONG;
            d->seg[d->count++] = pend;
            memset(&pend, 0, sizeof(pend));
            pending = false;
        }
        hu_direction_segment_t *g = &d->seg[d->count - 1];
        if (g->text_len + tn >= sizeof(g->text))
            return HU_DIRECTION_TOO_LONG;
        memcpy(g->text + g->text_len, t, tn);
        g->text_len += tn;
        g->text[g->text_len] = '\0';
    }
    for (size_t k = 0; k < d->count; k++) {
        hu_direction_segment_t *g = &d->seg[k];
        size_t a = 0, b = g->text_len;
        while (a < b && isspace((unsigned char)g->text[a]))
            a++;
        while (b > a && isspace((unsigned char)g->text[b - 1]))
            b--;
        memmove(g->text, g->text + a, b - a);
        g->text_len = b - a;
        g->text[g->text_len] = '\0';
        if (g->text_len == 0)
            continue;
        if (d->words_len + g->text_len + 2 >= sizeof(d->words))
            return HU_DIRECTION_TOO_LONG;
        if (d->words_len > 0)
            d->words[d->words_len++] = ' ';
        memcpy(d->words + d->words_len, g->text, g->text_len);
        d->words_len += g->text_len;
        d->words[d->words_len] = '\0';
    }
    if (d->words_len == 0)
        return HU_DIRECTION_EMPTY;
    d->sentences = count_sentences(d->words, d->words_len);
    size_t emotion_cap = (d->sentences + 1) / 2;
    if (emotion_cap < 1)
        emotion_cap = 1;
    if (laughs > lim->max_laughs || speeds > lim->max_speed_tags ||
        volumes > lim->max_volume_tags || emotions > lim->max_emotion_changes ||
        emotions > emotion_cap)
        return HU_DIRECTION_OVER_BUDGET;
    return HU_DIRECTION_OK;
}
```

- [ ] **Step 5: GREEN** — Run: `./build/human_tests --suite="speech direction"`. Expected: `9/9 passed`. Then full suite `./build/human_tests 2>&1 | grep Results` → all pass.

- [ ] **Step 6: Commit**

```bash
git add include/human/tts/speech_direction.h src/tts/speech_direction.c tests/test_speech_direction.c tests/test_main.c CMakeLists.txt
git commit -m "feat(voice): D2 — parse and validate a directed voice line"
```

---

### Task 4: D3 — render canonical tags, laugh style, directed request

**Files:**
- Modify: `include/human/tts/speech_direction.h`, `src/tts/speech_direction.c` (render)
- Modify: `include/human/tts/voice_reply.h`, `src/tts/voice_reply.c` (directed request)
- Test: `tests/test_speech_direction.c` (render), `tests/test_voice_reply.c` (directed request; Cartesia-gated file, already in the gated list at `CMakeLists.txt:4073`)

**Interfaces:**
- Consumes: Task 3's `hu_direction_t`; `hu_transcript_normalize_for_speech(const char *text, size_t text_len, char *out, size_t cap, bool strip_ssml)`.
- Produces: `typedef enum { HU_LAUGH_CARTESIA = 0, HU_LAUGH_TEXT } hu_laugh_style_t;`, `hu_laugh_style_t hu_laugh_style_parse(const char *s);`, `size_t hu_direction_render(const hu_direction_t *d, hu_laugh_style_t laugh, char *out, size_t cap);`, `hu_error_t hu_voice_reply_build_request_directed(const struct hu_persona_voice_config *voice, const char *rendered, size_t rendered_len, const char *first_emotion, size_t sentence_count, hu_voice_reply_request_t *out);`

- [ ] **Step 1: Failing tests** — append to `tests/test_speech_direction.c` (and register):

```c
static void test_direction_render_normalizes_words_not_tags(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("<speed ratio=\"0.93\"/>See you at 7 tonight.", &d), HU_DIRECTION_OK);
    char out[HU_DIRECTION_RENDER_CAP];
    size_t n = hu_direction_render(&d, HU_LAUGH_CARTESIA, out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_CONTAINS(out, "<speed ratio=\"0.93\"/>");
    HU_ASSERT_STR_CONTAINS(out, "seven");
    HU_ASSERT_STR_NOT_CONTAINS(out, "point nine");
}

static void test_direction_render_reemits_only_parsed_tags(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("<emotion value=\"Excited\"/>No way! <break time=\"400ms\"/>That's huge.", &d),
                 HU_DIRECTION_OK);
    char out[HU_DIRECTION_RENDER_CAP];
    hu_direction_render(&d, HU_LAUGH_CARTESIA, out, sizeof(out));
    HU_ASSERT_STR_CONTAINS(out, "<emotion value=\"excited\"/>No way!");
    HU_ASSERT_STR_CONTAINS(out, "<break time=\"400ms\"/>That's huge.");
}

static void test_direction_render_laugh_styles(void) {
    static hu_direction_t d;
    HU_ASSERT_EQ(parse("[laughter] you're ridiculous.", &d), HU_DIRECTION_OK);
    char out[HU_DIRECTION_RENDER_CAP];
    hu_direction_render(&d, HU_LAUGH_CARTESIA, out, sizeof(out));
    HU_ASSERT_STR_CONTAINS(out, "[laughter]");
    hu_direction_render(&d, HU_LAUGH_TEXT, out, sizeof(out));
    HU_ASSERT_STR_NOT_CONTAINS(out, "[laughter]");
    HU_ASSERT_STR_CONTAINS(out, "haha");
    HU_ASSERT_EQ(hu_laugh_style_parse("text"), HU_LAUGH_TEXT);
    HU_ASSERT_EQ(hu_laugh_style_parse(NULL), HU_LAUGH_CARTESIA);
}
```

And in `tests/test_voice_reply.c` (register in its runner):

```c
static void test_voice_reply_directed_request_keeps_the_rendered_transcript(void) {
    hu_persona_voice_config_t v;
    memset(&v, 0, sizeof(v));
    snprintf(v.voice_id, sizeof(v.voice_id), "voice-test");
    v.default_speed = 0.95f;
    const char *r = "<emotion value=\"proud\"/>I'm so proud of you.";
    hu_voice_reply_request_t req;
    HU_ASSERT_EQ(hu_voice_reply_build_request_directed(&v, r, strlen(r), "proud", 1, &req), HU_OK);
    HU_ASSERT_STR_EQ(req.transcript, r);
    HU_ASSERT_STR_EQ(req.tts.emotion, "proud");
    HU_ASSERT_STR_EQ(req.tts.model_id, HU_VOICE_REPLY_DEFAULT_MODEL);
    HU_ASSERT_TRUE(req.tts.speed > 0.94f && req.tts.speed < 0.96f);
    HU_ASSERT_EQ(req.sentence_count, 1);
}
```

- [ ] **Step 2: RED** — Run: `cmake --build build --target human_tests -j10 2>&1 | grep -E " error" | head -3`. Expected: undeclared `hu_direction_render` / `hu_voice_reply_build_request_directed`.

- [ ] **Step 3: Implement** — header additions to `speech_direction.h`:

```c
typedef enum { HU_LAUGH_CARTESIA = 0, HU_LAUGH_TEXT } hu_laugh_style_t;
/* HU_VOICE_LAUGH: "text" writes a spoken laugh; anything else uses Cartesia's
 * [laughter] (the ear A/B decides — voiceai avoids the stock laugh). */
hu_laugh_style_t hu_laugh_style_parse(const char *s);
/* D3: canonical Cartesia transcript re-emitted from the parsed values, each
 * segment's words normalized for speech. Returns the length (0 on overflow). */
size_t hu_direction_render(const hu_direction_t *d, hu_laugh_style_t laugh, char *out, size_t cap);
```

In `speech_direction.c` add `#include "human/tts/transcript_prep.h"` and:

```c
hu_laugh_style_t hu_laugh_style_parse(const char *s) {
    return s && strcmp(s, "text") == 0 ? HU_LAUGH_TEXT : HU_LAUGH_CARTESIA;
}

size_t hu_direction_render(const hu_direction_t *d, hu_laugh_style_t laugh, char *out, size_t cap) {
    if (!d || !out || cap == 0)
        return 0;
    size_t o = 0;
    out[0] = '\0';
    for (size_t i = 0; i < d->count; i++) {
        const hu_direction_segment_t *g = &d->seg[i];
        char norm[HU_DIRECTION_TEXT_CAP * 2];
        size_t nn = hu_transcript_normalize_for_speech(g->text, g->text_len, norm, sizeof(norm), false);
        int w = snprintf(out + o, cap - o, "%s", o ? " " : "");
        if (w < 0 || (size_t)w >= cap - o)
            return 0;
        o += (size_t)w;
        if (g->break_ms)
            w = snprintf(out + o, cap - o, "<break time=\"%ums\"/>", (unsigned)g->break_ms);
        else
            w = 0;
        if (w < 0 || (size_t)w >= cap - o)
            return 0;
        o += (size_t)w;
        if (g->laugh) {
            w = snprintf(out + o, cap - o, "%s",
                         laugh == HU_LAUGH_TEXT ? "haha, <break time=\"150ms\"/>" : "[laughter] ");
            if (w < 0 || (size_t)w >= cap - o)
                return 0;
            o += (size_t)w;
        }
        w = snprintf(out + o, cap - o, "%s%s%s", g->emotion[0] ? "<emotion value=\"" : "",
                     g->emotion, g->emotion[0] ? "\"/>" : "");
        if (w < 0 || (size_t)w >= cap - o)
            return 0;
        o += (size_t)w;
        if (g->speed > 0.f) {
            w = snprintf(out + o, cap - o, "<speed ratio=\"%.2f\"/>", (double)g->speed);
            if (w < 0 || (size_t)w >= cap - o)
                return 0;
            o += (size_t)w;
        }
        if (g->volume > 0.f) {
            w = snprintf(out + o, cap - o, "<volume ratio=\"%.2f\"/>", (double)g->volume);
            if (w < 0 || (size_t)w >= cap - o)
                return 0;
            o += (size_t)w;
        }
        if (nn >= cap - o)
            return 0;
        memcpy(out + o, norm, nn);
        o += nn;
        out[o] = '\0';
    }
    return o;
}
```

`voice_reply.h` addition:

```c
/* A directed memo (spec 2026-09-27 voice direction): `rendered` comes from
 * hu_direction_render and is used as-is; only the request-level config is set.
 * `first_emotion` NULL = persona default. */
hu_error_t hu_voice_reply_build_request_directed(const struct hu_persona_voice_config *voice,
                                                 const char *rendered, size_t rendered_len,
                                                 const char *first_emotion, size_t sentence_count,
                                                 hu_voice_reply_request_t *out);
```

`voice_reply.c` implementation:

```c
hu_error_t hu_voice_reply_build_request_directed(const hu_persona_voice_config_t *voice,
                                                 const char *rendered, size_t rendered_len,
                                                 const char *first_emotion, size_t sentence_count,
                                                 hu_voice_reply_request_t *out) {
    if (!voice || !rendered || rendered_len == 0 || !out ||
        rendered_len >= sizeof(out->transcript))
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    memcpy(out->transcript, rendered, rendered_len);
    out->transcript[rendered_len] = '\0';
    out->transcript_len = rendered_len;
    out->sentence_count = sentence_count;
    snprintf(out->emotion, sizeof(out->emotion), "%s",
             first_emotion && first_emotion[0]
                 ? first_emotion
                 : (voice->default_emotion[0] ? voice->default_emotion : "content"));
    snprintf(out->model, sizeof(out->model), "%s",
             voice->model[0] ? voice->model : HU_VOICE_REPLY_DEFAULT_MODEL);
    out->tts.model_id = out->model;
    out->tts.voice_id = voice->voice_id;
    out->tts.emotion = out->emotion;
    out->tts.speed = voice->default_speed > 0.f ? voice->default_speed : HU_VOICE_REPLY_DEFAULT_SPEED;
    out->tts.volume = 1.0f;
    out->tts.nonverbals = voice->nonverbals;
    return HU_OK;
}
```

- [ ] **Step 4: GREEN** — Run: `./build/human_tests --suite="speech direction"` → 12/12; `./build/human_tests --filter=directed_request` → 1/1; full suite green. If `test_direction_render_normalizes_words_not_tags` shows `hu_transcript_normalize_for_speech` renders "7" differently from "seven", read its output and assert the number word it actually produces — the assertion that matters is that the tag's `0.93` survives.

- [ ] **Step 5: Commit**

```bash
git add include/human/tts/speech_direction.h src/tts/speech_direction.c include/human/tts/voice_reply.h src/tts/voice_reply.c tests/test_speech_direction.c tests/test_voice_reply.c
git commit -m "feat(voice): D3 — render canonical Cartesia tags for a directed line"
```

---

### Task 5: D1 — the model performs the line

**Files:**
- Create: `include/human/tts/speech_perform.h`, `src/tts/speech_perform.c`
- Modify: `CMakeLists.txt` (`list(APPEND HU_CORE_SOURCES src/tts/speech_perform.c)` after speech_direction; `list(APPEND HU_TEST_SOURCES tests/test_speech_perform.c)`), `tests/test_main.c`
- Test: `tests/test_speech_perform.c`

**Interfaces:**
- Consumes: `hu_direction_parse`, `hu_direction_emotion_count/at`, `hu_direction_verdict_name` (Task 3); `hu_speech_drift_check`, `hu_speech_drift_name` (`include/human/tts/speech_text.h`); provider `chat_with_system(ctx, alloc, sys, sys_len, msg, msg_len, model, model_len, double temperature, char **out, size_t *out_len)`.
- Produces: `hu_perform_scene_t`, `hu_perform_result_t`, `hu_speech_perform_system_prompt`, `hu_speech_perform_user_message`, `hu_speech_perform`.

- [ ] **Step 1: Header** `include/human/tts/speech_perform.h`:

```c
#ifndef HU_TTS_SPEECH_PERFORM_H
#define HU_TTS_SPEECH_PERFORM_H
/*
 * Voice direction D1 (spec 2026-09-27): one LLM call casts the model as the
 * speaker in a scene and returns the spoken line with Cartesia tags. The line
 * is parsed and validated (D2) and its words must pass the drift guard (S3)
 * against the intent; anything else is reported, never spoken.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/tts/speech_direction.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct hu_perform_scene {
    const char *speaker;      /* persona name, NULL = "the speaker" */
    const char *listener;     /* contact name, NULL = "a friend" */
    const char *relationship; /* e.g. "sister", NULL = omit */
    int hour_local;           /* 0-23 */
    int weekday;              /* 0 = Sunday .. 6, -1 = omit */
    const char *inbound;      /* what they last said, may be NULL */
    size_t inbound_len;
} hu_perform_scene_t;

typedef struct {
    bool ok;            /* dir is valid and its words pass the drift guard */
    const char *reason; /* static: ok, no_provider, provider_error, empty, a D2 verdict or a drift name */
    hu_direction_t dir;
} hu_perform_result_t;

size_t hu_speech_perform_system_prompt(char *out, size_t cap);
size_t hu_speech_perform_user_message(const hu_perform_scene_t *scene, const char *intent,
                                      size_t intent_len, char *out, size_t cap);
/* Never fails for content reasons; see out->ok / out->reason. */
hu_error_t hu_speech_perform(hu_allocator_t *alloc, const hu_provider_t *provider,
                             const char *model, size_t model_len, const hu_perform_scene_t *scene,
                             const char *intent, size_t intent_len, hu_perform_result_t *out);
#endif
```

- [ ] **Step 2: Failing tests** — `tests/test_speech_perform.c`:

```c
/* D1: the model is cast as the speaker and directs its own delivery; only a
 * valid, faithful line is reported ok. */
#include "human/tts/speech_perform.h"
#include "test_framework.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *reply;
    hu_error_t err;
    int calls;
    char sys[16384];
    char msg[2048];
} perf_mock_t;
static perf_mock_t *g_pm;

static hu_error_t pm_chat(void *ctx, hu_allocator_t *alloc, const char *sys, size_t sl,
                          const char *msg, size_t ml, const char *model, size_t mlen, double t,
                          char **out, size_t *out_len) {
    (void)ctx;
    (void)model;
    (void)mlen;
    (void)t;
    g_pm->calls++;
    snprintf(g_pm->sys, sizeof(g_pm->sys), "%.*s", (int)sl, sys);
    snprintf(g_pm->msg, sizeof(g_pm->msg), "%.*s", (int)ml, msg);
    if (g_pm->err != HU_OK)
        return g_pm->err;
    size_t n = strlen(g_pm->reply);
    char *b = alloc->alloc(alloc->ctx, n + 1);
    memcpy(b, g_pm->reply, n + 1);
    *out = b;
    *out_len = n;
    return HU_OK;
}

static hu_perform_result_t *run(perf_mock_t *m, const char *intent) {
    static hu_provider_vtable_t vt;
    static hu_perform_result_t r;
    memset(&vt, 0, sizeof(vt));
    vt.chat_with_system = pm_chat;
    g_pm = m;
    hu_provider_t p;
    memset(&p, 0, sizeof(p));
    p.vtable = &vt;
    hu_perform_scene_t scene = {.speaker = "Seth", .listener = "Mindy", .relationship = "sister",
                                .hour_local = 14, .weekday = 0, .inbound = "love you bro",
                                .inbound_len = 11};
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_speech_perform(&alloc, &p, "m", 1, &scene, intent, strlen(intent), &r), HU_OK);
    return &r;
}

static void test_perform_directed_line_is_ok(void) {
    perf_mock_t m = {.reply = "<emotion value=\"affectionate\"/>Love you too. I'm so proud of you."};
    hu_perform_result_t *r = run(&m, "love you too, so proud of you");
    HU_ASSERT_TRUE(r->ok);
    HU_ASSERT_STR_EQ(r->reason, "ok");
    HU_ASSERT_STR_EQ(r->dir.seg[0].emotion, "affectionate");
    HU_ASSERT_EQ(m.calls, 1);
}

static void test_perform_strips_quotes_and_a_line_label(void) {
    perf_mock_t m = {.reply = "Line: \"<emotion value=\"content\"/>Love you too.\""};
    hu_perform_result_t *r = run(&m, "love you too");
    HU_ASSERT_TRUE(r->ok);
    HU_ASSERT_STR_EQ(r->dir.words, "Love you too.");
}

static void test_perform_rejects_invented_facts(void) {
    perf_mock_t m = {.reply = "<emotion value=\"excited\"/>See you at 7 tonight!"};
    hu_perform_result_t *r = run(&m, "see you later");
    HU_ASSERT_FALSE(r->ok);
    HU_ASSERT_STR_EQ(r->reason, "new_number");
}

static void test_perform_reports_invalid_lines(void) {
    perf_mock_t m = {.reply = "<prosody rate=\"slow\">love you</prosody>"};
    HU_ASSERT_STR_EQ(run(&m, "love you")->reason, "bad_tag");
    perf_mock_t e = {.reply = "", .err = HU_ERR_IO};
    HU_ASSERT_STR_EQ(run(&e, "love you")->reason, "provider_error");
    perf_mock_t z = {.reply = "   "};
    HU_ASSERT_STR_EQ(run(&z, "love you")->reason, "empty");
}

static void test_perform_without_provider(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static hu_perform_result_t r;
    HU_ASSERT_EQ(hu_speech_perform(&alloc, NULL, NULL, 0, NULL, "hi", 2, &r), HU_OK);
    HU_ASSERT_FALSE(r.ok);
    HU_ASSERT_STR_EQ(r.reason, "no_provider");
}

static void test_perform_scene_reaches_the_model(void) {
    perf_mock_t m = {.reply = "Love you too."};
    run(&m, "love you too");
    HU_ASSERT_STR_CONTAINS(m.msg, "Mindy");
    HU_ASSERT_STR_CONTAINS(m.msg, "sister");
    HU_ASSERT_STR_CONTAINS(m.msg, "Sunday afternoon");
    HU_ASSERT_STR_CONTAINS(m.msg, "love you bro");
    HU_ASSERT_STR_CONTAINS(m.msg, "love you too");
}

static void test_perform_prompt_lists_the_whole_palette(void) {
    static char sys[16384];
    HU_ASSERT_TRUE(hu_speech_perform_system_prompt(sys, sizeof(sys)) > 0);
    for (size_t i = 0; i < hu_direction_emotion_count(); i++)
        HU_ASSERT_STR_CONTAINS(sys, hu_direction_emotion_at(i));
    HU_ASSERT_STR_CONTAINS(sys, "[laughter]");
    HU_ASSERT_STR_CONTAINS(sys, "spoken aloud");
    HU_ASSERT_STR_NOT_CONTAINS(sys, "Ferni");
}

void run_speech_perform_tests(void) {
    HU_TEST_SUITE("speech perform (D1)");
    HU_RUN_TEST(test_perform_directed_line_is_ok);
    HU_RUN_TEST(test_perform_strips_quotes_and_a_line_label);
    HU_RUN_TEST(test_perform_rejects_invented_facts);
    HU_RUN_TEST(test_perform_reports_invalid_lines);
    HU_RUN_TEST(test_perform_without_provider);
    HU_RUN_TEST(test_perform_scene_reaches_the_model);
    HU_RUN_TEST(test_perform_prompt_lists_the_whole_palette);
}
```

Register `run_speech_perform_tests` in `tests/test_main.c`.

- [ ] **Step 3: RED** — stub `src/tts/speech_perform.c` (`return 0;` / `out->ok=false; out->reason="empty"; return HU_OK;`). Run the suite. Expected: 6 of 7 FAIL on assertions.

- [ ] **Step 4: Implement** `src/tts/speech_perform.c`. The prompt is split into literals below 4095 bytes each (ISO C minimum) and assembled at runtime; the emotion list comes from D2's table so the prompt and the validator can never disagree. The voice block is a **placeholder** until the calibration plan replaces it with values measured from Seth's MV7 takes (spec §Calibration).

```c
/* Voice direction D1; see include/human/tts/speech_perform.h. */
#include "human/tts/speech_perform.h"
#include "human/tts/speech_text.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char k_cast[] =
    "You are an actor voicing a real person in a play. Your line is recorded as a voice memo "
    "on a phone and played to the person it is meant for. Every word you write is spoken aloud "
    "by a voice clone of the speaker, so write only what the speaker would actually say.\n\n"
    "THE LINE\n"
    "- Say what the intent says, the way the speaker would say it out loud to this listener: "
    "contractions, short sentences; fragments are fine.\n"
    "- Add nothing new: no new names, times, numbers, days, places, plans, promises or "
    "questions. Keep every question the intent asks. About as long as the intent.\n"
    "- Never start with \"Well\", \"So\", \"Hmm\" or \"Um\". Never say \"good question\".\n"
    "- No asterisks, parentheses, emoji, stage directions (\"warmly\") or narrated actions "
    "(\"*laughs*\").\n\n"
    "DIRECTING YOUR DELIVERY\n"
    "Direct your own voice with these tags, placed right before the words they affect:\n"
    "  <emotion value=\"NAME\"/>  NAME is one of the emotions listed below\n"
    "  <speed ratio=\"0.85\"/> to <speed ratio=\"1.10\"/>  slower for weight, faster for "
    "excitement\n"
    "  <volume ratio=\"0.85\"/> to <volume ratio=\"1.15\"/>  softer for tender moments\n"
    "  <break time=\"300ms\"/>  a real pause, 100ms to 800ms\n"
    "  [laughter]  only when the moment is actually funny\n"
    "Tags are for emphasis, not every sentence: at most one emotion change every two "
    "sentences, one laugh, one speed change and one volume change. The emotion must match the "
    "words; a tag that fights the words sounds fake.\n"
    "Emotions: ";

/* Placeholder voice DNA — replaced from Seth's calibration takes (spec
 * §Calibration). Deliberately generic: no invented catchphrases. */
static const char k_voice[] =
    "\n\nTHE SPEAKER'S VOICE\n"
    "- Natural, unhurried pace. Usually content or affectionate; excited when something is "
    "genuinely good; sympathetic when it's hard. Rarely dramatic.\n"
    "- Reacts the way people do out loud (\"oh\", \"ha\", \"yeah\") only when it fits.\n\n"
    "EXAMPLES\n"
    "Intent: that's amazing, so proud of you\n"
    "Line: <emotion value=\"excited\"/>Wait, that's amazing! <break time=\"250ms\"/>"
    "<emotion value=\"proud\"/>I'm so proud of you.\n"
    "Intent: ugh I'm sorry, that sounds rough\n"
    "Line: <emotion value=\"sympathetic\"/><speed ratio=\"0.92\"/>Oh, I'm sorry. That sounds "
    "really rough.\n"
    "Intent: haha you're ridiculous\n"
    "Line: [laughter] You're ridiculous.\n\n"
    "Output only the line.\n";

static size_t put(char *out, size_t cap, size_t o, const char *s) {
    size_t n = strlen(s);
    if (o + n >= cap)
        return cap;
    memcpy(out + o, s, n);
    out[o + n] = '\0';
    return o + n;
}

size_t hu_speech_perform_system_prompt(char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    size_t o = put(out, cap, 0, k_cast);
    for (size_t i = 0; o < cap && i < hu_direction_emotion_count(); i++) {
        o = put(out, cap, o, hu_direction_emotion_at(i));
        o = put(out, cap, o, i + 1 < hu_direction_emotion_count() ? ", " : ".");
    }
    o = put(out, cap, o, k_voice);
    return o >= cap ? 0 : o;
}

static const char *part_of_day(int h) {
    if (h >= 5 && h < 12)
        return "morning";
    if (h >= 12 && h < 17)
        return "afternoon";
    if (h >= 17 && h < 22)
        return "evening";
    return "night";
}

size_t hu_speech_perform_user_message(const hu_perform_scene_t *s, const char *intent,
                                      size_t intent_len, char *out, size_t cap) {
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                       "Thursday", "Friday", "Saturday"};
    if (!out || cap == 0 || !intent)
        return 0;
    const char *who = s && s->listener ? s->listener : "a friend";
    int n = snprintf(out, cap,
                     "Scene: %s is recording a quick voice memo on the phone to %s%s%s%s. "
                     "It's %s%s%s.\n",
                     s && s->speaker ? s->speaker : "The speaker", who,
                     s && s->relationship ? " (" : "", s && s->relationship ? s->relationship : "",
                     s && s->relationship ? ")" : "",
                     s && s->weekday >= 0 && s->weekday < 7 ? days[s->weekday] : "",
                     s && s->weekday >= 0 && s->weekday < 7 ? " " : "",
                     part_of_day(s ? s->hour_local : 12));
    if (n < 0 || (size_t)n >= cap)
        return 0;
    size_t o = (size_t)n;
    if (s && s->inbound && s->inbound_len > 0) {
        n = snprintf(out + o, cap - o, "%s last said: \"%.*s\"\n", who,
                     (int)(s->inbound_len > 600 ? 600 : s->inbound_len), s->inbound);
        if (n < 0 || (size_t)n >= cap - o)
            return 0;
        o += (size_t)n;
    }
    n = snprintf(out + o, cap - o, "Intent (what the memo must say): \"%.*s\"\nSay the line.\n",
                 (int)intent_len, intent);
    if (n < 0 || (size_t)n >= cap - o)
        return 0;
    return o + (size_t)n;
}

/* Models wrap lines in quotes or prefix "Line:"; neither is spoken. */
static void trim_line(char *s, size_t *n) {
    size_t a = 0, b = *n;
    while (a < b && isspace((unsigned char)s[a]))
        a++;
    if (b - a >= 5 && strncmp(s + a, "Line:", 5) == 0)
        a += 5;
    while (a < b && isspace((unsigned char)s[a]))
        a++;
    while (b > a && isspace((unsigned char)s[b - 1]))
        b--;
    if (b - a >= 2 && s[a] == '"' && s[b - 1] == '"') {
        a++;
        b--;
    }
    memmove(s, s + a, b - a);
    *n = b - a;
    s[*n] = '\0';
}

hu_error_t hu_speech_perform(hu_allocator_t *alloc, const hu_provider_t *provider,
                             const char *model, size_t model_len, const hu_perform_scene_t *scene,
                             const char *intent, size_t intent_len, hu_perform_result_t *out) {
    if (!alloc || !out || !intent)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->reason = "empty";
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system) {
        out->reason = "no_provider";
        return HU_OK;
    }
    char *sys = alloc->alloc(alloc->ctx, 16384);
    char msg[2048];
    if (!sys)
        return HU_ERR_OUT_OF_MEMORY;
    size_t sn = hu_speech_perform_system_prompt(sys, 16384);
    size_t mn = hu_speech_perform_user_message(scene, intent, intent_len, msg, sizeof(msg));
    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = HU_ERR_INVALID_ARGUMENT;
    if (sn > 0 && mn > 0)
        err = provider->vtable->chat_with_system(provider->ctx, alloc, sys, sn, msg, mn, model,
                                                 model_len, 0.7, &raw, &raw_len);
    alloc->free(alloc->ctx, sys, 16384);
    if (err != HU_OK || !raw) {
        out->reason = "provider_error";
        return HU_OK;
    }
    size_t alloc_len = raw_len + 1;
    trim_line(raw, &raw_len);
    hu_direction_verdict_t v = hu_direction_parse(raw, raw_len, NULL, &out->dir);
    alloc->free(alloc->ctx, raw, alloc_len);
    if (v != HU_DIRECTION_OK) {
        out->reason = hu_direction_verdict_name(v);
        return HU_OK;
    }
    hu_speech_drift_t dr = hu_speech_drift_check(intent, intent_len, out->dir.words, out->dir.words_len);
    if (dr != HU_SPEECH_DRIFT_OK) {
        out->reason = hu_speech_drift_name(dr);
        return HU_OK;
    }
    out->ok = true;
    out->reason = "ok";
    return HU_OK;
}
```

- [ ] **Step 5: GREEN** — Run: `./build/human_tests --suite="speech perform"` → 7/7; full suite green. The free size (`raw_len + 1`, captured before trimming) must match how `speech_rewrite.c:134-140` frees the same provider's output; ASan in the dev build fails the suite if it doesn't.

- [ ] **Step 6: Commit**

```bash
git add include/human/tts/speech_perform.h src/tts/speech_perform.c tests/test_speech_perform.c tests/test_main.c CMakeLists.txt
git commit -m "feat(voice): D1 — cast the model as the speaker and let it direct the line"
```

---

### Task 6: Daemon — `HU_SPEECH_DIRECTION` in the voice path

**Files:**
- Modify: `src/daemon/daemon_voice_reply.c` (`voice_spoken_final` + both TTS arms)
- Test: `tests/test_daemon_voice_reply.c`

**Interfaces:**
- Consumes: `hu_speech_perform` (Task 5), `hu_direction_render`, `hu_laugh_style_parse`, `hu_direction_first_emotion` (Task 4), `hu_voice_reply_build_request_directed` (Task 4, Cartesia-gated), `hu_persona_find_contact(persona, id, id_len)` → `hu_contact_profile_t{name, relationship}`, `agent->persona->name`.

- [ ] **Step 1: Failing tests** — add to `tests/test_daemon_voice_reply.c` (reusing its `rw_mock_chat`, `g_rewrite_out`, `g_rewrite_calls`, `vr_send`, `g_voice_sends`, `hu_cartesia_test_last_transcript`):

```c
static bool run_direct_voice(const char *reply, const char *model_line, const char *mode) {
    static hu_provider_vtable_t pvt;
    memset(&pvt, 0, sizeof(pvt));
    pvt.chat_with_system = rw_mock_chat;
    g_rewrite_out = model_line;
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.provider.vtable = &pvt;
    static hu_config_t config;
    memset(&config, 0, sizeof(config));
    config.channels.default_daemon.voice_enabled = true;
    config.voice.tts_provider = "cartesia";
    hu_channel_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.name = vr_name_generic;
    vt.send = vr_send;
    hu_channel_t channel;
    memset(&channel, 0, sizeof(channel));
    channel.vtable = &vt;
    hu_service_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    ch.channel = &channel;
    setenv("HU_SPEECH_DIRECTION", mode, 1);
    bool sent = hu_daemon_voice_reply(&alloc, &agent, &config, &ch, "+15550000001", 12, "hey", 3,
                                      reply, strlen(reply), NULL, 0, 14);
    unsetenv("HU_SPEECH_DIRECTION");
    return sent;
}

static void test_voice_reply_speaks_the_directed_line(void) {
    g_voice_sends = 0;
    HU_ASSERT_TRUE(run_direct_voice("yeah sounds good", "<emotion value=\"excited\"/>Yeah, sounds good!", "live"));
    HU_ASSERT_EQ(g_voice_sends, 1);
    const char *t = hu_cartesia_test_last_transcript();
    HU_ASSERT_STR_CONTAINS(t, "<emotion value=\"excited\"/>");
    HU_ASSERT_STR_CONTAINS(t, "Yeah, sounds good!");
}

static void test_voice_reply_invalid_direction_speaks_plain_text(void) {
    g_voice_sends = 0;
    HU_ASSERT_TRUE(run_direct_voice("yeah sounds good", "<prosody>Yeah</prosody>", "live"));
    HU_ASSERT_STR_EQ(hu_cartesia_test_last_transcript(), "yeah sounds good");
}

static void test_voice_reply_direction_shadow_speaks_plain_text(void) {
    g_voice_sends = 0;
    g_rewrite_calls = 0;
    HU_ASSERT_TRUE(run_direct_voice("yeah sounds good", "<emotion value=\"excited\"/>Yeah, sounds good!", "shadow"));
    HU_ASSERT_EQ(g_rewrite_calls, 1);
    HU_ASSERT_STR_EQ(hu_cartesia_test_last_transcript(), "yeah sounds good");
}

static void test_voice_reply_directed_line_that_trips_moderation_is_not_spoken(void) {
    g_voice_sends = 0;
    HU_ASSERT_TRUE(run_direct_voice("we watched that show with the kids",
                                    "<emotion value=\"calm\"/>kill them with violence and murder", "live"));
    const char *t = hu_cartesia_test_last_transcript();
    HU_ASSERT_STR_NOT_CONTAINS(t, "kill");
    HU_ASSERT_STR_CONTAINS(t, "we watched that show with the kids");
}

static void test_voice_reply_direction_off_is_todays_path(void) {
    g_voice_sends = 0;
    g_rewrite_calls = 0;
    HU_ASSERT_TRUE(run_direct_voice("yeah sounds good", "<emotion value=\"excited\"/>Yeah!", "off"));
    HU_ASSERT_EQ(g_rewrite_calls, 0);
    HU_ASSERT_STR_EQ(hu_cartesia_test_last_transcript(), "yeah sounds good");
}
```

Register all five in `run_daemon_voice_reply_tests`. (If the moderation pair passes the drift guard's LENGTH check differently than expected, the assertion that matters is that "kill" is never spoken.)

- [ ] **Step 2: RED** — Run: `./build/human_tests --suite="daemon voice reply"`. Expected: `test_voice_reply_speaks_the_directed_line` FAILS (plain text spoken); the others may already pass — that is fine, they pin today's behavior.

- [ ] **Step 3: Implement** in `src/daemon/daemon_voice_reply.c`. Add includes `#include "human/persona.h"`, `#include "human/tts/speech_direction.h"`, `#include "human/tts/speech_perform.h"`. Add above `voice_spoken_final`:

```c
/* F2-voice direction state for one memo (spec 2026-09-27 voice direction). */
typedef struct {
    int state;     /* 0 not yet run, 1 speak, -1 declined */
    bool directed; /* LIVE direction passed: speak `rendered` */
    char rendered[HU_DIRECTION_RENDER_CAP];
    size_t rendered_len;
    char words[HU_DIRECTION_WORDS_CAP];
    size_t words_len;
    char emotion[24];
    size_t sentences;
} voice_final_t;

/* D1 + D2 + S3 (inside hu_speech_perform), then S4 on the words and D3. SHADOW
 * runs and logs only. HU_SPEECH_DIRECTION as a default is gated on Seth's ear
 * test (>= 8/10 directed) and the W5 real-or-clone test rated by Mindy — do
 * not flip without both. */
static void voice_direct(hu_allocator_t *alloc, hu_agent_t *agent, const char *batch_key,
                         size_t key_len, const char *combined, size_t combined_len,
                         const hu_speech_result_t *sp, hu_speech_rewrite_mode_t mode,
                         voice_final_t *vf) {
    hu_perform_result_t *r = alloc->alloc(alloc->ctx, sizeof(*r));
    if (!r)
        return;
    const hu_contact_profile_t *cp =
        agent && agent->persona && batch_key
            ? hu_persona_find_contact(agent->persona, batch_key, key_len)
            : NULL;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    hu_perform_scene_t scene = {
        .speaker = agent && agent->persona ? agent->persona->name : NULL,
        .listener = cp ? cp->name : NULL,
        .relationship = cp ? cp->relationship : NULL,
        .hour_local = tmv.tm_hour,
        .weekday = tmv.tm_wday,
        .inbound = combined,
        .inbound_len = combined_len,
    };
    (void)hu_speech_perform(alloc, agent ? &agent->provider : NULL,
                            agent ? agent->model_name : NULL, agent ? agent->model_name_len : 0,
                            &scene, sp->spoken, sp->spoken_len, r);
    hu_log_info("voice_reply", NULL, "direction %s: ok=%d reason=%s segments=%zu",
                mode == HU_SPEECH_REWRITE_LIVE ? "live" : "shadow", r->ok ? 1 : 0, r->reason,
                r->dir.count);
    if (mode == HU_SPEECH_REWRITE_LIVE && r->ok &&
        voice_gates_pass(alloc, r->dir.words, r->dir.words_len, combined, combined_len,
                         "directed")) {
        vf->rendered_len = hu_direction_render(&r->dir, hu_laugh_style_parse(getenv("HU_VOICE_LAUGH")),
                                               vf->rendered, sizeof(vf->rendered));
        if (vf->rendered_len > 0) {
            memcpy(vf->words, r->dir.words, r->dir.words_len + 1);
            vf->words_len = r->dir.words_len;
            const char *fe = hu_direction_first_emotion(&r->dir);
            snprintf(vf->emotion, sizeof(vf->emotion), "%s", fe ? fe : "");
            vf->sentences = r->dir.sentences;
            vf->directed = true;
        }
    }
    alloc->free(alloc->ctx, r, sizeof(*r));
}
```

Change `voice_spoken_final` to take `batch_key, key_len` and a `voice_final_t *vf` in place of `int *state`, skip the S1 rewrite when direction is on, and run `voice_direct` once the memo is cleared:

```c
static bool voice_spoken_final(hu_allocator_t *alloc, hu_agent_t *agent, const char *batch_key,
                               size_t key_len, const char *response, size_t response_len,
                               const char *combined, size_t combined_len, hu_speech_result_t *sp,
                               voice_final_t *vf) {
    if (vf->state == 0) {
        hu_speech_rewrite_mode_t dm = hu_speech_rewrite_mode_parse(getenv("HU_SPEECH_DIRECTION"));
        hu_speech_rewrite_mode_t m = hu_speech_rewrite_mode_parse(getenv("HU_SPEECH_REWRITE"));
        if (m != HU_SPEECH_REWRITE_OFF && dm == HU_SPEECH_REWRITE_OFF) {
            /* (existing S1 rewrite block, unchanged) */
        }
        vf->state = sp->spoken_len > 0 && voice_gates_pass(alloc, sp->spoken, sp->spoken_len,
                                                          combined, combined_len, "spoken")
                        ? 1
                        : -1;
        if (vf->state == 1 && dm != HU_SPEECH_REWRITE_OFF)
            voice_direct(alloc, agent, batch_key, key_len, combined, combined_len, sp, dm, vf);
    }
    return vf->state == 1;
}
```

In `hu_daemon_voice_reply`: replace `int spoken_state = 0;` with `voice_final_t vf; memset(&vf, 0, sizeof(vf));`, pass `batch_key, key_len, ..., &sp, &vf` at both `voice_spoken_final` calls. In the Cartesia arm replace the request build with:

```c
                    hu_error_t prep_err =
                        vf.directed
                            ? hu_voice_reply_build_request_directed(
                                  &agent->persona->voice, vf.rendered, vf.rendered_len,
                                  vf.emotion[0] ? vf.emotion : NULL, vf.sentences, &req)
                            : hu_voice_reply_build_request_ex(
                                  &agent->persona->voice, sp.spoken, sp.spoken_len, combined,
                                  combined_len, bth_hour, (uint32_t)time(NULL), sp.laughter_cue,
                                  &req);
```

In the fallback arm, speak tags only to Cartesia:

```c
                bool tags_ok = voice_cfg.tts_provider && strcmp(voice_cfg.tts_provider, "cartesia") == 0;
                const char *say = vf.directed ? (tags_ok ? vf.rendered : vf.words) : sp.spoken;
                size_t say_len = vf.directed ? (tags_ok ? vf.rendered_len : vf.words_len) : sp.spoken_len;
                hu_error_t tts_err = hu_voice_tts(alloc, &voice_cfg, say, say_len, &audio, &audio_len);
```

- [ ] **Step 4: GREEN** — Run: `touch src/daemon/daemon_voice_reply.c && cmake --build build --target human human_tests -j10 && ./build/human_tests --suite="daemon voice reply"` → all pass; full suite green. Caller check: `grep -rn "hu_speech_perform\b\|hu_direction_render\|hu_voice_reply_build_request_directed" src | grep -vE "speech_perform.c|speech_direction.c|voice_reply.c:"` shows `daemon_voice_reply.c`.

- [ ] **Step 5: Commit**

```bash
git add src/daemon/daemon_voice_reply.c tests/test_daemon_voice_reply.c
git commit -m "feat(voice): HU_SPEECH_DIRECTION — perform, validate and speak the directed line (default off)"
```

---

### Task 7: `human voice preview --direct` for the ear test

**Files:**
- Modify: `src/app/main.c` (`cmd_voice_preview`, lines ~2671-2845)

**Interfaces:**
- Consumes: Tasks 4–5.

- [ ] **Step 1: Extract the render tail into a helper (move, don't copy).** Move the block from `unsigned char *bytes = NULL;` through the final `printf("wrote ...")` into:

```c
/* Synthesize one request and write it to `out_path` (NULL/"" = temp file). */
static hu_error_t preview_render(hu_allocator_t *alloc, const char *api_key, const char *channel,
                                 hu_voice_reply_request_t *req, const char *out_path) {
    unsigned char *bytes = NULL;
    size_t len = 0;
    hu_error_t err = hu_cartesia_tts_synthesize(alloc, api_key, strlen(api_key), req->transcript,
                                                req->transcript_len, &req->tts,
                                                hu_tts_format_for_channel(channel), &bytes, &len);
    if (err != HU_OK || !bytes || len == 0) {
        fprintf(stderr, "Error: Cartesia synthesis failed: %s\n", hu_error_string(err));
        return err != HU_OK ? err : HU_ERR_IO;
    }
    char path[512];
    err = hu_voice_reply_audio_to_temp(alloc, channel, bytes, len, path, sizeof(path));
    hu_cartesia_tts_free_bytes(alloc, bytes, len);
    if (err != HU_OK) {
        fprintf(stderr, "Error: audio conversion failed: %s\n", hu_error_string(err));
        return err;
    }
    if (out_path && out_path[0]) {
        if (rename(path, out_path) != 0) {
            fprintf(stderr, "Error: could not move %s to %s\n", path, out_path);
            return HU_ERR_IO;
        }
        printf("wrote %s (%zu bytes of audio)\n", out_path, len);
    } else {
        printf("wrote %s (%zu bytes of audio; temp file, move it before the next run)\n", path, len);
    }
    return HU_OK;
}
```

and call it from the original site (free the persona before returning, as the old code did). Run `./build/human voice preview --persona seth --text "sounds good" --rewrite off --out /private/tmp/claude-501/f1/t7_base.caf` → `wrote ...`, exit 0 (no behavior change).

- [ ] **Step 2: Add the flags** next to `--rewrite`: `--direct` (bool), `--to <name>`, `--relationship <rel>`, `--laugh text|cartesia`, `--baseline-out <path>`; extend the usage string with `[--direct [--to NAME] [--relationship REL] [--laugh text|cartesia] [--baseline-out PATH]]`. Create the provider when `rw != HU_SPEECH_REWRITE_OFF || direct` (today's `have_prov` condition only checks `rw`).

- [ ] **Step 3: Direct mode** — after the S2/S1 `hu_speech_prepare` and before building `req`:

```c
        if (direct) {
            time_t tnow = time(NULL);
            struct tm tmd;
            localtime_r(&tnow, &tmd);
            hu_perform_scene_t scene = {.speaker = persona.name, .listener = to_name,
                                        .relationship = relationship, .hour_local = tmd.tm_hour,
                                        .weekday = tmd.tm_wday, .inbound = incoming,
                                        .inbound_len = incoming ? strlen(incoming) : 0};
            static hu_perform_result_t pr;
            (void)hu_speech_perform(alloc, have_prov ? &prov : NULL, mdl, strlen(mdl), &scene,
                                    sp.spoken, sp.spoken_len, &pr);
            printf("direction: ok=%d reason=%s\nwords: %s\n", pr.ok ? 1 : 0, pr.reason, pr.dir.words);
            if (baseline_out && baseline_out[0]) {
                hu_voice_reply_request_t base;
                if (hu_voice_reply_build_request_ex(&persona.voice, sp.spoken, sp.spoken_len, incoming,
                                                    incoming ? strlen(incoming) : 0, tmb.tm_hour,
                                                    (uint32_t)now, sp.laughter_cue, &base) == HU_OK)
                    (void)preview_render(alloc, api_key, channel, &base, baseline_out);
            }
            if (pr.ok) {
                static char rendered[HU_DIRECTION_RENDER_CAP];
                size_t rn = hu_direction_render(&pr.dir, hu_laugh_style_parse(laugh_arg), rendered,
                                                sizeof(rendered));
                const char *fe = hu_direction_first_emotion(&pr.dir);
                if (rn > 0 && hu_voice_reply_build_request_directed(&persona.voice, rendered, rn, fe,
                                                                    pr.dir.sentences, &req) == HU_OK) {
                    directed_ready = true; /* skip the heuristic build below */
                }
            }
        }
```

(`directed_ready` is a new `bool` initialized to `false`; the existing `hu_voice_reply_build_request_ex` call runs only when it is false. Provider deinit moves after this block. Includes: `human/tts/speech_direction.h`, `human/tts/speech_perform.h`.)

- [ ] **Step 4: Verify on real text (sends nothing)**

Run:
```bash
./build/human voice preview --persona seth --direct --to Mindy --relationship sister \
  --text "love you too, so proud of you" --incoming "love you bro" \
  --out /private/tmp/claude-501/f1/t7_dir.caf --baseline-out /private/tmp/claude-501/f1/t7_base.caf
```
Expected: `direction: ok=1 reason=ok` (or a named reason — record it), `transcript` shows the model's tags, two `wrote` lines, exit 0. Run it on five texts; record each `reason` in the ledger (this is the first measurement of the local model's reject rate — the spec's open decision 1).

- [ ] **Step 5: Full suite, commit**

```bash
git add src/app/main.c
git commit -m "feat(voice-cli): preview --direct renders the directed memo beside today's"
```

---

## Human steps (not code — Seth, in order)

1. **STT server.** Approve a launchd job running `python3 scripts/stt/mlx_whisper_server.py --port 8761` and `"voice": {"local_stt_endpoint": "http://127.0.0.1:8761/v1/audio/transcriptions", "stt_language": "en"}` in `~/.human/config.json`. (iOS transcripts already cover most memos after Task 1; this is the fallback.)
2. **Deploy** with `scripts/install-human-daemon.sh`, then set `HU_SPEECH_DIRECTION=shadow` in the daemon plist. Watch `direction shadow: ok=… reason=…` lines for a week.
3. **MV7 session (~20 min).** The W2 clone script → re-clone on sonic-3.6; six natural, unscripted memos. A follow-up plan replaces the placeholder voice block in `speech_perform.c` with values and examples measured from these takes.
4. **Ear test.** `human voice preview --direct --baseline-out` on ten real replies; proceed only if you prefer the directed version on ≥ 8/10.
5. **W5, rated by Mindy.** Ask her to rate 12 shuffled clips — your six real takes and six directed renders of comparable lines — real or clone. Pass: ≤ 7/12 correct.
6. **Go live for family** only after W5 passes: `HU_SPEECH_DIRECTION=live`.
