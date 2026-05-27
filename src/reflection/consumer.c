/* src/reflection/consumer.c — Reflection pattern consumer queries (T6).
 *
 * Spec: docs/plans/2026-05-26-reflection-loop/tasks.md Task 6 +
 *       design.md "Components → src/reflection/consumer.c".
 *
 * Four functions consume the storage layer from T2:
 *
 *   1. hu_reflection_query_for_system_prompt — channel-filtered slice
 *      for hu_personal_model_build_prompt to inject into the system
 *      prompt. Excludes retired + already-surfaced + low-confidence
 *      patterns. Cross-channel patterns (≥2 channels in channels_json)
 *      match every channel filter, scoping single-channel ones.
 *
 *   2. hu_reflection_query_unsurfaced — for init_proposer to use as a
 *      candidate source. Same filters but no channel restriction.
 *
 *   3. hu_reflection_mark_surfaced — UPSERT-friendly idempotent flag
 *      that prevents the same observation from being re-surfaced once
 *      the agent has already mentioned it.
 *
 *   4. hu_reflection_retire — sets retired=1 with timestamp. The
 *      storage UPSERT in T2 preserves the retired flag across new
 *      observations, so once a user contradicts a pattern, it stays
 *      retired even if the reflection model re-derives it later.
 *
 * All four use sqlite3_bind_text with SQLITE_STATIC per the project's
 * memory rule. Caller frees out_patterns arrays via free() (matches
 * hu_reflection_parse contract).
 *
 * JSON1 dependency: the channel filter uses json_each() on
 * channels_json. The query also checks json_array_length() > 1 as
 * the "cross-channel — applies to everyone" predicate; that's the
 * shape T7's system-prompt integration walks. */

#include "human/reflection.h"

#include "human/core/log.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── helpers ───────────────────────────────────────────────────── */

/* Map type string back to enum. Inverse of hu_reflection_pattern_type_str.
 * Returns HU_REFLECTION_PATTERN_PREFERENCE on unknown — defensive default.
 * The 6 known types match design.md's enum order; adding new types
 * requires keeping this mapping in sync. */
static hu_reflection_pattern_type_t type_from_str(const char *s) {
    if (!s)
        return HU_REFLECTION_PATTERN_PREFERENCE;
    if (strcmp(s, "topic_recurrence") == 0)
        return HU_REFLECTION_PATTERN_TOPIC_RECURRENCE;
    if (strcmp(s, "behavioral_shift") == 0)
        return HU_REFLECTION_PATTERN_BEHAVIORAL_SHIFT;
    if (strcmp(s, "preference") == 0)
        return HU_REFLECTION_PATTERN_PREFERENCE;
    if (strcmp(s, "emotional_state") == 0)
        return HU_REFLECTION_PATTERN_EMOTIONAL_STATE;
    if (strcmp(s, "schedule_pattern") == 0)
        return HU_REFLECTION_PATTERN_SCHEDULE_PATTERN;
    if (strcmp(s, "relationship") == 0)
        return HU_REFLECTION_PATTERN_RELATIONSHIP;
    return HU_REFLECTION_PATTERN_PREFERENCE;
}

/* Parse a JSON array of strings out of column N and copy up to `max_rows`
 * entries into a fixed-size [max_rows][item_len] buffer. Returns the
 * count actually copied (≤ max_rows). Truncates silently if the JSON
 * array has more entries — matches the parser's truncation policy. */
static int copy_json_array_column(sqlite3_stmt *st, int col, char (*dest)[64], int max_rows) {
    const unsigned char *json = sqlite3_column_text(st, col);
    if (!json || !*json)
        return 0;
    int count = 0;
    const char *p = (const char *)json;
    /* Walk past whitespace + '['. We don't do full JSON parsing — we
     * trust storage's own JSON encoder, which always emits '[' then
     * "..." entries separated by ','. */
    while (*p && (*p == ' ' || *p == '\t'))
        p++;
    if (*p != '[')
        return 0;
    p++;
    while (*p && count < max_rows) {
        while (*p && (*p == ' ' || *p == ',' || *p == '\t'))
            p++;
        if (*p == ']' || !*p)
            break;
        if (*p != '"')
            break; /* unexpected — bail */
        p++;       /* past opening quote */
        int len = 0;
        while (*p && *p != '"' && len < 63) {
            if (*p == '\\' && p[1]) {
                /* Trust the encoder — accept escape literally for now. */
                dest[count][len++] = p[1];
                p += 2;
            } else {
                dest[count][len++] = *p++;
            }
        }
        dest[count][len] = '\0';
        while (*p && *p != '"')
            p++;
        if (*p == '"')
            p++;
        count++;
    }
    return count;
}

/* Mirror for 8x32 channel buffer. C lets us reuse the 64-wide signature
 * via the pointer-to-array-of-32 cast, but keeping a separate function
 * is clearer than the casts. */
static int copy_json_array_column_32(sqlite3_stmt *st, int col, char (*dest)[32], int max_rows) {
    const unsigned char *json = sqlite3_column_text(st, col);
    if (!json || !*json)
        return 0;
    int count = 0;
    const char *p = (const char *)json;
    while (*p && (*p == ' ' || *p == '\t'))
        p++;
    if (*p != '[')
        return 0;
    p++;
    while (*p && count < max_rows) {
        while (*p && (*p == ' ' || *p == ',' || *p == '\t'))
            p++;
        if (*p == ']' || !*p)
            break;
        if (*p != '"')
            break;
        p++;
        int len = 0;
        while (*p && *p != '"' && len < 31) {
            if (*p == '\\' && p[1]) {
                dest[count][len++] = p[1];
                p += 2;
            } else {
                dest[count][len++] = *p++;
            }
        }
        dest[count][len] = '\0';
        while (*p && *p != '"')
            p++;
        if (*p == '"')
            p++;
        count++;
    }
    return count;
}

/* Hydrate one pattern row from the current sqlite3_stmt cursor. Caller
 * passes a pre-zeroed struct. Columns are in the order produced by the
 * SELECT statements below — DO NOT reorder columns without updating
 * both query strings and this hydrator. */
static void hydrate_pattern_from_row(sqlite3_stmt *st, hu_reflection_pattern_t *p) {
    /* 0: id  1: type  2: subject  3: observation  4: confidence
     * 5: evidence_json  6: channels_json
     * 7: created_at_ms  8: last_observed_at_ms  9: expires_at_ms
     * 10: surfaced_to_user  11: retired  12: retired_at_ms */
    const unsigned char *id = sqlite3_column_text(st, 0);
    if (id)
        snprintf(p->id, sizeof p->id, "%s", (const char *)id);
    const unsigned char *type_s = sqlite3_column_text(st, 1);
    p->type = type_from_str(type_s ? (const char *)type_s : NULL);
    const unsigned char *subj = sqlite3_column_text(st, 2);
    if (subj)
        snprintf(p->subject, sizeof p->subject, "%s", (const char *)subj);
    const unsigned char *obs = sqlite3_column_text(st, 3);
    if (obs)
        snprintf(p->observation, sizeof p->observation, "%s", (const char *)obs);
    p->confidence = sqlite3_column_double(st, 4);
    p->evidence_count = copy_json_array_column(st, 5, p->evidence_ids, 8);
    p->channel_count = copy_json_array_column_32(st, 6, p->channels, 8);
    p->created_at_ms = (uint64_t)sqlite3_column_int64(st, 7);
    p->last_observed_at_ms = (uint64_t)sqlite3_column_int64(st, 8);
    p->expires_at_ms = (uint64_t)sqlite3_column_int64(st, 9);
    p->surfaced_to_user = sqlite3_column_int(st, 10) != 0;
    p->retired = sqlite3_column_int(st, 11) != 0;
    p->retired_at_ms = (uint64_t)sqlite3_column_int64(st, 12);
}

/* ── query_for_system_prompt ───────────────────────────────────── */

hu_error_t hu_reflection_query_for_system_prompt(struct sqlite3 *db, const char *channel,
                                                 int max_patterns,
                                                 hu_reflection_pattern_t **out_patterns,
                                                 int *out_count) {
    if (!db || !channel || !out_patterns || !out_count || max_patterns <= 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_patterns = NULL;
    *out_count = 0;

    /* Channel match: pattern's channels_json contains `channel`, OR
     * the pattern is cross-channel (≥2 channels) — those count for
     * every channel filter since they're observed-everywhere traits.
     *
     * Ordering: confidence weighted by recency decay (1 / (1 + age_days)).
     * Single-channel high-confidence recent → highest. Cross-channel
     * stale low-confidence → lowest. */
    static const char *const k_sql =
        "SELECT id, type, subject, observation, confidence, evidence_json, channels_json, "
        "       created_at_ms, last_observed_at_ms, expires_at_ms, "
        "       surfaced_to_user, retired, retired_at_ms "
        "FROM reflection_patterns "
        "WHERE retired = 0 "
        "  AND surfaced_to_user = 0 "
        "  AND confidence > 0.7 "
        "  AND last_observed_at_ms > ? "
        "  AND (EXISTS(SELECT 1 FROM json_each(channels_json) WHERE value = ?) "
        "       OR json_array_length(channels_json) >= 2) "
        "ORDER BY confidence * (1.0 / (1.0 + (? - last_observed_at_ms) / 86400000.0)) DESC "
        "LIMIT ?";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, k_sql, -1, &st, NULL) != SQLITE_OK) {
        hu_log_error("reflection", NULL, "query_for_system_prompt prepare: %s",
                     sqlite3_errmsg(db));
        return HU_ERR_MEMORY_BACKEND;
    }

    /* 7-day recency window for the WHERE filter; the ORDER BY uses
     * full `now` for the decay. */
    uint64_t now_ms = (uint64_t)time(NULL) * 1000;
    uint64_t window_start_ms =
        now_ms > (uint64_t)(7L * 86400000L) ? now_ms - (uint64_t)(7L * 86400000L) : 0;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)window_start_ms);
    sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)now_ms);
    sqlite3_bind_int(st, 4, max_patterns);

    hu_reflection_pattern_t *arr =
        (hu_reflection_pattern_t *)calloc((size_t)max_patterns, sizeof(*arr));
    if (!arr) {
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < max_patterns) {
        hydrate_pattern_from_row(st, &arr[n]);
        n++;
    }
    sqlite3_finalize(st);

    if (n == 0) {
        free(arr);
        return HU_OK; /* No matching patterns — out_count=0, out_patterns=NULL */
    }
    *out_patterns = arr;
    *out_count = n;
    return HU_OK;
}

/* ── query_unsurfaced ──────────────────────────────────────────── */

hu_error_t hu_reflection_query_unsurfaced(struct sqlite3 *db, double min_confidence,
                                          hu_reflection_pattern_t **out_patterns, int *out_count) {
    if (!db || !out_patterns || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_patterns = NULL;
    *out_count = 0;

    /* Wider time window than the system-prompt query — init_proposer can
     * surface a pattern from up to 30 days ago. Caller passes
     * min_confidence; default-ish caller value should be 0.6 (lower bar
     * than 0.7 because init_proposer is about candidate generation, not
     * load-bearing context injection). Caps at 32 rows per the spec's
     * `out_patterns` array. */
    static const char *const k_sql =
        "SELECT id, type, subject, observation, confidence, evidence_json, channels_json, "
        "       created_at_ms, last_observed_at_ms, expires_at_ms, "
        "       surfaced_to_user, retired, retired_at_ms "
        "FROM reflection_patterns "
        "WHERE retired = 0 "
        "  AND surfaced_to_user = 0 "
        "  AND confidence >= ? "
        "  AND last_observed_at_ms > ? "
        "ORDER BY confidence DESC, last_observed_at_ms DESC "
        "LIMIT 32";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, k_sql, -1, &st, NULL) != SQLITE_OK) {
        hu_log_error("reflection", NULL, "query_unsurfaced prepare: %s", sqlite3_errmsg(db));
        return HU_ERR_MEMORY_BACKEND;
    }
    uint64_t now_ms = (uint64_t)time(NULL) * 1000;
    uint64_t window_start_ms =
        now_ms > (uint64_t)(30L * 86400000L) ? now_ms - (uint64_t)(30L * 86400000L) : 0;
    sqlite3_bind_double(st, 1, min_confidence);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)window_start_ms);

    hu_reflection_pattern_t *arr = (hu_reflection_pattern_t *)calloc(32, sizeof(*arr));
    if (!arr) {
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < 32) {
        hydrate_pattern_from_row(st, &arr[n]);
        n++;
    }
    sqlite3_finalize(st);

    if (n == 0) {
        free(arr);
        return HU_OK;
    }
    *out_patterns = arr;
    *out_count = n;
    return HU_OK;
}

/* ── mark_surfaced ─────────────────────────────────────────────── */

void hu_reflection_mark_surfaced(struct sqlite3 *db, const char *pattern_id) {
    if (!db || !pattern_id)
        return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "UPDATE reflection_patterns SET surfaced_to_user = 1 WHERE id = ?", -1,
                           &st, NULL) != SQLITE_OK) {
        hu_log_error("reflection", NULL, "mark_surfaced prepare: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(st, 1, pattern_id, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* ── retire ────────────────────────────────────────────────────── */

void hu_reflection_retire(struct sqlite3 *db, const char *pattern_id) {
    if (!db || !pattern_id)
        return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "UPDATE reflection_patterns SET retired = 1, retired_at_ms = ? "
                           "WHERE id = ?",
                           -1, &st, NULL) != SQLITE_OK) {
        hu_log_error("reflection", NULL, "retire prepare: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)((uint64_t)time(NULL) * 1000));
    sqlite3_bind_text(st, 2, pattern_id, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}
