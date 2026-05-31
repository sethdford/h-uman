#ifndef HU_VOICE_LOCAL_TTS_H
#define HU_VOICE_LOCAL_TTS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/*
 * Local (private, on-device) text-to-speech over HTTP.
 *
 * The turnkey target is Kokoro-FastAPI, an OpenAI-compatible TTS server that
 * listens on 127.0.0.1:8880 and exposes `POST /v1/audio/speech` taking a JSON
 * body {"model","voice","input"} and returning audio bytes. Other
 * OpenAI-compatible servers share the same contract. The cross-language
 * boundary is HTTP to a local server on loopback, NOT an embedded TTS FFI (see
 * docs/plans/2026-05-31-local-first-voice/design.md). See
 * docs/guides/local-tts-server.md for the one command to start the server.
 */
typedef struct hu_local_tts_config {
    const char *endpoint; /* e.g. "http://127.0.0.1:8880/v1/audio/speech" */
    const char *model;    /* NULL = omit from JSON (e.g. "kokoro") */
    const char *voice;    /* NULL = omit from JSON (e.g. "af_heart"); Kokoro requires it */
} hu_local_tts_config_t;

/*
 * Build the OpenAI-compatible /v1/audio/speech JSON request body for `text`.
 * Pure: allocates, performs no I/O. On HU_OK, `*out_json` is a NUL-terminated
 * heap string (free with alloc->free, size `*out_len + 1`) of the form
 * {"model":...,"voice":...,"input":...} with `model`/`voice` present only when
 * configured and every value JSON-escaped.
 *
 * Extracted as a pure builder so the request body is unit-testable without
 * spawning curl — see .claude/rules/security-predicate-extraction.md.
 */
hu_error_t hu_local_tts_build_body(hu_allocator_t *alloc, const hu_local_tts_config_t *config,
                                   const char *text, char **out_json, size_t *out_len);

/*
 * Synthesize `text` to a temp audio file; `*out_path` (caller frees with
 * alloc->free, size strlen + 1) is the path written. Posts the build_body JSON
 * to config->endpoint via curl.
 */
hu_error_t hu_local_tts_synthesize(hu_allocator_t *alloc, const hu_local_tts_config_t *config,
                                   const char *text, char **out_path);

#endif
