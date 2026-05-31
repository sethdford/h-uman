#ifndef HU_VOICE_LOCAL_STT_H
#define HU_VOICE_LOCAL_STT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/*
 * Local (private, on-device) speech-to-text over HTTP.
 *
 * The turnkey target is whisper.cpp's bundled `whisper-server`, which listens
 * on 127.0.0.1:8080 and exposes `POST /inference` with a multipart `file`
 * field, returning {"text": "..."} for its default json response_format.
 * OpenAI-compatible transcription servers (`POST /v1/audio/transcriptions`,
 * e.g. speaches / faster-whisper-server / LocalAI) also work — those require a
 * `model` field, so set `model` for them.
 *
 * The cross-language boundary is HTTP to a local server, NOT an embedded
 * Whisper FFI binding (see docs/plans/2026-05-31-local-first-voice/design.md
 * and the cross-language-via-http rule). See docs/guides/local-stt-server.md
 * for the one command to start the server.
 */
typedef struct hu_local_stt_config {
    const char *endpoint; /* e.g. "http://127.0.0.1:8080/inference" (whisper.cpp) */
    const char *model;    /* NULL = omit (whisper.cpp loads its model at startup via -m) */
    const char *language; /* NULL = omit (auto-detect) */
} hu_local_stt_config_t;

/* Upper bounds for a built request: curl + flags + (2 per form field) + endpoint
 * + NULL terminator must fit in argv; owned holds the heap-formatted fields. */
#define HU_LOCAL_STT_MAX_ARGV  16
#define HU_LOCAL_STT_MAX_OWNED 4

/*
 * A fully-built curl invocation for one STT request. `argv` is NULL-terminated
 * and ready to hand to hu_process_run; its entries either point into `owned[]`
 * (heap strings this struct owns) or into caller-owned config strings. Build
 * with hu_local_stt_build_request, release with hu_local_stt_request_free.
 *
 * This is extracted as a pure, side-effect-free builder so the request shape
 * (form fields + endpoint) is unit-testable without spawning curl or touching
 * the network — see .claude/rules/security-predicate-extraction.md.
 */
typedef struct hu_local_stt_request {
    const char *argv[HU_LOCAL_STT_MAX_ARGV];
    size_t argc; /* number of non-NULL argv entries */
    char *owned[HU_LOCAL_STT_MAX_OWNED];
    size_t owned_sizes[HU_LOCAL_STT_MAX_OWNED];
    size_t owned_count;
} hu_local_stt_request_t;

/*
 * Build the curl argv for transcribing `audio_path` against `config`. Pure:
 * allocates strings, performs no I/O or process spawning. Always emits
 * `file=@<audio_path>` and `response_format=json`; emits `model`/`language`
 * only when the corresponding config field is non-empty. On HU_OK, `*out_req`
 * must be released with hu_local_stt_request_free.
 */
hu_error_t hu_local_stt_build_request(hu_allocator_t *alloc, const hu_local_stt_config_t *config,
                                      const char *audio_path, hu_local_stt_request_t *out_req);

/* Free heap strings held by a request built with hu_local_stt_build_request.
 * Safe to call on a zeroed/failed request. */
void hu_local_stt_request_free(hu_allocator_t *alloc, hu_local_stt_request_t *req);

/*
 * Transcribe `audio_path` and return the recognized text (caller frees
 * `*out_text` with alloc->free, size `*out_len + 1`). Posts to config->endpoint
 * via curl. Returns HU_ERR_INVALID_ARGUMENT on bad args, HU_ERR_PROVIDER_RESPONSE
 * on empty server output, HU_ERR_PARSE on a malformed/textless JSON body.
 */
hu_error_t hu_local_stt_transcribe(hu_allocator_t *alloc, const hu_local_stt_config_t *config,
                                   const char *audio_path, char **out_text, size_t *out_len);

#endif
