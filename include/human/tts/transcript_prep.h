#ifndef HU_TTS_TRANSCRIPT_PREP_H
#define HU_TTS_TRANSCRIPT_PREP_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define HU_PREP_MAX_SENTENCES 64
#define HU_PREP_MAX_OUTPUT    8192

typedef struct hu_prep_config {
    const char *incoming_msg;
    size_t incoming_msg_len;
    const char *default_emotion;
    const char *prev_turn_emotion; /* cross-turn emotional momentum; NULL = none */
    float base_speed;
    float pause_factor;
    float discourse_rate;
    bool nonverbals_enabled;
    bool strip_ssml;
    bool thinking_sounds; /* prepend "hmm"/"well" for complex responses */
    uint32_t seed;
    uint8_t hour_local;
} hu_prep_config_t;

typedef struct hu_prep_sentence {
    const char *text;
    size_t len;
    const char *emotion;
    float speed_ratio;
} hu_prep_sentence_t;

typedef struct hu_prep_result {
    char output[HU_PREP_MAX_OUTPUT];
    size_t output_len;
    hu_prep_sentence_t sentences[HU_PREP_MAX_SENTENCES];
    size_t sentence_count;
    const char *dominant_emotion;
    float volume;
    float base_speed; /* request-level speed after late-night adjustment; <speed> tags are relative
                         to it */
} hu_prep_result_t;

/*
 * Preprocess a transcript for Cartesia TTS with SSML annotations.
 *
 * Splits text into sentences, assigns per-sentence emotion, injects
 * SSML <break>, <speed>, and <emotion> tags, weaves discourse markers,
 * and computes volume from emotional context.
 *
 * The output buffer in `result` contains the SSML-annotated transcript
 * ready to send to Cartesia's /tts/bytes endpoint.
 */
hu_error_t hu_transcript_prep(const char *transcript, size_t transcript_len,
                              const hu_prep_config_t *config, hu_prep_result_t *result);

/*
 * Segment text into sentences. Respects abbreviations (Mr., Dr., etc.)
 * and avoids splitting on decimal numbers or ellipsis.
 * Returns sentence count. Sentences point into the original text buffer.
 */
size_t hu_transcript_segment(const char *text, size_t text_len, hu_prep_sentence_t *out,
                             size_t max_sentences);

/*
 * Map emotion enum to a volume multiplier.
 * Calm/empathetic -> softer (0.85-0.9), excited/urgent -> louder (1.1-1.2).
 */
float hu_emotion_to_volume(const char *emotion);

/*
 * Strip stage directions (*actions*), tool JSON blobs, and common
 * emoji-as-icon characters from text before TTS synthesis.
 * Writes cleaned result into `out` (max `cap` bytes). Returns output length.
 */
size_t hu_transcript_strip_junk(const char *text, size_t text_len, char *out, size_t cap);

/*
 * Smooth difficult consonant clusters for clearer TTS pronunciation.
 * Inserts micro-breaks or simplifies sequences like "sts", "thm", "ngths".
 * Writes into `out` (max `cap` bytes). Returns output length.
 */
size_t hu_transcript_smooth_consonants(const char *text, size_t text_len, char *out, size_t cap,
                                       bool strip_ssml);

/*
 * Normalize text for spoken delivery. Rewrites numbers, dates, times,
 * currency, and phone numbers into speech-friendly forms:
 *   - Small integers (0-99) → words ("forty two")
 *   - Phone numbers → <spell> tags with breaks
 *   - Dates (MM/DD/YYYY, YYYY-MM-DD) → spoken form ("April third, twenty twenty-six")
 *   - Times (7:30 PM) → spoken form ("seven thirty PM")
 *   - Currency ($42.50) → "forty-two dollars and fifty cents"
 *   - Percentages (85%) → "eighty-five percent"
 * When strip_ssml is true, <spell> tags are omitted and raw text is used.
 * Writes into `out` (max `cap` bytes). Returns output length.
 */
size_t hu_transcript_normalize_for_speech(const char *text, size_t text_len, char *out, size_t cap,
                                          bool strip_ssml);

/*
 * Limit break density in SSML output. Scans for <break> tags and removes
 * excess breaks that exceed `max_breaks_per_100_chars`. Returns new length.
 * Operates in-place on `buf` (null-terminated, length `len`).
 */
size_t hu_transcript_limit_breaks(char *buf, size_t len, int max_breaks_per_100_chars);

#endif /* HU_TTS_TRANSCRIPT_PREP_H */
