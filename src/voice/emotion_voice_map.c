#include "human/voice/emotion_voice_map.h"
#include "human/core/string.h"
#include <string.h>

hu_voice_params_t hu_voice_params_default(void) {
    return (hu_voice_params_t){
        .pitch_shift = 0.0f,
        .rate_factor = 1.0f,
        .emphasis = 0.5f,
        .warmth = 0.5f,
        .pause_factor = 1.0f,
    };
}

static const hu_voice_params_t EMOTION_MAP[HU_VOICE_EMOTION_COUNT] = {
    [HU_VOICE_EMOTION_NEUTRAL] = {0.0f, 1.0f, 0.5f, 0.5f, 1.0f},
    [HU_VOICE_EMOTION_JOY] = {1.5f, 1.1f, 0.8f, 0.8f, 0.8f},
    [HU_VOICE_EMOTION_SADNESS] = {-1.0f, 0.85f, 0.3f, 0.7f, 1.4f},
    [HU_VOICE_EMOTION_EMPATHY] = {-0.5f, 0.9f, 0.4f, 0.9f, 1.2f},
    [HU_VOICE_EMOTION_EXCITEMENT] = {2.0f, 1.2f, 0.9f, 0.6f, 0.6f},
    [HU_VOICE_EMOTION_CONCERN] = {-0.3f, 0.95f, 0.6f, 0.7f, 1.1f},
    [HU_VOICE_EMOTION_CALM] = {-0.2f, 0.9f, 0.3f, 0.8f, 1.3f},
    [HU_VOICE_EMOTION_URGENCY] = {0.5f, 1.15f, 0.85f, 0.4f, 0.7f},
};

hu_voice_params_t hu_emotion_voice_map(hu_voice_emotion_t emotion) {
    if (emotion < 0 || emotion >= HU_VOICE_EMOTION_COUNT)
        return hu_voice_params_default();
    return EMOTION_MAP[emotion];
}

typedef struct {
    const char *keyword;
    size_t len;
    hu_voice_emotion_t emotion;
    float weight;
} lexical_cue_t;

static const lexical_cue_t CUES[] = {
    {"happy", 5, HU_VOICE_EMOTION_JOY, 0.7f},
    {"glad", 4, HU_VOICE_EMOTION_JOY, 0.6f},
    {"great", 5, HU_VOICE_EMOTION_JOY, 0.5f},
    {"wonderful", 9, HU_VOICE_EMOTION_JOY, 0.7f},
    {"excited", 7, HU_VOICE_EMOTION_EXCITEMENT, 0.8f},
    {"amazing", 7, HU_VOICE_EMOTION_EXCITEMENT, 0.7f},
    {"fantastic", 9, HU_VOICE_EMOTION_EXCITEMENT, 0.7f},
    {"sorry", 5, HU_VOICE_EMOTION_EMPATHY, 0.7f},
    {"understand", 10, HU_VOICE_EMOTION_EMPATHY, 0.5f},
    {"feel", 4, HU_VOICE_EMOTION_EMPATHY, 0.3f},
    {"sad", 3, HU_VOICE_EMOTION_SADNESS, 0.7f},
    {"unfortunately", 13, HU_VOICE_EMOTION_SADNESS, 0.6f},
    {"loss", 4, HU_VOICE_EMOTION_SADNESS, 0.5f},
    {"worried", 7, HU_VOICE_EMOTION_CONCERN, 0.7f},
    {"careful", 7, HU_VOICE_EMOTION_CONCERN, 0.5f},
    {"warning", 7, HU_VOICE_EMOTION_CONCERN, 0.6f},
    {"urgent", 6, HU_VOICE_EMOTION_URGENCY, 0.8f},
    {"immediately", 11, HU_VOICE_EMOTION_URGENCY, 0.7f},
    {"critical", 8, HU_VOICE_EMOTION_URGENCY, 0.7f},
    {"calm", 4, HU_VOICE_EMOTION_CALM, 0.6f},
    {"relax", 5, HU_VOICE_EMOTION_CALM, 0.6f},
    {"peace", 5, HU_VOICE_EMOTION_CALM, 0.5f},
};
static const size_t CUE_COUNT = sizeof(CUES) / sizeof(CUES[0]);

hu_error_t hu_emotion_detect_from_text(const char *text, size_t text_len,
                                       hu_voice_emotion_t *out_emotion, float *out_confidence) {
    if (!text || !out_emotion || !out_confidence)
        return HU_ERR_INVALID_ARGUMENT;
    *out_emotion = HU_VOICE_EMOTION_NEUTRAL;
    *out_confidence = 0.0f;

    if (text_len == 0)
        return HU_OK;

    float scores[HU_VOICE_EMOTION_COUNT];
    memset(scores, 0, sizeof(scores));

    for (size_t i = 0; i < CUE_COUNT; i++) {
        if (hu_str_contains_ci(text, text_len, CUES[i].keyword, CUES[i].len))
            scores[CUES[i].emotion] += CUES[i].weight;
    }

    for (size_t i = 0; i < text_len; i++) {
        if (text[i] == '!')
            scores[HU_VOICE_EMOTION_EXCITEMENT] += 0.15f;
    }

    float max_score = 0.0f;
    hu_voice_emotion_t best = HU_VOICE_EMOTION_NEUTRAL;
    for (int e = 1; e < HU_VOICE_EMOTION_COUNT; e++) {
        if (scores[e] > max_score) {
            max_score = scores[e];
            best = (hu_voice_emotion_t)e;
        }
    }

    if (max_score >= 0.3f) {
        *out_emotion = best;
        *out_confidence = max_score > 1.0f ? 1.0f : max_score;
    }
    return HU_OK;
}

hu_voice_params_t hu_voice_params_blend(const hu_voice_params_t *a, const hu_voice_params_t *b,
                                        float factor) {
    if (!a || !b)
        return hu_voice_params_default();
    float inv = 1.0f - factor;
    return (hu_voice_params_t){
        .pitch_shift = a->pitch_shift * inv + b->pitch_shift * factor,
        .rate_factor = a->rate_factor * inv + b->rate_factor * factor,
        .emphasis = a->emphasis * inv + b->emphasis * factor,
        .warmth = a->warmth * inv + b->warmth * factor,
        .pause_factor = a->pause_factor * inv + b->pause_factor * factor,
    };
}

static const char *const EMOTION_NAMES[HU_VOICE_EMOTION_COUNT] = {
    "neutral", "joy", "sadness", "empathy", "excitement", "concern", "calm", "urgency",
};

const char *hu_emotion_class_name(hu_voice_emotion_t emotion) {
    if (emotion < 0 || emotion >= HU_VOICE_EMOTION_COUNT)
        return "unknown";
    return EMOTION_NAMES[emotion];
}
