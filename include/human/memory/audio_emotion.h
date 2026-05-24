/* include/human/memory/audio_emotion.h
 *
 * Multi-modal audio emotion (heuristic stub) — Sprint B Story 5
 * (2026-05-19).
 *
 * Goal: voice messages carry pitch/pause/speech-rate info that the
 * transcript loses. An audio-aware model would emit "Alice's last
 * voice message sounded anxious" as a fact alongside the transcript.
 *
 * THIS MODULE IS A HEURISTIC STUB. Real pitch/spectral analysis
 * requires audio infra (whisper-pitch, librosa) we haven't shipped.
 * The heuristic gives the daemon a wire point + the same output enum
 * a future model would produce, so caller code (persona prompt,
 * fact-extract) can be wired now and the real model dropped in later
 * with zero call-site changes.
 *
 * The heuristic uses just two inputs the transcript pipeline ALREADY
 * has: duration_seconds + word_count. Speech rate (words per minute)
 * is a noisy but real signal — anxious / excited speech tends to be
 * fast (180+ wpm); deliberate / sad / contemplative tends to be slow
 * (<100 wpm). Long durations with low word counts suggest hesitation
 * or non-verbal pauses (sighs, "umm").
 *
 * Not detected (require real audio model):
 *   - Absolute pitch level
 *   - Tremor / vocal stress markers
 *   - Cry / laugh / shout
 *   - Volume changes within message
 */
#ifndef HU_MEMORY_AUDIO_EMOTION_H
#define HU_MEMORY_AUDIO_EMOTION_H

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hu_audio_tone {
    HU_AUDIO_TONE_UNKNOWN = 0,
    HU_AUDIO_TONE_DELIBERATE, /* slow speech, possibly tired/contemplative */
    HU_AUDIO_TONE_NEUTRAL,    /* 100-180 wpm, normal conversational pace */
    HU_AUDIO_TONE_ENERGETIC,  /* >180 wpm, possibly excited/anxious */
    HU_AUDIO_TONE_HESITANT,   /* low wpm relative to duration — long pauses */
} hu_audio_tone_t;

/* Classify by transcript stats. Pure, deterministic.
 *
 *   duration_seconds — total recording length (must be > 0 to be useful)
 *   word_count       — words in transcript (must be > 0 to be useful)
 *
 * Returns HU_AUDIO_TONE_UNKNOWN when inputs are insufficient. */
hu_audio_tone_t hu_audio_tone_classify(double duration_seconds, int word_count);

/* Human-readable label for the enum. Returns a borrowed string;
 * never NULL. */
const char *hu_audio_tone_label(hu_audio_tone_t tone);

/* Render the tone as a single-line prompt block:
 *   "VOICE TONE: <handle>'s last voice message sounded <label>."
 *
 * Returns bytes written. Returns 0 for UNKNOWN tone (no signal to
 * surface). */
size_t hu_audio_tone_render(const char *contact_handle, hu_audio_tone_t tone, char *out,
                            size_t cap);

/* B5 production wire helper: format the (contact, tone) pair as a
 * canonical English fact suitable for hu_personal_model_ingest. Returns
 * bytes written. Output shape:
 *   "<handle>'s voice message sounded <label>."
 *
 * Caller is expected to feed the result into the personal-model
 * ingest pipeline (which will then fact-extract subject/predicate/
 * object and stamp provenance). For UNKNOWN tone returns 0 (no
 * meaningful signal to surface). */
size_t hu_audio_tone_format_fact(const char *contact_handle, hu_audio_tone_t tone, char *out,
                                 size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* HU_MEMORY_AUDIO_EMOTION_H */
