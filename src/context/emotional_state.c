#ifdef HU_ENABLE_SQLITE

#include "human/context/emotional_state.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/emotional_moments.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Keyword-based emotion detection ─────────────────────────────── */

typedef struct {
    const char *keyword;
    const char *emotion;
    float intensity;
} hu_emotion_signal_t;

static const hu_emotion_signal_t EMOTION_SIGNALS[] = {
    {"i love you", "love", 0.95f},     {"i miss you", "longing", 0.85f},
    {"i'm sorry", "remorse", 0.7f},    {"i am sorry", "remorse", 0.7f},
    {"so sorry", "remorse", 0.7f},     {"worried about", "worry", 0.8f},
    {"i'm worried", "worry", 0.8f},    {"i am worried", "worry", 0.8f},
    {"scared", "fear", 0.8f},          {"afraid", "fear", 0.75f},
    {"angry", "anger", 0.8f},          {"furious", "anger", 0.9f},
    {"pissed", "anger", 0.85f},        {"frustrated", "frustration", 0.7f},
    {"sad", "sadness", 0.7f},          {"crying", "sadness", 0.85f},
    {"depressed", "sadness", 0.9f},    {"anxious", "anxiety", 0.75f},
    {"stressed", "stress", 0.7f},      {"overwhelmed", "stress", 0.8f},
    {"exhausted", "fatigue", 0.7f},    {"burned out", "fatigue", 0.85f},
    {"excited", "excitement", 0.75f},  {"so happy", "joy", 0.8f},
    {"thrilled", "excitement", 0.85f}, {"proud of", "pride", 0.75f},
    {"grateful", "gratitude", 0.7f},   {"thankful", "gratitude", 0.7f},
    {"lonely", "loneliness", 0.8f},    {"heartbroken", "grief", 0.9f},
    {"grieving", "grief", 0.9f},       {"lost someone", "grief", 0.95f},
    {"passed away", "grief", 0.95f},   {"diagnosis", "worry", 0.85f},
    {"hospital", "worry", 0.75f},      {"surgery", "worry", 0.8f},
    {"broke up", "heartbreak", 0.85f}, {"breakup", "heartbreak", 0.8f},
    {"fight with", "conflict", 0.7f},  {"arguing", "conflict", 0.7f},
};

#define EMOTION_SIGNAL_COUNT (sizeof(EMOTION_SIGNALS) / sizeof(EMOTION_SIGNALS[0]))

/* Case-insensitive substring search. */
static const char *hu_strcasestr_local(const char *haystack, size_t haystack_len,
                                       const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > haystack_len)
        return NULL;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        bool match = true;
        for (size_t j = 0; j < needle_len; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match)
            return haystack + i;
    }
    return NULL;
}

/* Classify the strongest emotional signal in the text. */
static bool detect_emotion(const char *text, size_t text_len, const char **emotion_out,
                           float *intensity_out, const char **topic_out) {
    if (!text || text_len == 0)
        return false;

    const char *best_emotion = NULL;
    float best_intensity = 0.0f;
    const char *best_pos = NULL;

    for (size_t i = 0; i < EMOTION_SIGNAL_COUNT; i++) {
        const char *found = hu_strcasestr_local(text, text_len, EMOTION_SIGNALS[i].keyword);
        if (found && EMOTION_SIGNALS[i].intensity > best_intensity) {
            best_emotion = EMOTION_SIGNALS[i].emotion;
            best_intensity = EMOTION_SIGNALS[i].intensity;
            best_pos = found;
        }
    }

    if (!best_emotion)
        return false;

    *emotion_out = best_emotion;
    *intensity_out = best_intensity;
    if (topic_out)
        *topic_out = best_pos;
    return true;
}

/* ── mood_log table management ───────────────────────────────────── */

static hu_error_t ensure_mood_log_table(sqlite3 *db) {
    static const char sql[] = "CREATE TABLE IF NOT EXISTS mood_log ("
                              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                              "contact_id TEXT NOT NULL,"
                              "mood TEXT NOT NULL,"
                              "intensity REAL NOT NULL,"
                              "context TEXT,"
                              "created_at INTEGER NOT NULL)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

/* ── Public API ──────────────────────────────────────────────────── */

hu_error_t hu_emotional_state_record(hu_allocator_t *alloc, hu_memory_t *memory,
                                     const char *contact_id, size_t contact_id_len,
                                     const char *text, size_t text_len) {
    if (!alloc || !memory || !contact_id || contact_id_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (!text || text_len == 0)
        return HU_OK;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    const char *emotion = NULL;
    float intensity = 0.0f;
    const char *topic_pos = NULL;
    if (!detect_emotion(text, text_len, &emotion, &intensity, &topic_pos))
        return HU_OK;

    /* P2-2 (2026-05-16): NEVER store a raw window of user text as topic.
     * The previous code copied 60 chars from around the keyword match, which
     * shipped substrings like "but boy I am just more lonely now" to family
     * contacts via F25/F30. Use the proper noun-phrase extractor, and if it
     * yields nothing, fall back to the emotion keyword via the SAFE
     * predicate. NEVER fall back to raw text. */
    (void)topic_pos;
    char context_buf[128];
    context_buf[0] = '\0';
    (void)hu_conversation_extract_topic(text, text_len, context_buf, sizeof(context_buf));

    char topic_buf[64];
    size_t topic_len =
        hu_emotional_moment_select_topic(context_buf, emotion, topic_buf, sizeof(topic_buf));
    if (topic_len == 0)
        return HU_OK;

    /* Write to emotional_moments table (existing infrastructure) */
    hu_emotional_moment_record(alloc, memory, contact_id, contact_id_len, topic_buf, topic_len,
                               emotion, strlen(emotion), intensity);

    /* Write to mood_log table */
    hu_error_t err = ensure_mood_log_table(db);
    if (err != HU_OK)
        return err;

    sqlite3_stmt *stmt = NULL;
    int rc =
        sqlite3_prepare_v2(db,
                           "INSERT INTO mood_log(contact_id,mood,intensity,context,created_at) "
                           "VALUES(?,?,?,?,?)",
                           -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    int64_t now_ts = (int64_t)time(NULL);
#ifdef HU_IS_TEST
    now_ts = 1700000000;
#endif

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, emotion, (int)strlen(emotion), SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, (double)intensity);
    sqlite3_bind_text(stmt, 4, context_buf[0] ? context_buf : NULL, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

/* Produce a human-readable time-ago label. */
static void time_ago_label(int64_t seconds_ago, char *buf, size_t cap) {
    if (seconds_ago < 60)
        snprintf(buf, cap, "just now");
    else if (seconds_ago < 3600)
        snprintf(buf, cap, "%d minutes ago", (int)(seconds_ago / 60));
    else if (seconds_ago < 7200)
        snprintf(buf, cap, "about an hour ago");
    else if (seconds_ago < 86400)
        snprintf(buf, cap, "%d hours ago", (int)(seconds_ago / 3600));
    else if (seconds_ago < 172800)
        snprintf(buf, cap, "yesterday");
    else
        snprintf(buf, cap, "%d days ago", (int)(seconds_ago / 86400));
}

hu_error_t hu_emotional_state_get_recent(hu_allocator_t *alloc, hu_memory_t *memory,
                                         const char *contact_id, size_t contact_id_len, char **out,
                                         size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (!memory || !contact_id || contact_id_len == 0)
        return HU_OK;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_OK;

    if (ensure_mood_log_table(db) != HU_OK)
        return HU_OK;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT mood, intensity, context, created_at FROM mood_log "
                                "WHERE contact_id=? ORDER BY created_at DESC LIMIT 5",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_OK;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);

    int64_t now_ts = (int64_t)time(NULL);
#ifdef HU_IS_TEST
    now_ts = 1700000000;
#endif

#define EMO_STATE_BUF_CAP 1024
    char buf[EMO_STATE_BUF_CAP];
    size_t count = 0;
    int64_t most_recent_ts = 0;
    char most_recent_mood[64] = {0};
    float most_recent_intensity = 0.0f;
    char most_recent_context[128] = {0};

    while (sqlite3_step(stmt) == SQLITE_ROW && count < 5) {
        const char *mood = (const char *)sqlite3_column_text(stmt, 0);
        double intensity = sqlite3_column_double(stmt, 1);
        const char *context = (const char *)sqlite3_column_text(stmt, 2);
        int64_t ts = sqlite3_column_int64(stmt, 3);

        if (!mood)
            continue;

        if (count == 0) {
            most_recent_ts = ts;
            size_t mlen = strlen(mood);
            if (mlen >= sizeof(most_recent_mood))
                mlen = sizeof(most_recent_mood) - 1;
            memcpy(most_recent_mood, mood, mlen);
            most_recent_mood[mlen] = '\0';
            most_recent_intensity = (float)intensity;
            if (context) {
                size_t clen = strlen(context);
                size_t copy =
                    clen < sizeof(most_recent_context) - 1 ? clen : sizeof(most_recent_context) - 1;
                memcpy(most_recent_context, context, copy);
                most_recent_context[copy] = '\0';
            }
        }
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0)
        return HU_OK;

    char time_label[64];
    time_ago_label(now_ts - most_recent_ts, time_label, sizeof(time_label));

    int n;
    if (most_recent_context[0] && most_recent_intensity >= 0.7f) {
        n = snprintf(buf, EMO_STATE_BUF_CAP,
                     "\n### Emotional Carry-over\n"
                     "Last conversation with this person (%s) ended on a %s note — "
                     "they were feeling %s. Context: \"%.80s\"\n",
                     time_label, most_recent_intensity >= 0.8f ? "heavy" : "notable",
                     most_recent_mood, most_recent_context);
    } else if (most_recent_intensity >= 0.7f) {
        n = snprintf(buf, EMO_STATE_BUF_CAP,
                     "\n### Emotional Carry-over\n"
                     "Last conversation with this person (%s) had a strong emotional tone — "
                     "they were feeling %s (intensity %.1f).\n",
                     time_label, most_recent_mood, most_recent_intensity);
    } else {
        n = snprintf(buf, EMO_STATE_BUF_CAP,
                     "\n### Emotional Carry-over\n"
                     "Last conversation with this person (%s) — "
                     "mood was %s (mild, intensity %.1f).\n",
                     time_label, most_recent_mood, most_recent_intensity);
    }

    if (n <= 0 || (size_t)n >= EMO_STATE_BUF_CAP)
        return HU_OK;

    size_t pos = (size_t)n;
#undef EMO_STATE_BUF_CAP

    char *result = (char *)alloc->alloc(alloc->ctx, pos + 1);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(result, buf, pos);
    result[pos] = '\0';
    *out = result;
    *out_len = pos;
    return HU_OK;
}

hu_error_t hu_emotional_state_get_seth_mood(hu_allocator_t *alloc, hu_memory_t *memory, char **out,
                                            size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (!memory)
        return HU_OK;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_OK;

    if (ensure_mood_log_table(db) != HU_OK)
        return HU_OK;

    int64_t now_ts = (int64_t)time(NULL);
#ifdef HU_IS_TEST
    now_ts = 1700000000;
#endif
    int64_t day_ago = now_ts - 86400;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT mood, intensity FROM mood_log "
                                "WHERE created_at>=? ORDER BY created_at DESC LIMIT 20",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_OK;

    sqlite3_bind_int64(stmt, 1, day_ago);

    size_t total = 0;
    size_t heavy = 0;
    size_t positive = 0;
    float sum_intensity = 0.0f;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *mood = (const char *)sqlite3_column_text(stmt, 0);
        double intensity = sqlite3_column_double(stmt, 1);
        total++;
        sum_intensity += (float)intensity;

        if (intensity >= 0.7)
            heavy++;

        if (mood) {
            if (strcmp(mood, "joy") == 0 || strcmp(mood, "excitement") == 0 ||
                strcmp(mood, "gratitude") == 0 || strcmp(mood, "pride") == 0 ||
                strcmp(mood, "love") == 0)
                positive++;
        }
    }
    sqlite3_finalize(stmt);

    if (total == 0)
        return HU_OK;

#define SETH_MOOD_BUF_CAP 256
    char buf[SETH_MOOD_BUF_CAP];
    int n;

    if (heavy >= 3) {
        n = snprintf(buf, SETH_MOOD_BUF_CAP,
                     "\n[Seth's day] Seth's had a tough day — %zu heavy conversations "
                     "in the last 24 hours. Be gentle.\n",
                     heavy);
    } else if (heavy >= 1) {
        n = snprintf(buf, SETH_MOOD_BUF_CAP,
                     "\n[Seth's day] Seth's had %zu heavy conversation%s today "
                     "alongside %zu lighter ones. Be aware of emotional load.\n",
                     heavy, heavy == 1 ? "" : "s", total - heavy);
    } else if (positive > total / 2) {
        n = snprintf(buf, SETH_MOOD_BUF_CAP,
                     "\n[Seth's day] Seth's been in good spirits today — "
                     "%zu conversations, mostly positive.\n",
                     total);
    } else {
        n = snprintf(buf, SETH_MOOD_BUF_CAP,
                     "\n[Seth's day] Seth's had %zu conversation%s today, "
                     "average emotional intensity %.1f.\n",
                     total, total == 1 ? "" : "s", total > 0 ? sum_intensity / (float)total : 0.0f);
    }
#undef SETH_MOOD_BUF_CAP

    if (n <= 0)
        return HU_OK;

    size_t pos = (size_t)n;
    char *result = (char *)alloc->alloc(alloc->ctx, pos + 1);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(result, buf, pos);
    result[pos] = '\0';
    *out = result;
    *out_len = pos;
    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

#include "human/context/emotional_state.h"

hu_error_t hu_emotional_state_record(hu_allocator_t *alloc, hu_memory_t *memory,
                                     const char *contact_id, size_t contact_id_len,
                                     const char *text, size_t text_len) {
    (void)alloc;
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    (void)text;
    (void)text_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_emotional_state_get_recent(hu_allocator_t *alloc, hu_memory_t *memory,
                                         const char *contact_id, size_t contact_id_len, char **out,
                                         size_t *out_len) {
    (void)alloc;
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_emotional_state_get_seth_mood(hu_allocator_t *alloc, hu_memory_t *memory, char **out,
                                            size_t *out_len) {
    (void)alloc;
    (void)memory;
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */
