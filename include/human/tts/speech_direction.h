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
    char emotion[24];  /* "" = unchanged */
    float speed;       /* 0 = unchanged */
    float volume;      /* 0 = unchanged */
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
    uint8_t max_breaks; /* Sonic paces from punctuation; extra breaks are dropped */
    uint8_t max_laughs, max_speed_tags, max_volume_tags;
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
/* voiceai 2026-09-27: only calm emotions reach Sonic ("excited" widened the
 * clone's pitch range to 10.9 semitones vs 6.4 for "sympathetic"). */
bool hu_direction_emotion_is_calm(const char *s, size_t n);
size_t hu_direction_calm_count(void);
const char *hu_direction_calm_at(size_t i);
/* lim NULL = defaults. `out` is fully overwritten. */
hu_direction_verdict_t hu_direction_parse(const char *line, size_t len,
                                          const hu_direction_limits_t *lim, hu_direction_t *out);
const char *hu_direction_verdict_name(hu_direction_verdict_t v);
/* The first segment's emotion, or NULL. */
const char *hu_direction_first_emotion(const hu_direction_t *d);

typedef enum { HU_LAUGH_CARTESIA = 0, HU_LAUGH_TEXT } hu_laugh_style_t;
/* HU_VOICE_LAUGH: "text" writes a spoken laugh; anything else uses Cartesia's
 * [laughter] (the ear A/B decides — voiceai avoids the stock laugh). */
hu_laugh_style_t hu_laugh_style_parse(const char *s);
/* D3: canonical Cartesia transcript re-emitted from the parsed values, each
 * segment's words normalized for speech. Returns the length (0 on overflow). */
size_t hu_direction_render(const hu_direction_t *d, hu_laugh_style_t laugh, char *out, size_t cap);
/* For logs (no words): "emotions=a,b breaks=N laughs=N speed=N volume=N". */
size_t hu_direction_summary(const hu_direction_t *d, char *out, size_t cap);
#endif
