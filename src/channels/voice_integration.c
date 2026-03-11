#include "human/channels/voice_integration.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HU_VOICE_ESCAPE_BUF 1024

/* ── SQL escape ───────────────────────────────────────────────────────────── */

static size_t escape_sql_string(const char *s, size_t len, char *out, size_t out_cap)
{
    size_t j = 0;
    for (size_t i = 0; i < len && s[i] != '\0'; i++) {
        if (s[i] == '\'') {
            if (j + 2 > out_cap)
                return 0;
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            if (j + 1 > out_cap)
                return 0;
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return j;
}

static bool strncasecmp_eq(const char *a, size_t a_len, const char *b, size_t b_len)
{
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static bool contains_word(const char *text, size_t text_len, const char *word, size_t word_len)
{
    if (word_len == 0 || text_len < word_len)
        return false;
    for (size_t i = 0; i <= text_len - word_len; i++) {
        if (strncasecmp_eq(text + i, word_len, word, word_len)) {
            bool start_ok = (i == 0) || !isalnum((unsigned char)text[i - 1]);
            bool end_ok = (i + word_len >= text_len) || !isalnum((unsigned char)text[i + word_len]);
            if (start_ok && end_ok)
                return true;
        }
    }
    return false;
}

/* ── F147-F149: Voice Understanding ────────────────────────────────────────── */

hu_error_t hu_voice_create_table_sql(char *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS voice_analyses (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    contact_id TEXT NOT NULL,\n"
        "    transcription TEXT NOT NULL,\n"
        "    emotion TEXT NOT NULL,\n"
        "    confidence REAL NOT NULL,\n"
        "    speech_rate REAL NOT NULL,\n"
        "    duration INTEGER NOT NULL,\n"
        "    timestamp INTEGER NOT NULL\n"
        ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_voice_insert_analysis_sql(const hu_voice_analysis_t *a, const char *contact_id,
                                         size_t cid_len, char *buf, size_t cap, size_t *out_len)
{
    if (!a || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    if (!contact_id || cid_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *trans = a->transcription ? a->transcription : "";
    size_t trans_len = a->transcription_len ? a->transcription_len : strlen(trans);
    const char *emotion = a->detected_emotion ? a->detected_emotion : "neutral";
    size_t emotion_len = a->detected_emotion_len ? a->detected_emotion_len : strlen(emotion);

    char contact_esc[HU_VOICE_ESCAPE_BUF];
    char trans_esc[HU_VOICE_ESCAPE_BUF];
    char emotion_esc[HU_VOICE_ESCAPE_BUF];

    size_t ce_len = escape_sql_string(contact_id, cid_len, contact_esc, sizeof(contact_esc));
    size_t te_len = escape_sql_string(trans, trans_len, trans_esc, sizeof(trans_esc));
    size_t em_len = escape_sql_string(emotion, emotion_len, emotion_esc, sizeof(emotion_esc));

    if (ce_len == 0 && cid_len > 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (te_len == 0 && trans_len > 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (em_len == 0 && emotion_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                     "INSERT INTO voice_analyses (contact_id, transcription, emotion, confidence, "
                     "speech_rate, duration, timestamp) VALUES ('%s', '%s', '%s', %f, %f, %u, "
                     "strftime('%%s','now'))",
                     contact_esc, trans_esc, emotion_esc, a->emotion_confidence, a->speech_rate,
                     a->duration_seconds);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_voice_detect_emotion_text(const char *text, size_t text_len, char *emotion_out,
                                         size_t emotion_cap, double *confidence)
{
    if (!text || !emotion_out || emotion_cap < 8 || !confidence)
        return HU_ERR_INVALID_ARGUMENT;

    *confidence = 0.5; /* default for keyword fallback */
    if (text_len == 0) {
        strncpy(emotion_out, "neutral", emotion_cap - 1);
        emotion_out[emotion_cap - 1] = '\0';
        return HU_OK;
    }

    /* happy: happy, excited, great, amazing, love, wonderful, fantastic */
    if (contains_word(text, text_len, "happy", 5) || contains_word(text, text_len, "excited", 7) ||
        contains_word(text, text_len, "great", 5) || contains_word(text, text_len, "amazing", 7) ||
        contains_word(text, text_len, "love", 4) || contains_word(text, text_len, "wonderful", 9) ||
        contains_word(text, text_len, "fantastic", 9)) {
        strncpy(emotion_out, "happy", emotion_cap - 1);
        emotion_out[emotion_cap - 1] = '\0';
        *confidence = 0.7;
        return HU_OK;
    }

    /* stressed: stressed, worried, anxious, overwhelmed, pressure */
    if (contains_word(text, text_len, "stressed", 8) || contains_word(text, text_len, "worried", 7) ||
        contains_word(text, text_len, "anxious", 7) ||
        contains_word(text, text_len, "overwhelmed", 11) ||
        contains_word(text, text_len, "pressure", 8)) {
        strncpy(emotion_out, "stressed", emotion_cap - 1);
        emotion_out[emotion_cap - 1] = '\0';
        *confidence = 0.7;
        return HU_OK;
    }

    /* sad: sad, disappointed, miss, lonely, depressed */
    if (contains_word(text, text_len, "sad", 3) ||
        contains_word(text, text_len, "disappointed", 11) || contains_word(text, text_len, "miss", 4) ||
        contains_word(text, text_len, "lonely", 6) || contains_word(text, text_len, "depressed", 9)) {
        strncpy(emotion_out, "sad", emotion_cap - 1);
        emotion_out[emotion_cap - 1] = '\0';
        *confidence = 0.7;
        return HU_OK;
    }

    /* angry: angry, mad, frustrated, furious */
    if (contains_word(text, text_len, "angry", 5) || contains_word(text, text_len, "mad", 3) ||
        contains_word(text, text_len, "frustrated", 10) || contains_word(text, text_len, "furious", 7)) {
        strncpy(emotion_out, "angry", emotion_cap - 1);
        emotion_out[emotion_cap - 1] = '\0';
        *confidence = 0.7;
        return HU_OK;
    }

    /* calm: calm, relaxed, peaceful, chill */
    if (contains_word(text, text_len, "calm", 4) || contains_word(text, text_len, "relaxed", 7) ||
        contains_word(text, text_len, "peaceful", 8) || contains_word(text, text_len, "chill", 5)) {
        strncpy(emotion_out, "calm", emotion_cap - 1);
        emotion_out[emotion_cap - 1] = '\0';
        *confidence = 0.7;
        return HU_OK;
    }

    strncpy(emotion_out, "neutral", emotion_cap - 1);
    emotion_out[emotion_cap - 1] = '\0';
    return HU_OK;
}

hu_error_t hu_voice_build_prompt(hu_allocator_t *alloc, const hu_voice_analysis_t *a, char **out,
                                  size_t *out_len)
{
    if (!alloc || !a || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *trans = a->transcription ? a->transcription : "";
    const char *emotion = a->detected_emotion ? a->detected_emotion : "neutral";

    /* Format: "[VOICE MESSAGE ANALYSIS]: They sound X (confidence: Y). Speaking fast (Z wpm).
     * Contains sighing. Transcript: [text]." */
    size_t cap = 512;
    char *buf = alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int pos = 0;
    pos += snprintf(buf + pos, (size_t)(cap - (size_t)pos),
                    "[VOICE MESSAGE ANALYSIS]: They sound %s (confidence: %.1f). ", emotion,
                    a->emotion_confidence);

    if (a->speech_rate > 0.0) {
        int n = snprintf(buf + pos, (size_t)(cap - (size_t)pos), "Speaking %s (%.0f wpm). ",
                        a->speech_rate > 150.0 ? "fast" : (a->speech_rate < 100.0 ? "slowly" : "at normal pace"),
                        a->speech_rate);
        if (n > 0)
            pos += n;
    }

    if (a->contains_sighing) {
        int n = snprintf(buf + pos, (size_t)(cap - (size_t)pos), "Contains sighing. ");
        if (n > 0)
            pos += n;
    }
    if (a->contains_laughter) {
        int n = snprintf(buf + pos, (size_t)(cap - (size_t)pos), "Contains laughter. ");
        if (n > 0)
            pos += n;
    }

    pos += snprintf(buf + pos, (size_t)(cap - (size_t)pos), "Transcript: [%s].", trans);

    *out = buf;
    *out_len = (size_t)pos;
    return HU_OK;
}

void hu_voice_analysis_deinit(hu_allocator_t *alloc, hu_voice_analysis_t *a)
{
    if (!alloc || !a)
        return;
    hu_str_free(alloc, a->transcription);
    hu_str_free(alloc, a->detected_emotion);
    a->transcription = NULL;
    a->detected_emotion = NULL;
    a->transcription_len = 0;
    a->detected_emotion_len = 0;
}

/* ── F169-F173: FaceTime/Phone Call Integration ────────────────────────────── */

hu_error_t hu_call_create_table_sql(char *buf, size_t cap, size_t *out_len)
{
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS call_events (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    type TEXT NOT NULL,\n"
        "    contact_id TEXT NOT NULL,\n"
        "    timestamp INTEGER NOT NULL,\n"
        "    duration INTEGER NOT NULL\n"
        ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_call_insert_event_sql(const hu_call_event_t *e, char *buf, size_t cap,
                                    size_t *out_len)
{
    if (!e || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    if (!e->contact_id || e->contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    const char *type_str = hu_call_event_type_str(e->type);
    char contact_esc[HU_VOICE_ESCAPE_BUF];
    size_t ce_len = escape_sql_string(e->contact_id, e->contact_id_len, contact_esc,
                                      sizeof(contact_esc));
    if (ce_len == 0 && e->contact_id_len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                    "INSERT INTO call_events (type, contact_id, timestamp, duration) "
                    "VALUES ('%s', '%s', %llu, %u)",
                    type_str, contact_esc, (unsigned long long)e->timestamp, e->duration_seconds);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_call_query_recent_sql(const char *contact_id, size_t len, uint32_t limit, char *buf,
                                    size_t cap, size_t *out_len)
{
    if (!contact_id || len == 0 || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_VOICE_ESCAPE_BUF];
    size_t ce_len = escape_sql_string(contact_id, len, contact_esc, sizeof(contact_esc));
    if (ce_len == 0 && len > 0)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                    "SELECT type, contact_id, timestamp, duration FROM call_events "
                    "WHERE contact_id = '%s' ORDER BY timestamp DESC LIMIT %u",
                    contact_esc, limit);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

bool hu_call_should_auto_text(const hu_call_event_t *e,
                               const hu_call_response_config_t *config)
{
    if (!e || !config)
        return false;
    if (e->type == HU_CALL_MISSED || e->type == HU_CALL_FACETIME_MISSED)
        return config->auto_text_on_missed;
    if (e->type == HU_CALL_DECLINED)
        return config->auto_text_on_declined;
    return false;
}

hu_error_t hu_call_build_text_prompt(hu_allocator_t *alloc, hu_call_event_type_t type,
                                     const char *contact_name, size_t name_len, char **out,
                                     size_t *out_len)
{
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *template = NULL;
    size_t template_len = 0;
    if (type == HU_CALL_MISSED || type == HU_CALL_FACETIME_MISSED) {
        template = "sorry I missed your call, what's up?";
        template_len = 32;
    } else if (type == HU_CALL_DECLINED) {
        template = "hey can't talk rn, what's up?";
        template_len = 28;
    } else {
        return HU_ERR_INVALID_ARGUMENT;
    }

    char *result = hu_strndup(alloc, template, template_len);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    *out = result;
    *out_len = template_len;
    (void)contact_name;
    (void)name_len;
    return HU_OK;
}

const char *hu_call_event_type_str(hu_call_event_type_t t)
{
    switch (t) {
    case HU_CALL_INCOMING:
        return "incoming";
    case HU_CALL_OUTGOING:
        return "outgoing";
    case HU_CALL_MISSED:
        return "missed";
    case HU_CALL_DECLINED:
        return "declined";
    case HU_CALL_FACETIME_INCOMING:
        return "facetime_incoming";
    case HU_CALL_FACETIME_MISSED:
        return "facetime_missed";
    default:
        return "unknown";
    }
}

void hu_call_event_deinit(hu_allocator_t *alloc, hu_call_event_t *e)
{
    if (!alloc || !e)
        return;
    hu_str_free(alloc, e->contact_id);
    e->contact_id = NULL;
    e->contact_id_len = 0;
}
