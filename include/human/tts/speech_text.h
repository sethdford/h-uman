#ifndef HU_TTS_SPEECH_TEXT_H
#define HU_TTS_SPEECH_TEXT_H

/*
 * F1 spoken memos (spec 2026-09-27): what a voice memo is allowed to say.
 *
 * S2 — hu_speech_cleanup: the spoken form of a reply. Texting shorthand is
 * expanded (lmk -> "let me know"), narrated actions (*laughs*, (sighs)),
 * brackets, emoji and URLs never reach TTS, and laugh tokens (lol, haha) are
 * removed but reported as a laughter cue for transcript prep.
 *
 * S3 — hu_speech_drift_check: a rewrite-for-the-ear may change HOW the reply
 * is said, never WHAT. It is rejected when it adds a number, time or name,
 * changes whether it asks a question, drifts too far in length, or uses a
 * banned opener / stage direction.
 *
 * All matching is word-boundary and case-insensitive.
 */

#include <stdbool.h>
#include <stddef.h>

/* Returns the output length (0 = nothing speakable). out is always
 * NUL-terminated when cap > 0. laughter_cue may be NULL. */
size_t hu_speech_cleanup(const char *in, size_t in_len, char *out, size_t cap, bool *laughter_cue);

typedef enum {
    HU_SPEECH_DRIFT_OK = 0,
    HU_SPEECH_DRIFT_NEW_NUMBER,
    HU_SPEECH_DRIFT_NEW_NAME,
    HU_SPEECH_DRIFT_QUESTION,
    HU_SPEECH_DRIFT_LENGTH,
    HU_SPEECH_DRIFT_BANNED,
} hu_speech_drift_t;

hu_speech_drift_t hu_speech_drift_check(const char *original, size_t original_len,
                                        const char *rewritten, size_t rewritten_len);
const char *hu_speech_drift_name(hu_speech_drift_t d);

#endif /* HU_TTS_SPEECH_TEXT_H */
