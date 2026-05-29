#include "human/voice/semantic_eot.h"
#include "human/core/string.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void hu_semantic_eot_config_default(hu_semantic_eot_config_t *cfg) {
    if (!cfg)
        return;
    cfg->min_utterance_chars = 2;
    cfg->pause_threshold_ms = 400;
    cfg->confidence_threshold = 0.6;
}

static bool ends_with_char(const char *text, size_t len, char c) {
    while (len > 0 && isspace((unsigned char)text[len - 1]))
        len--;
    return len > 0 && text[len - 1] == c;
}

static bool ends_with_str(const char *text, size_t len, const char *suffix) {
    size_t slen = strlen(suffix);
    while (len > 0 && isspace((unsigned char)text[len - 1]))
        len--;
    return len >= slen && memcmp(text + len - slen, suffix, slen) == 0;
}

static size_t trimmed_len(const char *text, size_t len) {
    while (len > 0 && isspace((unsigned char)text[len - 1]))
        len--;
    return len;
}

static size_t count_words(const char *text, size_t len) {
    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && isspace((unsigned char)text[i]))
            i++;
        if (i >= len)
            break;
        n++;
        while (i < len && !isspace((unsigned char)text[i]))
            i++;
    }
    return n;
}

static bool syntax_complete_clause(const char *text, size_t text_len) {
    return ends_with_char(text, text_len, '.') || ends_with_char(text, text_len, '!') ||
           ends_with_char(text, text_len, '?');
}

static bool is_backchannel_phrase(const char *text, size_t tlen) {
    return hu_str_contains_ci_cstr(text, tlen, "uh huh") || hu_str_contains_ci_cstr(text, tlen, "uh-huh") ||
           hu_str_contains_ci_cstr(text, tlen, "mhm") || hu_str_contains_ci_cstr(text, tlen, "mm-hmm") ||
           hu_str_contains_ci_cstr(text, tlen, "yeah") || hu_str_contains_ci_cstr(text, tlen, "okay") ||
           hu_str_contains_ci_cstr(text, tlen, "ok ") || hu_str_contains_ci_cstr(text, tlen, "right") ||
           hu_str_contains_ci_cstr(text, tlen, "i see") || hu_str_contains_ci_cstr(text, tlen, "got it");
}

static bool has_hold_fillers(const char *text, size_t tlen) {
    return hu_str_contains_ci_cstr(text, tlen, " um") || hu_str_contains_ci_cstr(text, tlen, " uh") ||
           hu_str_contains_ci_cstr(text, tlen, " like ") || hu_str_contains_ci_cstr(text, tlen, "you know");
}

hu_error_t hu_semantic_eot_analyze(const hu_semantic_eot_config_t *cfg, const char *text,
                                   size_t text_len, uint32_t silence_ms,
                                   hu_semantic_eot_result_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->is_endpoint = false;
    out->confidence = 0.0;
    out->suggested_signal = HU_TURN_SIGNAL_NONE;
    out->predicted_state = HU_EOT_COMPLETE;

    if (!cfg || !text || text_len == 0)
        return HU_OK;

    size_t tlen = trimmed_len(text, text_len);
    if (tlen < cfg->min_utterance_chars)
        return HU_OK;

    double conf = 0.0;

    /* Syntactic completeness: sentence-ending punctuation */
    if (ends_with_char(text, text_len, '.') || ends_with_char(text, text_len, '!'))
        conf += 0.35;
    else if (ends_with_char(text, text_len, '?'))
        conf += 0.45;

    /* Trailing ellipsis = speaker holding */
    if (ends_with_str(text, text_len, "...")) {
        out->predicted_state = HU_EOT_HOLD;
        out->confidence = 0.15;
        out->suggested_signal = HU_TURN_SIGNAL_HOLD;
        out->is_endpoint = false;
        return HU_OK;
    }

    /* Lexical yielding phrases */
    if (hu_str_contains_ci_cstr(text, tlen, "what do you think") || hu_str_contains_ci_cstr(text, tlen, "your thoughts") ||
        hu_str_contains_ci_cstr(text, tlen, "can you help") || hu_str_contains_ci_cstr(text, tlen, "do you know"))
        conf += 0.25;

    /* Short complete utterance (under ~40 chars with punctuation) */
    if (tlen < 40 && conf >= 0.3)
        conf += 0.10;

    /* Silence correlation: more silence → higher confidence */
    if (silence_ms >= cfg->pause_threshold_ms)
        conf += 0.20;
    else if (silence_ms >= cfg->pause_threshold_ms / 2)
        conf += 0.10;

    /* Clamp */
    if (conf > 1.0)
        conf = 1.0;

    out->confidence = conf;
    out->is_endpoint = (conf >= cfg->confidence_threshold);

    if (out->is_endpoint) {
        out->suggested_signal =
            ends_with_char(text, text_len, '?') ? HU_TURN_SIGNAL_YIELD : HU_TURN_SIGNAL_CONTINUE;
    }

    return HU_OK;
}

hu_error_t hu_semantic_eot_analyze_with_audio(const hu_semantic_eot_config_t *cfg, const char *text,
                                              size_t text_len, uint32_t silence_ms, float energy_db,
                                              float pitch_delta, hu_semantic_eot_result_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->is_endpoint = false;
    out->confidence = 0.0;
    out->suggested_signal = HU_TURN_SIGNAL_NONE;
    out->predicted_state = HU_EOT_COMPLETE;

    if (!cfg || !text || text_len == 0)
        return HU_OK;

    size_t tlen = trimmed_len(text, text_len);
    if (tlen < cfg->min_utterance_chars)
        return HU_OK;

    /* HOLD: ellipsis or strong filler presence */
    if (ends_with_str(text, text_len, "...") ||
        (has_hold_fillers(text, tlen) && !syntax_complete_clause(text, text_len))) {
        out->predicted_state = HU_EOT_HOLD;
        out->confidence = 0.15;
        out->suggested_signal = HU_TURN_SIGNAL_HOLD;
        return HU_OK;
    }

    /* BACKCHANNEL: very short + lexical cue */
    if (count_words(text, tlen) < 5 && is_backchannel_phrase(text, tlen)) {
        out->predicted_state = HU_EOT_BACKCHANNEL;
        out->confidence = 0.55;
        out->is_endpoint = false;
        out->suggested_signal = HU_TURN_SIGNAL_CONTINUE;
        return HU_OK;
    }

    bool syn_complete = syntax_complete_clause(text, text_len);
    double conf = 0.0;

    if (syn_complete) {
        if (ends_with_char(text, text_len, '?'))
            conf += 0.45;
        else
            conf += 0.35;
    }

    if (hu_str_contains_ci_cstr(text, tlen, "what do you think") || hu_str_contains_ci_cstr(text, tlen, "your thoughts") ||
        hu_str_contains_ci_cstr(text, tlen, "can you help") || hu_str_contains_ci_cstr(text, tlen, "do you know"))
        conf += 0.25;

    if (tlen < 40 && conf >= 0.3)
        conf += 0.10;

    if (silence_ms >= cfg->pause_threshold_ms)
        conf += 0.20;
    else if (silence_ms >= cfg->pause_threshold_ms / 2)
        conf += 0.10;

    /* Acoustic fusion: energy and pitch each adjust confidence by up to 0.15 */
    double econtrib = 0.0;
    if (energy_db > -25.0f)
        econtrib = 0.15 * fmin(1.0, (double)(energy_db + 25.0f) / 25.0);
    else if (energy_db < -40.0f)
        econtrib = -0.15 * fmin(1.0, (double)(-40.0f - energy_db) / 20.0);

    double pcontrib = 0.15 * fmin(1.0, fabs((double)pitch_delta) / 80.0);
    if (pitch_delta < 0)
        pcontrib = -pcontrib;

    conf += econtrib + pcontrib;

    /* Multi-class state from text + acoustics */
    bool high_energy = energy_db > -28.0f;
    bool rising_pitch = pitch_delta > 8.0f;
    bool low_energy = energy_db < -38.0f;
    bool falling_pitch = pitch_delta < -8.0f;

    if (high_energy && rising_pitch && !syn_complete) {
        out->predicted_state = HU_EOT_INCOMPLETE;
        conf = fmax(conf, 0.35);
    } else if (low_energy && falling_pitch && syn_complete) {
        out->predicted_state = HU_EOT_COMPLETE;
        conf += 0.08;
    }

    if (conf > 1.0)
        conf = 1.0;
    if (conf < 0.0)
        conf = 0.0;

    out->confidence = conf;
    out->is_endpoint =
        (out->predicted_state == HU_EOT_COMPLETE && conf >= cfg->confidence_threshold);

    if (out->predicted_state == HU_EOT_INCOMPLETE) {
        out->is_endpoint = false;
        out->suggested_signal = HU_TURN_SIGNAL_CONTINUE;
        return HU_OK;
    }

    if (out->is_endpoint) {
        out->suggested_signal =
            ends_with_char(text, text_len, '?') ? HU_TURN_SIGNAL_YIELD : HU_TURN_SIGNAL_CONTINUE;
    }

    return HU_OK;
}

/*
 * Learned feature-based EOT classifier — logistic regression over engineered features.
 * Default weights calibrated from conversational turn-taking corpora.
 * Feature indices:
 *   0: syntax_complete    1: question_mark    2: ellipsis
 *   3: yield_phrase        4: backchannel      5: hold_filler
 *   6: word_count_norm     7: silence_norm     8: energy_norm
 *   9: pitch_norm
 */

void hu_semantic_eot_classifier_default(hu_semantic_eot_classifier_t *cls) {
    if (!cls)
        return;
    cls->weights[0] = 1.8f;
    cls->weights[1] = 2.1f;
    cls->weights[2] = -2.5f;
    cls->weights[3] = 1.4f;
    cls->weights[4] = -1.0f;
    cls->weights[5] = -1.2f;
    cls->weights[6] = 0.3f;
    cls->weights[7] = 1.6f;
    cls->weights[8] = -0.4f;
    cls->weights[9] = -0.6f;
    cls->bias = -1.2f;
    cls->threshold = 0.55f;
}

hu_error_t hu_semantic_eot_set_weights(hu_semantic_eot_classifier_t *cls, const float *weights,
                                       size_t dim, float bias, float threshold) {
    if (!cls || !weights || dim != HU_EOT_FEATURE_DIM)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < HU_EOT_FEATURE_DIM; i++)
        cls->weights[i] = weights[i];
    cls->bias = bias;
    cls->threshold = threshold;
    return HU_OK;
}

static float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static float sigmoid_f(float x) {
    if (x > 20.0f)
        return 1.0f;
    if (x < -20.0f)
        return 0.0f;
    return 1.0f / (1.0f + expf(-x));
}

hu_error_t hu_semantic_eot_extract_features(const char *text, size_t text_len, uint32_t silence_ms,
                                            float energy_db, float pitch_delta,
                                            float *out_features) {
    if (!out_features)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_features, 0, sizeof(float) * HU_EOT_FEATURE_DIM);

    if (!text || text_len == 0)
        return HU_OK;

    size_t tlen = trimmed_len(text, text_len);

    out_features[0] = syntax_complete_clause(text, text_len) ? 1.0f : 0.0f;
    out_features[1] = ends_with_char(text, text_len, '?') ? 1.0f : 0.0f;
    out_features[2] = ends_with_str(text, text_len, "...") ? 1.0f : 0.0f;
    out_features[3] =
        (hu_str_contains_ci_cstr(text, tlen, "what do you think") || hu_str_contains_ci_cstr(text, tlen, "your thoughts") ||
         hu_str_contains_ci_cstr(text, tlen, "can you help") || hu_str_contains_ci_cstr(text, tlen, "do you know"))
            ? 1.0f
            : 0.0f;
    out_features[4] = is_backchannel_phrase(text, tlen) ? 1.0f : 0.0f;
    out_features[5] = has_hold_fillers(text, tlen) ? 1.0f : 0.0f;
    out_features[6] = clamp01((float)count_words(text, tlen) / 20.0f);
    out_features[7] = clamp01((float)silence_ms / 1000.0f);
    out_features[8] = clamp01((energy_db + 50.0f) / 50.0f);
    out_features[9] = clamp01((pitch_delta + 100.0f) / 200.0f);

    return HU_OK;
}

hu_error_t hu_semantic_eot_classify(const hu_semantic_eot_classifier_t *cls,
                                    const hu_semantic_eot_config_t *cfg, const char *text,
                                    size_t text_len, uint32_t silence_ms, float energy_db,
                                    float pitch_delta, hu_semantic_eot_result_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;

    if (!cls)
        return hu_semantic_eot_analyze_with_audio(cfg, text, text_len, silence_ms, energy_db,
                                                  pitch_delta, out);

    out->is_endpoint = false;
    out->confidence = 0.0;
    out->suggested_signal = HU_TURN_SIGNAL_NONE;
    out->predicted_state = HU_EOT_COMPLETE;

    if (!text || text_len == 0)
        return HU_OK;

    float features[HU_EOT_FEATURE_DIM];
    hu_error_t err = hu_semantic_eot_extract_features(text, text_len, silence_ms, energy_db,
                                                      pitch_delta, features);
    if (err != HU_OK)
        return err;

    float logit = cls->bias;
    for (size_t i = 0; i < HU_EOT_FEATURE_DIM; i++)
        logit += cls->weights[i] * features[i];

    float prob = sigmoid_f(logit);
    out->confidence = (double)prob;

    if (features[2] > 0.5f || (features[5] > 0.5f && features[0] < 0.5f)) {
        out->predicted_state = HU_EOT_HOLD;
        out->suggested_signal = HU_TURN_SIGNAL_HOLD;
        out->is_endpoint = false;
        return HU_OK;
    }

    size_t tlen = trimmed_len(text, text_len);
    if (features[4] > 0.5f && count_words(text, tlen) < 5) {
        out->predicted_state = HU_EOT_BACKCHANNEL;
        out->suggested_signal = HU_TURN_SIGNAL_CONTINUE;
        out->is_endpoint = false;
        return HU_OK;
    }

    if (energy_db > -28.0f && pitch_delta > 8.0f && features[0] < 0.5f) {
        out->predicted_state = HU_EOT_INCOMPLETE;
        out->suggested_signal = HU_TURN_SIGNAL_CONTINUE;
        out->is_endpoint = false;
        return HU_OK;
    }

    out->is_endpoint = (prob >= cls->threshold);
    if (out->is_endpoint)
        out->suggested_signal = features[1] > 0.5f ? HU_TURN_SIGNAL_YIELD : HU_TURN_SIGNAL_CONTINUE;

    return HU_OK;
}
