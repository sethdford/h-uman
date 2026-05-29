/* Phase 6 — F69 Humor Generation Principles + Humor Engine */
#include "human/cognition/humor.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---------- Helpers ---------- */

static bool str_eq_case_insensitive(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

static bool stage_is_deep(const char *relationship_stage) {
    if (!relationship_stage)
        return false;
    size_t len = strlen(relationship_stage);
    return str_eq_case_insensitive(relationship_stage, len, "deep", 4) ||
           str_eq_case_insensitive(relationship_stage, len, "intimate", 8);
}

static bool stage_is_acquaintance(const char *relationship_stage) {
    if (!relationship_stage)
        return true; /* default: treat unknown as acquaintance */
    size_t len = strlen(relationship_stage);
    return str_eq_case_insensitive(relationship_stage, len, "new", 3) ||
           str_eq_case_insensitive(relationship_stage, len, "acquaintance", 12);
}

/* ---------- Humor type names ---------- */

static const char *const humor_type_names[HU_HUMOR_TYPE_COUNT] = {
    "callback", "misdirection", "understatement", "self_deprecation", "wordplay", "observational"};

const char *hu_humor_type_name(hu_humor_type_t type) {
    if (type < 0 || type >= HU_HUMOR_TYPE_COUNT)
        return "unknown";
    return humor_type_names[type];
}

hu_humor_type_t hu_humor_type_from_name(const char *name) {
    if (!name)
        return HU_HUMOR_OBSERVATIONAL;
    size_t len = strlen(name);
    for (int i = 0; i < HU_HUMOR_TYPE_COUNT; i++) {
        if (str_eq_case_insensitive(name, len, humor_type_names[i], strlen(humor_type_names[i])))
            return (hu_humor_type_t)i;
    }
    return HU_HUMOR_OBSERVATIONAL;
}

/* ---------- Original persona directive builder ---------- */

static bool emotion_matches_never_during(const hu_humor_profile_t *humor, const char *emotion,
                                         size_t emotion_len) {
    if (!humor || !emotion || emotion_len == 0)
        return false;
    for (size_t i = 0; i < humor->never_during_count; i++) {
        size_t nd_len = strlen(humor->never_during[i]);
        if (str_eq_case_insensitive(emotion, emotion_len, humor->never_during[i], nd_len))
            return true;
    }
    return false;
}

static bool frequency_is_low(const hu_humor_profile_t *humor) {
    if (!humor || !humor->frequency)
        return false;
    size_t len = strlen(humor->frequency);
    return str_eq_case_insensitive(humor->frequency, len, "low", 3);
}

char *hu_humor_build_persona_directive(hu_allocator_t *alloc, const hu_humor_profile_t *humor,
                                       const char *dominant_emotion, size_t emotion_len,
                                       bool conversation_playful, size_t *out_len) {
    if (!alloc || !out_len)
        return NULL;
    *out_len = 0;

    if (!humor)
        return NULL;

    /* No meaningful humor config */
    if (humor->style_count == 0 && humor->signature_phrases_count == 0 &&
        humor->self_deprecation_count == 0)
        return NULL;

    /* Emotion matches never_during -> no humor */
    if (emotion_len > 0 && emotion_matches_never_during(humor, dominant_emotion, emotion_len))
        return NULL;

    /* Not playful and frequency low -> skip */
    if (!conversation_playful && frequency_is_low(humor))
        return NULL;

    /* Build directive */
    char buf[1024];
    size_t pos = 0;

    pos = hu_buf_appendf(buf, sizeof(buf), pos, "[HUMOR: Use ");
    if (humor->style_count > 0) {
        for (size_t i = 0; i < humor->style_count; i++) {
            if (i > 0)
                pos = hu_buf_appendf(buf, sizeof(buf), pos, ", ");
            pos = hu_buf_appendf(buf, sizeof(buf), pos, "%s", humor->style[i]);
        }
    } else {
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "subtle");
    }
    pos = hu_buf_appendf(buf, sizeof(buf), pos, ". ");

    if (humor->signature_phrases_count > 0) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "Signature phrases: ");
        for (size_t i = 0; i < humor->signature_phrases_count; i++) {
            if (i > 0)
                pos = hu_buf_appendf(buf, sizeof(buf), pos, ", ");
            pos = hu_buf_appendf(buf, sizeof(buf), pos, "%s", humor->signature_phrases[i]);
        }
        pos = hu_buf_appendf(buf, sizeof(buf), pos, ". ");
    }

    if (humor->self_deprecation_count > 0) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "Self-deprecate about: ");
        for (size_t i = 0; i < humor->self_deprecation_count; i++) {
            if (i > 0)
                pos = hu_buf_appendf(buf, sizeof(buf), pos, ", ");
            pos = hu_buf_appendf(buf, sizeof(buf), pos, "%s", humor->self_deprecation_topics[i]);
        }
        pos = hu_buf_appendf(buf, sizeof(buf), pos, ". ");
    }

    if (humor->never_during_count > 0) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "Never during: ");
        for (size_t i = 0; i < humor->never_during_count; i++) {
            if (i > 0)
                pos = hu_buf_appendf(buf, sizeof(buf), pos, ", ");
            pos = hu_buf_appendf(buf, sizeof(buf), pos, "%s", humor->never_during[i]);
        }
        pos = hu_buf_appendf(buf, sizeof(buf), pos, ". ");
    }

    pos = hu_buf_appendf(buf, sizeof(buf), pos, "Rule of three, misdirection when appropriate.]");
    if (pos >= sizeof(buf))
        pos = sizeof(buf) - 1;

    char *result = hu_strndup(alloc, buf, pos);
    if (result)
        *out_len = pos;
    return result;
}

/* ================================================================
 * Task 1: Audience model (SQLite-backed)
 * ================================================================ */

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_humor_audience_init(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql = "CREATE TABLE IF NOT EXISTS humor_preferences ("
                      "contact_id TEXT NOT NULL,"
                      "humor_type TEXT NOT NULL,"
                      "success_count INTEGER DEFAULT 0,"
                      "attempt_count INTEGER DEFAULT 0,"
                      "last_success INTEGER DEFAULT 0,"
                      "PRIMARY KEY (contact_id, humor_type))";
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err)
        sqlite3_free(err);
    return (rc == SQLITE_OK) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_humor_audience_record(sqlite3 *db, const char *contact_id, hu_humor_type_t type,
                                    bool success) {
    if (!db || !contact_id || type < 0 || type >= HU_HUMOR_TYPE_COUNT)
        return HU_ERR_INVALID_ARGUMENT;

    const char *type_name = hu_humor_type_name(type);

    /* Upsert: increment attempt_count always, success_count + last_success on success */
    const char *sql =
        "INSERT INTO humor_preferences (contact_id, humor_type, success_count, attempt_count, "
        "last_success) "
        "VALUES (?, ?, ?, 1, ?) "
        "ON CONFLICT(contact_id, humor_type) DO UPDATE SET "
        "attempt_count = attempt_count + 1, "
        "success_count = success_count + ?, "
        "last_success = CASE WHEN ? > last_success THEN ? ELSE last_success END";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    int success_inc = success ? 1 : 0;
#ifdef HU_IS_TEST
    int64_t now = 1000000; /* deterministic for tests */
#else
    int64_t now = (int64_t)time(NULL);
#endif
    int64_t ts = success ? now : 0;

    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, type_name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, success_inc);
    sqlite3_bind_int64(stmt, 4, ts);
    sqlite3_bind_int(stmt, 5, success_inc);
    sqlite3_bind_int64(stmt, 6, ts);
    sqlite3_bind_int64(stmt, 7, ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_humor_audience_load(sqlite3 *db, const char *contact_id, hu_humor_audience_t *out) {
    if (!db || !contact_id || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    const char *sql = "SELECT humor_type, success_count, attempt_count, last_success "
                      "FROM humor_preferences WHERE contact_id = ?";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *tn = (const char *)sqlite3_column_text(stmt, 0);
        hu_humor_type_t t = hu_humor_type_from_name(tn);
        if (t >= 0 && t < HU_HUMOR_TYPE_COUNT) {
            out->success_count[t] = sqlite3_column_int(stmt, 1);
            out->attempt_count[t] = sqlite3_column_int(stmt, 2);
            out->last_success[t] = sqlite3_column_int64(stmt, 3);
        }
    }

    sqlite3_finalize(stmt);
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */

hu_humor_type_t hu_humor_audience_preferred_type(const hu_humor_audience_t *audience) {
    if (!audience)
        return HU_HUMOR_OBSERVATIONAL;

    hu_humor_type_t best = HU_HUMOR_OBSERVATIONAL;
    int best_successes = 0;

    for (int i = 0; i < HU_HUMOR_TYPE_COUNT; i++) {
        if (audience->success_count[i] > best_successes) {
            best_successes = audience->success_count[i];
            best = (hu_humor_type_t)i;
        }
    }
    return best;
}

/* ================================================================
 * Task 2: Timing + appropriateness
 * ================================================================ */

hu_humor_timing_result_t hu_humor_check_timing(int hour, float emotional_valence,
                                               bool crisis_active, const char *relationship_stage) {
    hu_humor_timing_result_t r = {.allowed = true, .reason = {0}};

    if (crisis_active) {
        r.allowed = false;
        snprintf(r.reason, sizeof(r.reason), "Crisis active — humor suppressed");
        return r;
    }

    if (emotional_valence < -0.7f) {
        r.allowed = false;
        snprintf(r.reason, sizeof(r.reason), "Emotional distress (valence %.2f) — humor suppressed",
                 (double)emotional_valence);
        return r;
    }

    /* Late night (23-5) only allowed for deep relationships */
    if (hour >= 23 || hour < 5) {
        if (!stage_is_deep(relationship_stage)) {
            r.allowed = false;
            snprintf(r.reason, sizeof(r.reason), "Late night — humor requires deep relationship");
            return r;
        }
    }

    return r;
}

bool hu_humor_check_appropriate(hu_humor_type_t type, const char *topic,
                                const char *relationship_stage) {
    /* No dark/edgy humor with acquaintances */
    if (stage_is_acquaintance(relationship_stage)) {
        if (type == HU_HUMOR_SELF_DEPRECATION || type == HU_HUMOR_MISDIRECTION)
            return false;
    }

    /* No self-deprecation about sensitive topics */
    if (type == HU_HUMOR_SELF_DEPRECATION && topic) {
        size_t tlen = strlen(topic);
        static const char *const sensitive[] = {"health", "death",      "loss",
                                                "trauma", "disability", "mental"};
        for (size_t i = 0; i < sizeof(sensitive) / sizeof(sensitive[0]); i++) {
            if (hu_str_contains_ci(topic, tlen, sensitive[i], strlen(sensitive[i])))
                return false;
        }
    }

    /* No humor about grief or death topics regardless of type */
    if (topic) {
        size_t tlen = strlen(topic);
        if (hu_str_contains_ci(topic, tlen, "grief", 5) ||
            hu_str_contains_ci(topic, tlen, "death", 5) ||
            hu_str_contains_ci(topic, tlen, "funeral", 7))
            return false;
    }

    return true;
}

/* ================================================================
 * Task 3: Generation strategy
 * ================================================================ */

char *hu_humor_generate_strategy(hu_allocator_t *alloc, const hu_humor_audience_t *audience,
                                 const char *topic, float mood_valence,
                                 const char *relationship_stage, const hu_humor_profile_t *humor,
                                 size_t *out_len) {
    if (!alloc || !out_len)
        return NULL;
    *out_len = 0;

    /* Pick humor type based on audience, mood, relationship */
    hu_humor_type_t selected = HU_HUMOR_OBSERVATIONAL; /* safe default */

    /* If audience data exists, prefer their preferred type */
    if (audience) {
        hu_humor_type_t pref = hu_humor_audience_preferred_type(audience);
        /* Only use preference if it's appropriate for current context */
        if (hu_humor_check_appropriate(pref, topic, relationship_stage))
            selected = pref;
    }

    /* Mood-based overrides */
    if (mood_valence > 0.5f &&
        hu_humor_check_appropriate(HU_HUMOR_WORDPLAY, topic, relationship_stage)) {
        /* Happy mood: wordplay works well unless audience strongly prefers something else */
        if (!audience || audience->success_count[selected] < 2)
            selected = HU_HUMOR_WORDPLAY;
    } else if (mood_valence < -0.3f) {
        /* Low mood: understatement is safest */
        if (hu_humor_check_appropriate(HU_HUMOR_UNDERSTATEMENT, topic, relationship_stage))
            selected = HU_HUMOR_UNDERSTATEMENT;
    }

    /* Deep relationships unlock self-deprecation */
    if (stage_is_deep(relationship_stage) && audience &&
        audience->success_count[HU_HUMOR_SELF_DEPRECATION] > 2) {
        if (hu_humor_check_appropriate(HU_HUMOR_SELF_DEPRECATION, topic, relationship_stage))
            selected = HU_HUMOR_SELF_DEPRECATION;
    }

    /* Build directive string */
    char buf[512];
    size_t pos = 0;

    pos = hu_buf_appendf(buf, sizeof(buf), pos, "Use %s humor", hu_humor_type_name(selected));
    if (topic)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " about %s", topic);
    pos = hu_buf_appendf(buf, sizeof(buf), pos, ".");

    /* Add persona style if available */
    if (humor && humor->style_count > 0)
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " Style: %s.", humor->style[0]);

    /* Add audience preference note */
    if (audience) {
        hu_humor_type_t pref = hu_humor_audience_preferred_type(audience);
        pos = hu_buf_appendf(buf, sizeof(buf), pos, " Audience prefers %s.",
                             hu_humor_type_name(pref));
    }

    if (pos >= sizeof(buf))
        pos = sizeof(buf) - 1;

    char *result = hu_strndup(alloc, buf, pos);
    if (result)
        *out_len = pos;
    return result;
}

/* ================================================================
 * Task 4: Failed humor recovery
 * ================================================================ */

bool hu_humor_detect_failure(const char *user_response, size_t response_len) {
    if (!user_response || response_len == 0)
        return false;

    /* Very short response (1-3 chars) likely indicates discomfort */
    if (response_len <= 3)
        return true;

    /* Check for confusion/displeasure signals */
    static const char *const failure_signals[] = {"what?",  "huh?",      "i don't get it",
                                                  "anyway", "moving on", "let's talk about",
                                                  "ok...",  "um",        "uh"};
    for (size_t i = 0; i < sizeof(failure_signals) / sizeof(failure_signals[0]); i++) {
        if (hu_str_contains_ci(user_response, response_len, failure_signals[i],
                               strlen(failure_signals[i])))
            return true;
    }

    return false;
}

char *hu_humor_recover(hu_allocator_t *alloc, size_t *out_len) {
    if (!alloc || !out_len)
        return NULL;
    *out_len = 0;

    static const char directive[] =
        "Your humor attempt didn't land. Briefly acknowledge ('okay that was bad') or move on "
        "naturally. Do NOT explain the joke.";

    char *result = hu_strndup(alloc, directive, sizeof(directive) - 1);
    if (result)
        *out_len = sizeof(directive) - 1;
    return result;
}
