#include "human/behavior/affect.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

/* Tiny lexicon — designed for transparency, not scale. Production callers
 * should fuse multimodal evidence (B12) before relying on this baseline.
 */
struct affect_lex_entry {
    const char *word;
    float valence;
    float arousal;
    float dominance;
};

static const struct affect_lex_entry HU_AFFECT_LEX[] = {
    /* word                v       a       d   */
    {"happy",            0.75f,  0.50f,  0.55f},
    {"glad",             0.55f,  0.40f,  0.50f},
    {"love",             0.85f,  0.55f,  0.55f},
    {"calm",             0.30f,  0.10f,  0.55f},
    {"grateful",         0.70f,  0.40f,  0.55f},
    {"excited",          0.60f,  0.85f,  0.65f},
    {"anxious",         -0.50f,  0.80f,  0.30f},
    {"worried",         -0.45f,  0.70f,  0.30f},
    {"scared",          -0.60f,  0.85f,  0.20f},
    {"afraid",          -0.55f,  0.75f,  0.25f},
    {"angry",           -0.65f,  0.85f,  0.55f},
    {"furious",         -0.85f,  0.95f,  0.60f},
    {"frustrated",      -0.50f,  0.70f,  0.40f},
    {"sad",             -0.65f,  0.30f,  0.30f},
    {"depressed",       -0.80f,  0.20f,  0.20f},
    {"hopeless",        -0.85f,  0.30f,  0.10f},
    {"tired",           -0.20f,  0.20f,  0.35f},
    {"exhausted",       -0.30f,  0.15f,  0.30f},
    {"lonely",          -0.60f,  0.40f,  0.25f},
    {"overwhelmed",     -0.55f,  0.85f,  0.25f},
    {"crisis",          -0.85f,  0.90f,  0.15f},
    {"stressed",        -0.55f,  0.75f,  0.30f},
    {"hurt",            -0.55f,  0.55f,  0.30f},
    {"thankful",         0.65f,  0.40f,  0.55f},
    {"hopeful",          0.55f,  0.45f,  0.60f},
    {"confident",        0.55f,  0.55f,  0.80f},
    {"proud",            0.65f,  0.55f,  0.75f},
    {"helpless",        -0.65f,  0.55f,  0.10f},
    {"meh",             -0.05f,  0.10f,  0.45f},
    {"fine",             0.10f,  0.20f,  0.55f},
    {"okay",             0.10f,  0.20f,  0.55f},
    {"good",             0.45f,  0.30f,  0.55f},
    {"great",            0.65f,  0.55f,  0.60f},
    {"awful",           -0.65f,  0.55f,  0.30f},
    {"terrible",        -0.70f,  0.55f,  0.30f},
};

static const size_t HU_AFFECT_LEX_LEN = sizeof(HU_AFFECT_LEX) / sizeof(HU_AFFECT_LEX[0]);

static float aff_clamp(float x, float lo, float hi) {
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

void hu_affect_init(hu_affect_state_t *s) {
    if (!s) {
        return;
    }
    s->valence = 0.f;
    s->arousal = 0.f;
    s->dominance = 0.5f;
    s->uncertainty = 1.f;
    s->modality = HU_AFFECT_TEXT;
    s->ts = 0;
}

static bool aff_word_match(const char *t, size_t len, size_t i, const char *needle) {
    size_t n = strlen(needle);
    if (i + n > len) {
        return false;
    }
    for (size_t j = 0; j < n; j++) {
        unsigned char a = (unsigned char)t[i + j];
        unsigned char b = (unsigned char)needle[j];
        if (tolower(a) != tolower(b)) {
            return false;
        }
    }
    /* Word boundary on either side. */
    if (i > 0 && (isalpha((unsigned char)t[i - 1]) || t[i - 1] == '\'')) {
        return false;
    }
    if (i + n < len && isalpha((unsigned char)t[i + n])) {
        return false;
    }
    return true;
}

hu_error_t hu_affect_estimate_text(const char *text, size_t len, hu_affect_state_t *out) {
    if (!out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_affect_init(out);
    out->modality = HU_AFFECT_TEXT;
    if (!text || len == 0) {
        return HU_OK;
    }

    /* Aggregate lexicon hits with simple averaging, plus arousal bump for
     * exclamation marks and ALL-CAPS spans. Negation flips valence on the
     * next matched lexicon entry. */
    int matches = 0;
    float v_sum = 0.f, a_sum = 0.f, d_sum = 0.f;
    bool negate_next = false;

    /* Find words. */
    size_t i = 0;
    while (i < len) {
        if (!isalpha((unsigned char)text[i])) {
            i++;
            continue;
        }
        size_t start = i;
        while (i < len && (isalpha((unsigned char)text[i]) || text[i] == '\'')) {
            i++;
        }
        size_t wlen = i - start;
        if (wlen == 0) {
            continue;
        }

        if (wlen == 3 && (strncasecmp(text + start, "not", 3) == 0)) {
            negate_next = true;
            continue;
        }
        if (wlen == 2 && (strncasecmp(text + start, "no", 2) == 0)) {
            negate_next = true;
            continue;
        }

        for (size_t k = 0; k < HU_AFFECT_LEX_LEN; k++) {
            if (aff_word_match(text, len, start, HU_AFFECT_LEX[k].word)) {
                float v = HU_AFFECT_LEX[k].valence;
                float a = HU_AFFECT_LEX[k].arousal;
                float d = HU_AFFECT_LEX[k].dominance;
                if (negate_next) {
                    v = -v;
                }
                v_sum += v;
                a_sum += a;
                d_sum += d;
                matches++;
                negate_next = false;
                break;
            }
        }
    }

    /* Punctuation cues. */
    int excl = 0, q = 0;
    int caps_run = 0, max_caps_run = 0;
    for (size_t j = 0; j < len; j++) {
        char c = text[j];
        if (c == '!') {
            excl++;
        } else if (c == '?') {
            q++;
        }
        if (isupper((unsigned char)c)) {
            caps_run++;
            if (caps_run > max_caps_run) {
                max_caps_run = caps_run;
            }
        } else if (isalpha((unsigned char)c)) {
            caps_run = 0;
        }
    }

    if (matches > 0) {
        out->valence = aff_clamp(v_sum / (float)matches, -1.f, 1.f);
        out->arousal = aff_clamp(a_sum / (float)matches, 0.f, 1.f);
        out->dominance = aff_clamp(d_sum / (float)matches, 0.f, 1.f);
        out->uncertainty = aff_clamp(0.6f - 0.05f * (float)matches, 0.1f, 0.9f);
    } else {
        out->valence = 0.f;
        out->arousal = 0.f;
        out->dominance = 0.5f;
        out->uncertainty = 0.85f;
    }

    /* Caps + exclamations bump arousal; questions reduce dominance. */
    out->arousal = aff_clamp(out->arousal + 0.10f * (float)excl, 0.f, 1.f);
    if (max_caps_run >= 4) {
        out->arousal = aff_clamp(out->arousal + 0.15f, 0.f, 1.f);
    }
    if (q > 0) {
        out->dominance = aff_clamp(out->dominance - 0.05f * (float)q, 0.f, 1.f);
    }

    return HU_OK;
}

hu_error_t hu_affect_decay(hu_affect_state_t *s, uint64_t now_ts, float half_life_s) {
    if (!s || half_life_s <= 0.f) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (s->ts == 0 || now_ts <= s->ts) {
        s->ts = now_ts;
        return HU_OK;
    }
    float dt = (float)(now_ts - s->ts);
    float k = expf(-0.6931472f * dt / half_life_s); /* ln(2)/half_life */
    s->valence *= k;
    s->arousal *= k;
    s->dominance = 0.5f + (s->dominance - 0.5f) * k;
    s->uncertainty = aff_clamp(s->uncertainty + (1.f - k) * 0.3f, 0.f, 1.f);
    s->ts = now_ts;
    return HU_OK;
}

hu_error_t hu_affect_fuse(const hu_affect_state_t *prior, const hu_affect_state_t *update,
                          hu_affect_state_t *out) {
    if (!prior || !update || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    float wp = 1.f - prior->uncertainty;
    float wu = 1.f - update->uncertainty;
    float total = wp + wu;
    if (total <= 0.0001f) {
        wp = wu = 0.5f;
        total = 1.f;
    }
    out->valence = (prior->valence * wp + update->valence * wu) / total;
    out->arousal = (prior->arousal * wp + update->arousal * wu) / total;
    out->dominance = (prior->dominance * wp + update->dominance * wu) / total;
    out->uncertainty = aff_clamp(prior->uncertainty * update->uncertainty, 0.f, 1.f);
    out->modality = HU_AFFECT_FUSED;
    out->ts = update->ts > prior->ts ? update->ts : prior->ts;
    return HU_OK;
}

bool hu_affect_is_distress(const hu_affect_state_t *s) {
    if (!s || s->uncertainty > 0.85f) {
        return false;
    }
    return s->valence < -0.4f && s->arousal > 0.5f;
}

int hu_affect_route_tier_score(const hu_affect_state_t *s) {
    if (!s) {
        return 0;
    }
    /* Confidence-weighted: high uncertainty → don't escalate purely on affect. */
    float confidence = 1.f - s->uncertainty;
    if (confidence < 0.25f) {
        return 0;
    }
    int score = 0;
    if (s->valence < 0.f) {
        score += (int)((-s->valence) * 5.f * confidence);
    }
    if (s->arousal > 0.5f) {
        score += (int)((s->arousal - 0.5f) * 8.f * confidence);
    }
    if (s->dominance < 0.3f) {
        score += 1;
    }
    if (hu_affect_is_distress(s)) {
        score += 2;
    }
    if (score < 0) {
        score = 0;
    }
    if (score > 10) {
        score = 10;
    }
    return score;
}
