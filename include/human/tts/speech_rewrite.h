#ifndef HU_TTS_SPEECH_REWRITE_H
#define HU_TTS_SPEECH_REWRITE_H

/*
 * F1 S1 (spec 2026-09-27): rewrite a voice reply for the ear.
 *
 * A reply was written as a text message; a memo should sound like the person
 * SAYING it. One LLM call turns the cleaned reply into speech — Ferni's
 * mechanics (voiceai voice-guidance.md, dynamic-speech-guidance.ts) in the
 * persona's own register — then the result is cleaned again (S2) and must
 * pass the drift guard (S3) against the cleaned original, or the cleaned
 * original is spoken instead.
 *
 * HU_SPEECH_REWRITE: off (default) | shadow (call + log, speak cleanup) | live.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_persona;

typedef enum {
    HU_SPEECH_REWRITE_OFF = 0,
    HU_SPEECH_REWRITE_SHADOW,
    HU_SPEECH_REWRITE_LIVE,
} hu_speech_rewrite_mode_t;

hu_speech_rewrite_mode_t hu_speech_rewrite_mode_parse(const char *s);

typedef struct {
    char spoken[2048]; /* what to speak; empty = nothing speakable */
    size_t spoken_len;
    char rewritten[2048]; /* the model's cleaned rewrite, "" when none */
    bool used_rewrite;
    bool laughter_cue;
    const char *reason; /* static: off, live, shadow, no_provider, provider_error,
                           empty, nothing_to_say, or a drift reason */
} hu_speech_result_t;

/* System prompt: the mechanics block plus the persona's style lines. */
size_t hu_speech_rewrite_system_prompt(const struct hu_persona *persona, char *out, size_t cap);

/* S1 -> S2 -> S3. provider may be NULL. Never fails for content reasons: any
 * rewrite problem falls back to the cleaned reply (see out->reason). */
hu_error_t hu_speech_prepare(hu_allocator_t *alloc, const hu_provider_t *provider,
                             const char *model, size_t model_len, const struct hu_persona *persona,
                             hu_speech_rewrite_mode_t mode, const char *reply, size_t reply_len,
                             const char *inbound, size_t inbound_len, hu_speech_result_t *out);

/* One JSON line for the SHADOW log: {ts, used, reason, spoken, rewritten}.
 * Never includes the inbound message. Caller frees with alloc (len + 1). */
hu_error_t hu_speech_shadow_line(hu_allocator_t *alloc, const hu_speech_result_t *r, int64_t ts,
                                 char **out, size_t *out_len);

/* Append that line to <state>/voice/shadow/<YYYY-MM-DD>.jsonl (no-op in tests). */
void hu_speech_shadow_record(hu_allocator_t *alloc, const hu_speech_result_t *r);

#endif /* HU_TTS_SPEECH_REWRITE_H */
