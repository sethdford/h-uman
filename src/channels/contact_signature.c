/* src/channels/contact_signature.c
 *
 * Per-contact relationship signatures over chat.db. See the header for
 * the contract.
 *
 * Same shape as imessage_gaps.c: pure helpers always compile and are
 * unit-testable; SQL-backed paths are gated under
 *   !HU_IS_TEST && __APPLE__ && __MACH__ && HU_ENABLE_SQLITE
 * and stub out to HU_ERR_NOT_SUPPORTED on every other build variant
 * (matches the test-source gate-symmetry rule). */

#include "human/channels/contact_signature.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
#include "human/channels/imessage.h" /* hu_imessage_extract_attributed_body — modern macOS NULL text fallback */
#include <sqlite3.h>
#endif

#define HU_SIG_SECONDS_PER_DAY      86400LL
#define HU_SIG_DEFAULT_CONV_GAP_SEC (4 * 3600) /* 4-hour gap = new conversation */
#define HU_MAC_EPOCH_OFFSET         978307200LL

/* --------------------------- PURE HELPERS ---------------------------- */

hu_tod_bucket_t hu_tod_bucket_from_unix(int64_t ts_unix, int32_t tz_offset_seconds) {
    /* Translate to local-time seconds, then mod-into hour-of-day.
     * Negative values can fall out of the +tz_offset math; clamp with
     * Euclidean modulo so e.g. UTC 02:00 + PST(-28800) → 18:00 (EVENING)
     * the previous local day, not "negative-hour" garbage. */
    int64_t local_sec = ts_unix + (int64_t)tz_offset_seconds;
    int64_t hour = (local_sec / 3600) % 24;
    if (hour < 0)
        hour += 24;

    if (hour < 6)
        return HU_TOD_NIGHT;
    if (hour < 12)
        return HU_TOD_MORNING;
    if (hour < 18)
        return HU_TOD_AFTERNOON;
    return HU_TOD_EVENING;
}

int32_t hu_signature_median_latency(const int32_t *latencies_sorted, size_t count) {
    if (!latencies_sorted || count == 0)
        return -1;
    if (count == 1)
        return latencies_sorted[0];
    if ((count % 2) == 1) {
        return latencies_sorted[count / 2];
    }
    /* Even count: average of the two middle values, rounded toward zero
     * (int division). */
    int64_t a = latencies_sorted[count / 2 - 1];
    int64_t b = latencies_sorted[count / 2];
    return (int32_t)((a + b) / 2);
}

double hu_signature_initiation_ratio(const int64_t *message_timestamps_sorted,
                                     const bool *is_from_me, size_t count,
                                     int32_t gap_threshold_sec) {
    if (!message_timestamps_sorted || !is_from_me || count == 0)
        return 0.5;
    if (gap_threshold_sec <= 0)
        gap_threshold_sec = HU_SIG_DEFAULT_CONV_GAP_SEC;

    /* Walk the sorted timestamps; a gap >= threshold starts a new
     * conversation. The first message of each conversation is the
     * initiator. */
    size_t total_convs = 0;
    size_t my_initiations = 0;
    int64_t prev_ts = 0;
    bool in_conv = false;

    for (size_t i = 0; i < count; i++) {
        int64_t ts = message_timestamps_sorted[i];
        bool start_new = !in_conv || (ts - prev_ts) >= (int64_t)gap_threshold_sec;
        if (start_new) {
            total_convs++;
            if (is_from_me[i])
                my_initiations++;
            in_conv = true;
        }
        prev_ts = ts;
    }

    if (total_convs == 0)
        return 0.5;
    return (double)my_initiations / (double)total_convs;
}

/* --------------------------- SQL-BACKED ------------------------------ */

#if HU_IS_TEST || !defined(__APPLE__) || !defined(__MACH__) || !defined(HU_ENABLE_SQLITE)

hu_error_t hu_contact_signature_compute(const char *db_path, const char *contact_handle,
                                        int64_t now_unix, hu_contact_signature_t *out) {
    (void)db_path;
    (void)contact_handle;
    (void)now_unix;
    if (out)
        memset(out, 0, sizeof(*out));
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_contact_signature_top_n(const char *db_path, int64_t now_unix, size_t n,
                                      hu_contact_signature_t *out, size_t cap, size_t *out_n) {
    (void)db_path;
    (void)now_unix;
    (void)n;
    (void)out;
    (void)cap;
    if (out_n)
        *out_n = 0;
    return HU_ERR_NOT_SUPPORTED;
}

#else

/* Compute the local-tz offset once per process, lazily. We don't have a
 * device-wide canonical "user tz" facility shared by every module, but
 * we DO have hu_timezone_compute for hours-based representation. For
 * the SQL path we use system localtime via time.h: */
#include <time.h>
static int32_t hu_sig_local_tz_offset_seconds(int64_t sample_unix) {
    time_t t = (time_t)sample_unix;
    struct tm lt;
    struct tm ut;
    if (!localtime_r(&t, &lt))
        return 0;
    if (!gmtime_r(&t, &ut))
        return 0;
    /* Difference of broken-down times converted back to seconds. */
    int64_t local = ((int64_t)lt.tm_hour * 3600) + ((int64_t)lt.tm_min * 60) + lt.tm_sec;
    int64_t utc = ((int64_t)ut.tm_hour * 3600) + ((int64_t)ut.tm_min * 60) + ut.tm_sec;
    int32_t diff = (int32_t)(local - utc);
    /* Day boundary wrap: shift to closest direction. */
    if (diff > 12 * 3600)
        diff -= 24 * 3600;
    if (diff < -12 * 3600)
        diff += 24 * 3600;
    return diff;
}

static int hu_sig_cmp_i32(const void *a, const void *b) {
    int32_t ai = *(const int32_t *)a;
    int32_t bi = *(const int32_t *)b;
    return (ai > bi) - (ai < bi);
}

/* Load the message stream for a single contact: timestamp + is_from_me +
 * text length. Caller owns the returned arrays via free(). */
typedef struct {
    int64_t *timestamps;
    bool *is_from_me;
    int32_t *text_lens;
    size_t count;
} hu_sig_stream_t;

static hu_error_t load_contact_stream(sqlite3 *db, const char *contact_handle,
                                      hu_sig_stream_t *out) {
    /* Modern macOS (15+) stores message bodies in `attributedBody` (Apple
     * typedstream / NSKeyedArchiver), leaving `text` NULL. Fetching only
     * LENGTH(text) returned 0 for every modern message, which made
     * avg_msg_length always 0 and silently broke every downstream
     * consumer (predictive drafts, drift detection, signature reports).
     *
     * Strategy: fetch both columns. If text is non-empty, use its length.
     * Otherwise decode attributedBody to its plain-text form and use THAT
     * length — not the blob length, which includes typedstream framing
     * overhead and would dramatically inflate avg_msg_length.
     *
     * Decoded text is bounded by attr_text_buf (1024 bytes); messages
     * longer than that are clamped — acceptable because we only need
     * length statistics, not the content itself. */
    const char *sql = "SELECT m.date, m.is_from_me, m.text, m.attributedBody "
                      "FROM message m "
                      "JOIN handle h ON h.ROWID = m.handle_id "
                      "WHERE h.id = ? "
                      "  AND m.associated_message_type = 0 "
                      "ORDER BY m.date ASC";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, contact_handle, -1, SQLITE_STATIC);

    size_t cap = 256;
    out->timestamps = (int64_t *)calloc(cap, sizeof(int64_t));
    out->is_from_me = (bool *)calloc(cap, sizeof(bool));
    out->text_lens = (int32_t *)calloc(cap, sizeof(int32_t));
    out->count = 0;
    if (!out->timestamps || !out->is_from_me || !out->text_lens) {
        free(out->timestamps);
        free(out->is_from_me);
        free(out->text_lens);
        out->timestamps = NULL;
        out->is_from_me = NULL;
        out->text_lens = NULL;
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (out->count == cap) {
            size_t new_cap = cap * 2;
            int64_t *nt = (int64_t *)realloc(out->timestamps, new_cap * sizeof(int64_t));
            bool *nf = (bool *)realloc(out->is_from_me, new_cap * sizeof(bool));
            int32_t *nl = (int32_t *)realloc(out->text_lens, new_cap * sizeof(int32_t));
            if (!nt || !nf || !nl) {
                free(nt ? nt : out->timestamps);
                free(nf ? nf : out->is_from_me);
                free(nl ? nl : out->text_lens);
                out->timestamps = NULL;
                out->is_from_me = NULL;
                out->text_lens = NULL;
                sqlite3_finalize(st);
                return HU_ERR_OUT_OF_MEMORY;
            }
            out->timestamps = nt;
            out->is_from_me = nf;
            out->text_lens = nl;
            cap = new_cap;
        }
        int64_t mac_ns = sqlite3_column_int64(st, 0);
        int from_me = sqlite3_column_int(st, 1);
        const unsigned char *text = sqlite3_column_text(st, 2);
        int text_bytes = sqlite3_column_bytes(st, 2);
        int len = (text && text_bytes > 0) ? text_bytes : 0;
        if (len == 0) {
            /* Modern macOS: text is NULL, decode attributedBody. We only
             * need the LENGTH of the decoded text, not the text itself,
             * but the decoder writes both — extract into a small bounded
             * buffer and read its strlen. */
            const unsigned char *ab = sqlite3_column_blob(st, 3);
            int ab_len = sqlite3_column_bytes(st, 3);
            if (ab && ab_len > 0) {
                char attr_text_buf[1024];
                size_t extracted = hu_imessage_extract_attributed_body(
                    ab, (size_t)ab_len, attr_text_buf, sizeof(attr_text_buf));
                if (extracted > 0)
                    len = (int)extracted;
            }
        }
        out->timestamps[out->count] = (mac_ns / 1000000000LL) + HU_MAC_EPOCH_OFFSET;
        out->is_from_me[out->count] = (from_me != 0);
        out->text_lens[out->count] = len;
        out->count++;
    }
    sqlite3_finalize(st);
    return HU_OK;
}

static void free_stream(hu_sig_stream_t *s) {
    if (!s)
        return;
    free(s->timestamps);
    free(s->is_from_me);
    free(s->text_lens);
    s->timestamps = NULL;
    s->is_from_me = NULL;
    s->text_lens = NULL;
    s->count = 0;
}

static void aggregate_signature(const hu_sig_stream_t *stream, int64_t now_unix, int32_t tz_off,
                                const char *contact_handle, hu_contact_signature_t *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->contact_handle, contact_handle, sizeof(out->contact_handle) - 1);
    out->median_response_latency_sec = -1;
    out->initiation_ratio = 0.5;
    out->weekday_skew_pct = 50;

    if (!stream || stream->count == 0)
        return;

    out->total_messages = (uint32_t)stream->count;
    out->first_seen_unix = stream->timestamps[0];
    out->last_seen_unix = stream->timestamps[stream->count - 1];

    uint64_t len_sum = 0;
    uint32_t weekday = 0;
    int64_t last_day = -1;
    uint32_t active_days = 0;

    /* Pre-allocate latency array; at most one latency per their-reply
     * to your-message. */
    int32_t *lats = (int32_t *)calloc(stream->count, sizeof(int32_t));
    size_t lat_n = 0;

    for (size_t i = 0; i < stream->count; i++) {
        int64_t ts = stream->timestamps[i];
        bool fm = stream->is_from_me[i];
        len_sum += (uint64_t)stream->text_lens[i];

        if (fm)
            out->outbound_count++;
        else
            out->inbound_count++;

        if ((now_unix - ts) <= 30 * HU_SIG_SECONDS_PER_DAY)
            out->messages_last_30_days++;

        hu_tod_bucket_t b = hu_tod_bucket_from_unix(ts, tz_off);
        out->tod_distribution[b]++;

        /* Weekday detection via localtime. */
        time_t tt = (time_t)ts;
        struct tm lt;
        if (localtime_r(&tt, &lt)) {
            /* tm_wday: 0=Sun..6=Sat. Weekday = 1..5. */
            if (lt.tm_wday >= 1 && lt.tm_wday <= 5)
                weekday++;
        }

        int64_t day = (ts + tz_off) / HU_SIG_SECONDS_PER_DAY;
        if (day != last_day) {
            active_days++;
            last_day = day;
        }

        /* Response-latency: when message i is from me and message i+1 is
         * from them, capture delay. */
        if (lats && fm && (i + 1) < stream->count && !stream->is_from_me[i + 1]) {
            int64_t delay = stream->timestamps[i + 1] - ts;
            if (delay > 0 && delay < 7 * HU_SIG_SECONDS_PER_DAY) /* cap absurd outliers */
                lats[lat_n++] = (int32_t)delay;
        }
    }

    out->avg_message_length = (int32_t)(len_sum / stream->count);
    out->active_days = (int32_t)active_days;
    out->weekday_skew_pct = (int32_t)((uint64_t)weekday * 100 / (uint64_t)stream->count);

    if (lats && lat_n > 0) {
        qsort(lats, lat_n, sizeof(int32_t), hu_sig_cmp_i32);
        out->median_response_latency_sec = hu_signature_median_latency(lats, lat_n);
    }
    free(lats);

    out->initiation_ratio = hu_signature_initiation_ratio(
        stream->timestamps, stream->is_from_me, stream->count, HU_SIG_DEFAULT_CONV_GAP_SEC);
}

hu_error_t hu_contact_signature_compute(const char *db_path, const char *contact_handle,
                                        int64_t now_unix, hu_contact_signature_t *out) {
    if (!db_path || !contact_handle || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));
    out->median_response_latency_sec = -1;
    out->initiation_ratio = 0.5;
    out->weekday_skew_pct = 50;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    hu_sig_stream_t stream = {0};
    hu_error_t err = load_contact_stream(db, contact_handle, &stream);
    sqlite3_close(db);
    if (err != HU_OK) {
        free_stream(&stream);
        return err;
    }

    int32_t tz_off = hu_sig_local_tz_offset_seconds(now_unix);
    aggregate_signature(&stream, now_unix, tz_off, contact_handle, out);
    free_stream(&stream);
    return HU_OK;
}

hu_error_t hu_contact_signature_top_n(const char *db_path, int64_t now_unix, size_t n,
                                      hu_contact_signature_t *out, size_t cap, size_t *out_n) {
    if (!db_path || !out || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;
    if (n == 0 || cap == 0)
        return HU_OK;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    /* Pick top-N by total message count. */
    const char *sql = "SELECT h.id, COUNT(*) AS c "
                      "FROM message m JOIN handle h ON h.ROWID = m.handle_id "
                      "WHERE m.associated_message_type = 0 "
                      "GROUP BY h.id "
                      "ORDER BY c DESC "
                      "LIMIT ?";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }
    sqlite3_bind_int(st, 1, (int)(n < cap ? n : cap));

    char handles[64][HU_CONTACT_SIGNATURE_HANDLE_MAX];
    size_t handle_n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && handle_n < 64 && handle_n < cap && handle_n < n) {
        const unsigned char *h = sqlite3_column_text(st, 0);
        if (!h)
            continue;
        strncpy(handles[handle_n], (const char *)h, HU_CONTACT_SIGNATURE_HANDLE_MAX - 1);
        handles[handle_n][HU_CONTACT_SIGNATURE_HANDLE_MAX - 1] = '\0';
        handle_n++;
    }
    sqlite3_finalize(st);

    int32_t tz_off = hu_sig_local_tz_offset_seconds(now_unix);

    for (size_t i = 0; i < handle_n; i++) {
        hu_sig_stream_t stream = {0};
        hu_error_t err = load_contact_stream(db, handles[i], &stream);
        if (err != HU_OK) {
            free_stream(&stream);
            continue;
        }
        aggregate_signature(&stream, now_unix, tz_off, handles[i], &out[*out_n]);
        free_stream(&stream);
        (*out_n)++;
    }
    sqlite3_close(db);
    return HU_OK;
}

#endif /* !HU_IS_TEST && Apple && SQLITE */
