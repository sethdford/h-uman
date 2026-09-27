/* Voice direction D2 (+ D3 in Task 4); see include/human/tts/speech_direction.h. */
#include "human/tts/speech_direction.h"
#include "human/tts/transcript_prep.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Cartesia Sonic's emotions, docs.cartesia.ai capability-guides/volume-speed-emotion
 * (read 2026-09-27). Best supported: neutral, calm, angry, content, sad, scared. */
static const char *const k_emotions[] = {
    "neutral",     "happy",        "excited",       "enthusiastic", "elated",       "euphoric",
    "triumphant",  "amazed",       "surprised",     "flirtatious",  "curious",      "content",
    "peaceful",    "serene",       "calm",          "grateful",     "affectionate", "trust",
    "sympathetic", "anticipation", "mysterious",    "angry",        "mad",          "outraged",
    "frustrated",  "agitated",     "threatened",    "disgusted",    "contempt",     "envious",
    "sarcastic",   "ironic",       "sad",           "dejected",     "melancholic",  "disappointed",
    "hurt",        "guilty",       "bored",         "tired",        "rejected",     "nostalgic",
    "wistful",     "apologetic",   "hesitant",      "insecure",     "confused",     "resigned",
    "anxious",     "panicked",     "alarmed",       "scared",       "proud",        "confident",
    "distant",     "skeptical",    "contemplative", "determined",
};
#define EMOTION_COUNT (sizeof(k_emotions) / sizeof(k_emotions[0]))

void hu_direction_default_limits(hu_direction_limits_t *o) {
    if (!o)
        return;
    o->speed_min = 0.85f;
    o->speed_max = 1.10f;
    o->volume_min = 0.85f;
    o->volume_max = 1.15f;
    o->break_max_ms = 800;
    o->max_emotion_changes = 3;
    o->max_laughs = 1;
    o->max_speed_tags = 1;
    o->max_volume_tags = 1;
}

bool hu_direction_emotion_valid(const char *s, size_t n) {
    for (size_t i = 0; s && i < EMOTION_COUNT; i++)
        if (strlen(k_emotions[i]) == n && strncasecmp(s, k_emotions[i], n) == 0)
            return true;
    return false;
}

size_t hu_direction_emotion_count(void) {
    return EMOTION_COUNT;
}

const char *hu_direction_emotion_at(size_t i) {
    return i < EMOTION_COUNT ? k_emotions[i] : NULL;
}

const char *hu_direction_verdict_name(hu_direction_verdict_t v) {
    switch (v) {
    case HU_DIRECTION_OK:
        return "ok";
    case HU_DIRECTION_EMPTY:
        return "empty";
    case HU_DIRECTION_BAD_TAG:
        return "bad_tag";
    case HU_DIRECTION_BAD_EMOTION:
        return "bad_emotion";
    case HU_DIRECTION_STAGE_DIRECTION:
        return "stage_direction";
    case HU_DIRECTION_EMOJI:
        return "emoji";
    case HU_DIRECTION_OVER_BUDGET:
        return "over_budget";
    case HU_DIRECTION_TOO_LONG:
        return "too_long";
    }
    return "unknown";
}

const char *hu_direction_first_emotion(const hu_direction_t *d) {
    for (size_t i = 0; d && i < d->count; i++)
        if (d->seg[i].emotion[0])
            return d->seg[i].emotion;
    return NULL;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* attr="value" inside a tag body. */
static bool tag_attr(const char *body, size_t n, const char *attr, char *val, size_t cap) {
    size_t al = strlen(attr);
    for (size_t i = 0; i + al + 2 <= n; i++) {
        if (strncmp(body + i, attr, al) != 0 || body[i + al] != '=' || body[i + al + 1] != '"')
            continue;
        size_t s = i + al + 2, e = s;
        while (e < n && body[e] != '"')
            e++;
        if (e >= n || e == s || e - s >= cap)
            return false;
        memcpy(val, body + s, e - s);
        val[e - s] = '\0';
        return true;
    }
    return false;
}

static bool parse_float(const char *v, float *out) {
    char *end = NULL;
    float f = strtof(v, &end);
    if (end == v || *end != '\0')
        return false;
    *out = f;
    return true;
}

static bool parse_ms(const char *v, uint16_t *out) {
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v || d < 0)
        return false;
    if (strcmp(end, "s") == 0)
        d *= 1000.0;
    else if (strcmp(end, "ms") != 0)
        return false;
    *out = (uint16_t)(d > 60000.0 ? 60000.0 : d);
    return true;
}

static bool is_markup(char c) {
    return c == '<' || c == '>' || c == '[' || c == ']' || c == '*' || c == '(' || c == ')';
}

static size_t count_sentences(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if ((s[i] == '.' || s[i] == '!' || s[i] == '?') &&
            (i + 1 == n || (s[i + 1] != '.' && s[i + 1] != '!' && s[i + 1] != '?')))
            c++;
    return c ? c : 1;
}

typedef struct {
    hu_direction_segment_t pend; /* tags waiting for their words */
    char last_emotion[24];
    unsigned emotions, laughs, speeds, volumes;
} dir_state_t;

/* One tag at line[i] == '<'. Returns the index past '>', or 0 with *v set. */
static size_t read_tag(const char *line, size_t len, size_t i, const hu_direction_limits_t *lim,
                       dir_state_t *st, hu_direction_verdict_t *v) {
    const char *gt = memchr(line + i, '>', len - i);
    const char *lt = i + 1 < len ? memchr(line + i + 1, '<', len - i - 1) : NULL;
    if (!gt || (lt && lt < gt)) {
        *v = HU_DIRECTION_BAD_TAG;
        return 0;
    }
    const char *body = line + i + 1;
    size_t bn = (size_t)(gt - body);
    if (bn > 0 && body[bn - 1] == '/')
        bn--;
    char val[32];
    float f = 0.f;
    if (bn > 8 && strncmp(body, "emotion ", 8) == 0 &&
        tag_attr(body, bn, "value", val, sizeof(val))) {
        for (char *p = val; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        if (!hu_direction_emotion_valid(val, strlen(val))) {
            *v = HU_DIRECTION_BAD_EMOTION;
            return 0;
        }
        if (strcmp(val, st->last_emotion) != 0) {
            if (st->last_emotion[0]) /* setting the opening emotion is not a change */
                st->emotions++;
            snprintf(st->pend.emotion, sizeof(st->pend.emotion), "%s", val);
            snprintf(st->last_emotion, sizeof(st->last_emotion), "%s", val);
        }
    } else if (bn > 6 && strncmp(body, "speed ", 6) == 0 &&
               tag_attr(body, bn, "ratio", val, sizeof(val)) && parse_float(val, &f)) {
        st->pend.speed = clampf(f, lim->speed_min, lim->speed_max);
        st->speeds++;
    } else if (bn > 7 && strncmp(body, "volume ", 7) == 0 &&
               tag_attr(body, bn, "ratio", val, sizeof(val)) && parse_float(val, &f)) {
        st->pend.volume = clampf(f, lim->volume_min, lim->volume_max);
        st->volumes++;
    } else if (bn > 6 && strncmp(body, "break ", 6) == 0 &&
               tag_attr(body, bn, "time", val, sizeof(val))) {
        uint16_t ms = 0;
        if (!parse_ms(val, &ms)) {
            *v = HU_DIRECTION_BAD_TAG;
            return 0;
        }
        st->pend.break_ms = ms > lim->break_max_ms ? lim->break_max_ms : ms;
    } else {
        *v = HU_DIRECTION_BAD_TAG;
        return 0;
    }
    return (size_t)(gt - line) + 1;
}

/* Trim each segment and join the words; returns false on overflow. */
static bool finish_words(hu_direction_t *d) {
    for (size_t k = 0; k < d->count; k++) {
        hu_direction_segment_t *g = &d->seg[k];
        size_t a = 0, b = g->text_len;
        while (a < b && isspace((unsigned char)g->text[a]))
            a++;
        while (b > a && isspace((unsigned char)g->text[b - 1]))
            b--;
        memmove(g->text, g->text + a, b - a);
        g->text_len = b - a;
        g->text[g->text_len] = '\0';
        if (g->text_len == 0)
            continue;
        if (d->words_len + g->text_len + 2 >= sizeof(d->words))
            return false;
        if (d->words_len > 0)
            d->words[d->words_len++] = ' ';
        memcpy(d->words + d->words_len, g->text, g->text_len);
        d->words_len += g->text_len;
        d->words[d->words_len] = '\0';
    }
    return true;
}

hu_direction_verdict_t hu_direction_parse(const char *line, size_t len,
                                          const hu_direction_limits_t *lim, hu_direction_t *d) {
    hu_direction_limits_t def;
    if (!lim) {
        hu_direction_default_limits(&def);
        lim = &def;
    }
    if (!d)
        return HU_DIRECTION_EMPTY;
    memset(d, 0, sizeof(*d));
    if (!line || len == 0)
        return HU_DIRECTION_EMPTY;
    dir_state_t st;
    memset(&st, 0, sizeof(st));
    bool pending = false;
    hu_direction_verdict_t v = HU_DIRECTION_OK;
    for (size_t i = 0; i < len;) {
        char c = line[i];
        if (c == '<') {
            i = read_tag(line, len, i, lim, &st, &v);
            if (!i)
                return v;
            pending = true;
            continue;
        }
        if (c == '[') {
            static const char laugh[] = "[laughter]";
            if (len - i >= sizeof(laugh) - 1 &&
                strncasecmp(line + i, laugh, sizeof(laugh) - 1) == 0) {
                st.pend.laugh = true;
                pending = true;
                st.laughs++;
                i += sizeof(laugh) - 1;
                continue;
            }
            return HU_DIRECTION_STAGE_DIRECTION;
        }
        if (is_markup(c))
            return c == '>' ? HU_DIRECTION_BAD_TAG : HU_DIRECTION_STAGE_DIRECTION;
        if ((unsigned char)c == 0xF0)
            return HU_DIRECTION_EMOJI;
        size_t s = i;
        while (i < len && !is_markup(line[i]) && (unsigned char)line[i] != 0xF0)
            i++;
        const char *t = line + s;
        size_t tn = i - s;
        bool blank = true;
        for (size_t k = 0; k < tn; k++)
            if (!isspace((unsigned char)t[k]))
                blank = false;
        if (blank && (pending || d->count == 0))
            continue; /* whitespace between tags */
        if (pending || d->count == 0) {
            if (d->count == HU_DIRECTION_MAX_SEGMENTS)
                return HU_DIRECTION_TOO_LONG;
            d->seg[d->count++] = st.pend;
            memset(&st.pend, 0, sizeof(st.pend));
            pending = false;
        }
        hu_direction_segment_t *g = &d->seg[d->count - 1];
        if (g->text_len + tn >= sizeof(g->text))
            return HU_DIRECTION_TOO_LONG;
        memcpy(g->text + g->text_len, t, tn);
        g->text_len += tn;
        g->text[g->text_len] = '\0';
    }
    if (!finish_words(d))
        return HU_DIRECTION_TOO_LONG;
    if (d->words_len == 0)
        return HU_DIRECTION_EMPTY;
    d->sentences = count_sentences(d->words, d->words_len);
    size_t emotion_cap = (d->sentences + 1) / 2;
    if (emotion_cap < 1)
        emotion_cap = 1;
    if (st.laughs > lim->max_laughs || st.speeds > lim->max_speed_tags ||
        st.volumes > lim->max_volume_tags || st.emotions > lim->max_emotion_changes ||
        st.emotions > emotion_cap)
        return HU_DIRECTION_OVER_BUDGET;
    return HU_DIRECTION_OK;
}

hu_laugh_style_t hu_laugh_style_parse(const char *s) {
    return s && strcmp(s, "text") == 0 ? HU_LAUGH_TEXT : HU_LAUGH_CARTESIA;
}

/* Append a formatted piece; false on overflow. */
static bool emit(char *out, size_t cap, size_t *o, const char *fmt, const char *a, double f) {
    int w = a ? snprintf(out + *o, cap - *o, fmt, a) : snprintf(out + *o, cap - *o, fmt, f);
    if (w < 0 || (size_t)w >= cap - *o)
        return false;
    *o += (size_t)w;
    return true;
}

size_t hu_direction_render(const hu_direction_t *d, hu_laugh_style_t laugh, char *out, size_t cap) {
    if (!d || !out || cap == 0)
        return 0;
    size_t o = 0;
    out[0] = '\0';
    for (size_t i = 0; i < d->count; i++) {
        const hu_direction_segment_t *g = &d->seg[i];
        char norm[HU_DIRECTION_TEXT_CAP * 2];
        size_t nn =
            hu_transcript_normalize_for_speech(g->text, g->text_len, norm, sizeof(norm), false);
        if (o > 0 && !emit(out, cap, &o, "%s", " ", 0))
            return 0;
        if (g->break_ms && !emit(out, cap, &o, "<break time=\"%.0fms\"/>", NULL, g->break_ms))
            return 0;
        if (g->laugh &&
            !emit(out, cap, &o, "%s",
                  laugh == HU_LAUGH_TEXT ? "haha, <break time=\"150ms\"/>" : "[laughter] ", 0))
            return 0;
        if (g->emotion[0] && !emit(out, cap, &o, "<emotion value=\"%s\"/>", g->emotion, 0))
            return 0;
        if (g->speed > 0.f && !emit(out, cap, &o, "<speed ratio=\"%.2f\"/>", NULL, g->speed))
            return 0;
        if (g->volume > 0.f && !emit(out, cap, &o, "<volume ratio=\"%.2f\"/>", NULL, g->volume))
            return 0;
        if (nn >= cap - o)
            return 0;
        memcpy(out + o, norm, nn);
        o += nn;
        out[o] = '\0';
    }
    return o;
}
