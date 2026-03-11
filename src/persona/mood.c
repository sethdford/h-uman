/*
 * F60 — Mood Persistence Across Conversations.
 * Global mood state carrying across all contacts. Decays toward neutral.
 */
#include "human/persona/mood.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

static const char *MOOD_NAMES[] = {
    "neutral", "happy", "stressed", "tired", "energized", "irritable",
    "contemplative", "excited", "sad",
};

#ifdef HU_ENABLE_SQLITE

static float mood_decay_rate(hu_mood_enum_t mood) {
    switch (mood) {
    case HU_MOOD_STRESSED:
    case HU_MOOD_TIRED:
    case HU_MOOD_SAD:
    case HU_MOOD_IRRITABLE:
        return 0.15f;
    case HU_MOOD_HAPPY:
    case HU_MOOD_EXCITED:
    case HU_MOOD_ENERGIZED:
    case HU_MOOD_CONTEMPLATIVE:
        return 0.2f;
    default:
        return 0.2f;
    }
}

/* In-memory cache */
static hu_mood_state_t s_cached = {0};
static int64_t s_cache_set_at = 0;
static bool s_cache_valid = false;

static hu_mood_enum_t parse_mood(const char *name) {
    if (!name)
        return HU_MOOD_NEUTRAL;
    for (int i = 0; i < (int)HU_MOOD_COUNT; i++) {
        if (strcmp(name, MOOD_NAMES[i]) == 0)
            return (hu_mood_enum_t)i;
    }
    return HU_MOOD_NEUTRAL;
}

hu_error_t hu_mood_get_current(hu_allocator_t *alloc, hu_memory_t *memory, hu_mood_state_t *out) {
    (void)alloc;
    if (!memory || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    out->mood = HU_MOOD_NEUTRAL;
    out->intensity = 0.f;
    out->decay_rate = 0.2f;
    out->set_at = 0;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
#ifdef HU_IS_TEST
    (void)s_cache_valid;
    (void)s_cache_set_at;
    /* In tests, always fetch from DB so direct inserts are visible */
#else
    /* Use cache if valid and recent (within 1 min); still apply decay */
    if (s_cache_valid && s_cached.set_at > 0 && (now_ts - s_cache_set_at) < 60) {
        *out = s_cached;
        double hours = (double)(now_ts - out->set_at) / 3600.0;
        out->intensity *= (float)exp(-(double)out->decay_rate * hours);
        if (out->intensity < 0.1f) {
            out->mood = HU_MOOD_NEUTRAL;
            out->intensity = 0.f;
        }
        return HU_OK;
    }
#endif

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT mood, intensity, cause, set_at FROM mood_log "
                                "ORDER BY set_at DESC LIMIT 1",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        s_cache_valid = false;
        return HU_OK; /* No mood logged, neutral */
    }

    const char *mood_str = (const char *)sqlite3_column_text(stmt, 0);
    float intensity = (float)sqlite3_column_double(stmt, 1);
    const char *cause_str = (const char *)sqlite3_column_text(stmt, 2);
    int64_t set_at = sqlite3_column_int64(stmt, 3);

    hu_mood_enum_t mood = parse_mood(mood_str);
    char cause_buf[128] = {0};
    if (cause_str) {
        size_t clen = strlen(cause_str);
        if (clen >= sizeof(cause_buf))
            clen = sizeof(cause_buf) - 1;
        memcpy(cause_buf, cause_str, clen);
        cause_buf[clen] = '\0';
    }
    sqlite3_finalize(stmt);

    float decay_rate = mood_decay_rate(mood);
    double hours = (double)(now_ts - set_at) / 3600.0;
    intensity *= (float)exp(-(double)decay_rate * hours);

    if (intensity < 0.1f) {
        out->mood = HU_MOOD_NEUTRAL;
        out->intensity = 0.f;
        out->set_at = now_ts;
        s_cache_valid = true;
        s_cached = *out;
        s_cache_set_at = now_ts;
        return HU_OK;
    }

    out->mood = mood;
    out->intensity = intensity;
    out->decay_rate = decay_rate;
    out->set_at = set_at;
    memcpy(out->cause, cause_buf, sizeof(cause_buf));

    s_cache_valid = true;
    s_cached = *out;
    s_cache_set_at = now_ts;
    return HU_OK;
}

hu_error_t hu_mood_set(hu_allocator_t *alloc, hu_memory_t *memory, hu_mood_enum_t mood,
                       float intensity, const char *cause, size_t cause_len) {
    (void)alloc;
    if (!memory)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    int64_t now_ts = (int64_t)time(NULL);
    if (intensity < 0.f)
        intensity = 0.f;
    if (intensity > 1.f)
        intensity = 1.f;

    const char *mood_str = (mood >= 0 && mood < (int)HU_MOOD_COUNT) ? MOOD_NAMES[mood] : "neutral";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO mood_log(mood, intensity, cause, set_at) "
                                "VALUES(?, ?, ?, ?)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, mood_str, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, (double)intensity);
    if (cause && cause_len > 0) {
        size_t copy = cause_len;
        if (copy > 127)
            copy = 127;
        sqlite3_bind_text(stmt, 3, cause, (int)copy, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt, 3);
    }
    sqlite3_bind_int64(stmt, 4, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_MEMORY_BACKEND;

    /* Update cache */
    s_cached.mood = mood;
    s_cached.intensity = intensity;
    s_cached.decay_rate = mood_decay_rate(mood);
    s_cached.set_at = now_ts;
    s_cached.cause[0] = '\0';
    if (cause && cause_len > 0) {
        size_t copy = cause_len < 127 ? cause_len : 127;
        memcpy(s_cached.cause, cause, copy);
        s_cached.cause[copy] = '\0';
    }
    s_cache_valid = true;
    s_cache_set_at = now_ts;

    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_mood_get_current(hu_allocator_t *alloc, hu_memory_t *memory, hu_mood_state_t *out) {
    (void)alloc;
    (void)memory;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->mood = HU_MOOD_NEUTRAL;
    out->intensity = 0.f;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_mood_set(hu_allocator_t *alloc, hu_memory_t *memory, hu_mood_enum_t mood,
                       float intensity, const char *cause, size_t cause_len) {
    (void)alloc;
    (void)memory;
    (void)mood;
    (void)intensity;
    (void)cause;
    (void)cause_len;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */

char *hu_mood_build_directive(hu_allocator_t *alloc, const hu_mood_state_t *state,
                              size_t *out_len) {
    if (!alloc || !state || !out_len)
        return NULL;

    if (state->intensity < 0.2f)
        return NULL;

    const char *qualifier = "Slightly";
    if (state->intensity >= 0.7f)
        qualifier = "Quite";
    else if (state->intensity >= 0.5f)
        qualifier = "Moderately";

    const char *mood_name =
        (state->mood >= 0 && state->mood < (int)HU_MOOD_COUNT) ? MOOD_NAMES[state->mood]
                                                               : "neutral";

    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return NULL;

    int n;
    if (state->cause[0] != '\0') {
        n = snprintf(buf, cap,
                     "[CURRENT MOOD: %s %s (%s). Affects warmth and patience.]", qualifier,
                     mood_name, state->cause);
    } else {
        n = snprintf(buf, cap, "[CURRENT MOOD: %s %s. Affects warmth and patience.]", qualifier,
                     mood_name);
    }

    if (n <= 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }

    size_t need = (size_t)n + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, cap, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }
    *out_len = (size_t)n;
    return shrunk;
}
