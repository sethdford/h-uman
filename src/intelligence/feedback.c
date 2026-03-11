#include "human/intelligence/feedback.h"
#include "human/core/error.h"
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

hu_feedback_class_t hu_feedback_classify(int64_t response_time_ms, size_t response_length_chars,
                                         bool has_emoji, bool has_tapback, bool topic_continued,
                                         bool has_laughter_words, bool topic_changed,
                                         bool ignored_question) {
    int score = 0;

    if (response_time_ms < 120000)
        score += 2;
    else if (response_time_ms > 7200000)
        score -= 2;

    if (response_length_chars > 50)
        score += 1;
    else if (response_length_chars < 10)
        score -= 1;

    if (has_emoji)
        score += 1;
    if (has_tapback)
        score += 2;
    if (topic_continued)
        score += 1;
    if (has_laughter_words)
        score += 1;

    if (topic_changed)
        score -= 1;
    if (ignored_question)
        score -= 2;

    if (score >= 2)
        return HU_FEEDBACK_CLASS_POSITIVE;
    if (score <= -2)
        return HU_FEEDBACK_CLASS_NEGATIVE;
    return HU_FEEDBACK_CLASS_NEUTRAL;
}

#ifdef HU_ENABLE_SQLITE
hu_error_t hu_feedback_record(sqlite3 *db, const char *behavior_type, size_t bt_len,
                             const char *contact_id, size_t cid_len, const char *signal,
                             size_t sig_len, const char *context, size_t ctx_len,
                             int64_t timestamp) {
    if (!db || !behavior_type || !contact_id || !signal)
        return HU_ERR_INVALID_ARGUMENT;

    const char *sql =
        "INSERT INTO behavioral_feedback (behavior_type, contact_id, signal, context, timestamp) "
        "VALUES (?1, ?2, ?3, ?4, ?5)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_STORE;

    sqlite3_bind_text(stmt, 1, behavior_type, (int)bt_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, contact_id, (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, signal, (int)sig_len, SQLITE_STATIC);
    if (context && ctx_len > 0)
        sqlite3_bind_text(stmt, 4, context, (int)ctx_len, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_int64(stmt, 5, timestamp);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_STORE;
    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */

const char *hu_feedback_class_str(hu_feedback_class_t signal) {
    switch (signal) {
    case HU_FEEDBACK_CLASS_POSITIVE:
        return "positive";
    case HU_FEEDBACK_CLASS_NEGATIVE:
        return "negative";
    case HU_FEEDBACK_CLASS_NEUTRAL:
        return "neutral";
    default:
        return "unknown";
    }
}
